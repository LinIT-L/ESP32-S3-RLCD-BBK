#include "board_battery.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_attr.h"       /* EXT_RAM_BSS_ATTR: 诊断记录放 PSRAM */
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rtc_time.h"   /* esp_rtc_get_time_us(): 窗口"天"计时, 跨深睡继续走 */
#include "nvs_flash.h"      /* 一个月电池日档持久化 (跨天写一次) */
#include "driver/gpio.h"
#include "board_pins.h"     /* board_pins_get()->chrg: 充电中检测脚 */
#include "battery_model.h"  /* ★ 2026-10-31: 纯算法层 (滤波/曲线/稳定/充电识别) */

static const char *TAG = "board_battery";

/*
 * 原理图连接（自制板 2M/1M ÷3）：
 *   VBAT -> R1 2M -> BAT_ADC(GPIO4) -> R2 1M -> GND
 *
 * ADC 测 BAT_ADC 该点电压，等于 VBAT 的 1/3，软件乘以 3 得电池电压。
 * ⚠️ 官方板（微雪 RLCD-4.2）亦为 200K/100K ÷3，系数一致。
 *
 * ★★ 2026-10-05 小智模块板（分支 board/xiaozhi-s3-core）—— **脚位改成 GPIO3**：
 *   `ESP32-S3-AI-2E` 模块**内部**自带电池分压并接到 **GPIO3**（该脚**没有引出到焊盘**，
 *   所以规格书上看不到 —— 焊盘 15 那个位置被 `VOP`(功放输出正) 占了，但**芯片内部照常可用**）。
 *   好处：**外部不用再画分压**，直接从模块读。
 *   ⚠ GPIO3 = **`ADC1_CH2`**（ESP32-S3 的 ADC1 覆盖 GPIO1..10）。选 ADC1 是对的 ——
 *     **只有 ADC2 会和 WiFi 抢**，ADC1 在 WiFi 跑着时也能读。旧脚 GPIO4 是 ADC1_CH3，同理。
 *   ⇒ 顺带 **GPIO4 空出来了**（旧板仍用它，别动 main）。
 *   ❓ **待校准**：模块内部分压比未知。现暂按 ÷3（= 旧板系数）。上机后拿万用表量一次真实
 *     VBAT，和固件打印的 mv 对比一下：若固件读数 ≈ 真实值×(3/k)，就把下面的 NUM/DEN 改成 k。
 */
#define BOARD_BATTERY_ADC_UNIT        ADC_UNIT_1
#define BOARD_BATTERY_ADC_CHANNEL     ADC_CHANNEL_2   /* GPIO3 (模块内部电池分压) */

/* ★ 2026-10-16: 通道号按板型表下发 (小智=GPIO3/CH2, 微雪=GPIO4/CH3) */
static int s_bat_chan = ADC_CHANNEL_2;
void board_battery_set_channel(int chan) { if (chan >= 0) s_bat_chan = chan; }
#define BOARD_BATTERY_ADC_CH          2               /* 同上, 纯为日志打印 */
#define BOARD_BATTERY_ADC_GPIO_NAME   "GPIO3"         /* 同上, 纯为日志打印 */
#define BOARD_BATTERY_ADC_ATTEN       ADC_ATTEN_DB_12
#define BOARD_BATTERY_SAMPLE_COUNT    8

/* ★★ 分压比标定 (linit 2026-10-14 两点实测) ★★
 *   实测对照 (万用表 vs 固件, 同一时刻):
 *     ① 放电/待机:  固件 3.97V  万用表 4.12V  → 需要 ×(4.12/3.97) = ×3.113
 *     ② 充电中:     固件 4.04V  万用表 4.16V  → 需要 ×(4.16/4.04) = ×3.089
 *   两点都是"乘法性"偏差(比值 3.09~3.11), 取**折中 ×3.10** ⇒ 两点残差都在 ±0.4% 内
 *   (只按某一点标会把另一头拉偏 0.7%)。
 *   偏差原因(两者叠加):
 *     ① 200K/100K 的**阻值容差** → 实际比值不是刚好 3.000;
 *     ② 分压点戴维南阻抗 = 200K∥100K ≈ **66.7K**, 对 ESP32-S3 ADC 偏高(建议 <10~20K)
 *        ⇒ 采样电容充不满 ⇒ **ADC 系统性欠读**几个百分点。
 *   ⚠ 改板/改电阻后必须重新标定: 串口 `bat` 打印"脚上 mV", 拿万用表量同时刻 VBAT,
 *     k = 真实VBAT / 脚上mV, 把 NUM/DEN 写成 k 的整数比即可 (k 约 3.10 现板)。 */
#define BOARD_BATTERY_DIVIDER_NUM     3100    /* 旧板标定值; 现板用 `batcal <真实mV>` 现场实测覆盖 (存 NVS) */
#define BOARD_BATTERY_DIVIDER_DEN     1000

/* ★★ 2026-10-27 linit: 分压比改成**运行期可标定 + NVS 持久化** ★★
 *   原来写死宏 ⇒ 换板/换电阻/模块内部分压不同就得重编固件。
 *   现在串口一条命令即可标定:
 *       bat                      看 raw / 脚上 mV / 当前系数 / 换算 VBAT
 *       batcal 4255              拿外部仪表(如 INA226)读到的**真实 VBAT(mV)** 标定
 *                                内部用**同一时刻**的脚上 mV 反算 k = 真实mV / 脚上mV,
 *                                写 NVS 后立即生效, 重启不丢。
 *   ⚠ 必须在"电池电压稳定"时标 (充电中/大负载时 VBAT 本身在变, 标出来会偏)。 */
#define BAT_DIV_NVS_NS   "batcal"
#define BAT_DIV_NVS_KEY  "div"
#define BAT_DIV_MAGIC    0xBD1Fu

typedef struct { uint16_t num, den, magic; } bat_div_nvs_t;

static int s_div_num = BOARD_BATTERY_DIVIDER_NUM;
static int s_div_den = BOARD_BATTERY_DIVIDER_DEN;

static void bat_div_save(void)
{
    nvs_handle_t h;
    if (nvs_open(BAT_DIV_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    bat_div_nvs_t v = { (uint16_t)s_div_num, (uint16_t)s_div_den, BAT_DIV_MAGIC };
    if (nvs_set_blob(h, BAT_DIV_NVS_KEY, &v, sizeof(v)) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

static void bat_div_load(void)
{
    nvs_handle_t h;
    if (nvs_open(BAT_DIV_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    bat_div_nvs_t v;
    size_t len = sizeof(v);
    if (nvs_get_blob(h, BAT_DIV_NVS_KEY, &v, &len) == ESP_OK && len == sizeof(v) &&
        v.magic == BAT_DIV_MAGIC && v.num >= 1000 && v.num <= 9000 && v.den > 0) {
        s_div_num = v.num; s_div_den = v.den;
        ESP_LOGI(TAG, "分压比已用 NVS 标定值: x%d/%d", s_div_num, s_div_den);
    }
    nvs_close(h);
}

/*
 * 单节锂电池的粗略电量映射 (线性)。不是"电量计"级别，只给用户一个大概剩余量。
 *
 * ★★ 满量程 4100mV (linit 2026-10-14 定)：
 *   分压比标定后 (见上面的 DIVIDER_NUM/DEN) 读数已和万用表一致 —— 带载运行时满电平台约
 *   **4.10~4.15V** (TP4057 的 CV 是 4.2V, 但系统一直在耗电, 端电压到不了 4.2)。
 *   用 4.2V 当 100% 会**永远读不满**, 所以按"带载满电平台"取 4100; 空电 3300。
 *   ⛔ 标定分压比**之前**这里是 4200 —— 那是把"读数偏低 3.8%"错当成"电池没充满", 别改回去。
 */
#define BOARD_BATTERY_FULL_MV         4100
#define BOARD_BATTERY_EMPTY_MV        3300

/* ==== 满/空电压口径（固定，不做动态锚点）====
 * ★ 2026-10-31 linit「精简」: 删掉"最近 30 天电压峰值当满电基准"的动态锚点。
 *   理由（实测证据）: 锚点由**带载/充电时**的读数顶高（本次实测 4116mV），
 *   而静置满电只有 4.10~4.15V ⇒ 满电基准被抬高 ⇒ 平时读数系统性偏低，
 *   而且这个偏差会随"最近一个月充没充过电"漂移，用户完全无从判断。
 *   现在固定 4100/3300，口径唯一、可复现；老电芯容量衰减不再靠锚点遮掩。
 * ⚠ 代价: 老电芯可能永远显示不足 100%（那就是真实情况，不再"修饰"）。
 */
#define BOARD_BATTERY_FULL_MV         4100
#define BOARD_BATTERY_EMPTY_MV        3300

static adc_oneshot_unit_handle_t s_adc_handle = NULL;
static adc_cali_handle_t s_cali_handle = NULL;
static bool s_calibrated = false;
static bool s_initialized = false;

/* ★ 2026-10-31 精简: 只保留**充电脚去抖**这一个状态。
 *   原先是四个（9 点滤波 / 5 点峰窗 / SOC 稳定器 / 充电趋势），
 *   现在电压每轮独立算、电量直接查表，跨轮状态清零 ⇒ 休眠唤醒天然没有旧数据。 */
static bm_chrg_db_t s_chg;

/* 诊断快照 (remcon `bat` 用; 纯只读, 不参与控制) */
static struct {
    uint16_t raw_avg;     /* 本轮 8 次原始采样均值 (去极值后) */
    uint16_t raw_min, raw_max;
    uint16_t pin_mv;      /* ADC 校准后的脚上电压 */
    uint16_t vbat_mv;     /* ×分压比后的电池电压 */
    uint8_t  soc;         /* 查表电量 */
    uint8_t  flags;       /* 异常标记, 见 BATF_* */
    bool     charging;
} s_diag;

#define BATF_ADC_SAT    0x01   /* 采样贴 0 或 4095: 断线/短路/超量程 */
#define BATF_RANGE      0x02   /* 电压超出 2500~4500mV */
#define BATF_NOISY      0x04   /* 单轮波动 > 80mV: 分压阻抗高/干扰大 */
#define BATF_UNCAL      0x08   /* 未启用 IDF 校准 (走了 3.3V 线性回退) */

static void bat_hist_meta_load(void);   /* 前向声明: init 里先调用, 定义在下方 */

/* ==================== 真机测试记录 (batlog) ====================
 * 每次真采样记一条。**不参与计算**，关闭时开销只有一次判断。
 * ★ 2026-10-31 精简: 字段从 9 个减到 5 个（去掉了峰窗/锚点/两套 SOC 口径）。 */
#define BAT_TRACE_N BOARD_BATTERY_TRACE_MAX   /* 120 × 12B = 1440B; 60s 一条 ⇒ 2 小时 */
typedef struct {
    uint32_t t;        /* RTC 秒（跨深睡连续） */
    uint16_t mv;       /* 分压换算后的电池电压（对外报的就是它） */
    uint8_t  soc;      /* 查表得到的电量 */
    uint8_t  flags;    /* BATF_* 位 */
    uint8_t  chg;      /* 1=CHRG 低(充电中) 0=高(未充电) */
} bat_trace_t;

/* 放 PSRAM: sdkconfig 已开 CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY ⇒
 * 不会占内部 RAM（本项目内部 DRAM 紧张，诊断缓冲不该挤在那里）。 */
EXT_RAM_BSS_ATTR static bat_trace_t s_trace[BAT_TRACE_N];
static volatile bool s_trace_on = false;   /* volatile: remcon 任务写, 采样方(UI 任务)读 */
static uint8_t  s_trace_n  = 0;    /* 已存条数（<= BAT_TRACE_N） */
static uint8_t  s_trace_i  = 0;    /* 下一个写位置 */

static void bat_trace_push(uint16_t mv, uint8_t soc, uint8_t flags, uint8_t chg)
{
    if (!s_trace_on) return;       /* 关闭时唯一的开销就是这一句 */
    bat_trace_t *e = &s_trace[s_trace_i];
    e->t     = board_battery_clock_s();
    e->mv    = mv;
    e->soc   = soc;
    e->flags = flags;
    e->chg   = chg;
    s_trace_i = (uint8_t)((s_trace_i + 1) % BAT_TRACE_N);
    if (s_trace_n < BAT_TRACE_N) s_trace_n++;
}

static uint8_t board_battery_voltage_to_percent(uint32_t mv)
{
    /* 固定口径的查表（表在 battery_model.c，单节锂电通用曲线）。
     * ⚠ 只是**估算**，不是库仑计；温度/负载/电芯老化都会让它偏，
     *   见 battery_model.h 顶部的说明。 */
    if (mv > 0xFFFF) mv = 0xFFFF;
    return bm_soc_from_mv((uint16_t)mv);
}

static void board_battery_init_calibration(void)
{
    esp_err_t ret = ESP_FAIL;

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t curve_config = {
        .unit_id = BOARD_BATTERY_ADC_UNIT,
        .chan = s_bat_chan,
        .atten = BOARD_BATTERY_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ret = adc_cali_create_scheme_curve_fitting(&curve_config, &s_cali_handle);
    if (ret == ESP_OK) {
        s_calibrated = true;
        ESP_LOGI(TAG, "ADC calibration: curve fitting");
        return;
    }
#endif

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t line_config = {
        .unit_id = BOARD_BATTERY_ADC_UNIT,
        .atten = BOARD_BATTERY_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ret = adc_cali_create_scheme_line_fitting(&line_config, &s_cali_handle);
    if (ret == ESP_OK) {
        s_calibrated = true;
        ESP_LOGI(TAG, "ADC calibration: line fitting");
        return;
    }
#endif

    ESP_LOGD(TAG, "ADC calibration unavailable, battery voltage will use raw estimate");
}

/* ---------------- 充电中检测 (LGS4056HDA CHRG) ----------------
 * CHRG 是**开漏低有效**: 充电中拉低, 不充电时高阻 ⇒ 必须上拉才能读。
 * 内部上拉(≈45k)就够用; 外部再加 100k 到 3V3 更稳(⛔ 不能上拉到 VBUS)。
 * 引脚来自板型表 `.chrg` (-1 = 本板没接), 所以这里按需懒初始化,
 * 避免在"板型还没识别完"的时候就把脚配错。 */
static int s_chrg_pin = -2;        /* -2 = 还没解析过 */

static void chrg_ensure(void) {
    if (s_chrg_pin != -2) return;
    const board_pins_t *bp = board_pins_get();
    s_chrg_pin = bp ? bp->chrg : -1;
    if (s_chrg_pin >= 0) {
        gpio_config_t io = {
            .pin_bit_mask = (1ULL << s_chrg_pin),
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        gpio_config(&io);
        ESP_LOGI(TAG, "CHRG 检测脚 = GPIO%d (开漏低有效: 低=充电中)", s_chrg_pin);
    } else {
        ESP_LOGI(TAG, "CHRG 检测脚: 本板未接 (充电状态只能靠 VBUS 推断)");
    }
}

int  board_battery_chrg_pin(void) { chrg_ensure(); return s_chrg_pin; }
bool board_battery_charging(void) {
    chrg_ensure();
    if (s_chrg_pin < 0) return false;              /* 没接 → 不知道, 一律报"未充电" */
    return gpio_get_level((gpio_num_t)s_chrg_pin) == 0;
}

/* 轮询版: 500ms 缓存一次, 状态翻转时打一行日志。
 * 上层(状态栏/管家/省电)按 1Hz 调这个就够 —— CHRG 是慢信号, 没必要每帧读。
 * ⚠ 共用 GPIO18 时(GPS EXTINT 是输入, 不主动驱动), 本函数只读不配, 不会和 GPS 抢线。 */
bool board_battery_charging_cached(void) {
    static bool     s_val = false;
    static int64_t  s_t   = 0;
    static bool     s_init = false;
    int64_t now = esp_timer_get_time();
    if (!s_init || (now - s_t) > 500000) {
        bool v = board_battery_charging();
        if (!s_init || v != s_val) {
            ESP_LOGI(TAG, "充电状态: %s", v ? "充电中" : "未充电");
        }
        s_val = v; s_t = now; s_init = true;
    }
    return s_val;
}

esp_err_t board_battery_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    adc_oneshot_unit_init_cfg_t unit_config = {
        .unit_id = BOARD_BATTERY_ADC_UNIT,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit_config, &s_adc_handle), TAG, "create adc unit failed");

    adc_oneshot_chan_cfg_t channel_config = {
        .atten = BOARD_BATTERY_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc_handle, s_bat_chan, &channel_config),
                        TAG,
                        "config battery adc channel failed");

    board_battery_init_calibration();
    bat_div_load();        /* ★ NVS 里若有标定过的分压比, 覆盖默认值 */
    bat_hist_meta_load();  /* 电压历史元数据 (数组懒加载, 不拖慢开机) */

    s_initialized = true;
    ESP_LOGI(TAG, "Battery ADC initialized: %s ADC1_CH%d, divider x%d/%d, 口径 %u..%u mV, 校准=%s",
             BOARD_BATTERY_ADC_GPIO_NAME, BOARD_BATTERY_ADC_CH,
             s_div_num, s_div_den,
             BOARD_BATTERY_EMPTY_MV, BOARD_BATTERY_FULL_MV,
             s_calibrated ? "IDF curve-fitting" : "无(3.3V 线性回退)");

    /* ★ 2026-10-05 一次性自检打印 —— 用来**校准分压比**。
     * 小智模块的电池分压做在**模块内部**(接 GPIO3)，阻值未知；拿万用表量一下真实 VBAT，
     * 和下面 "脚上" 那行对比即可算出真实分压比:
     *     真实分压比 k = 真实 VBAT(mV) / 脚上电压(mV)
     * 然后把上面的 DIVIDER_NUM/DEN 改成 k 即可 (例: k≈2 → NUM=2, DEN=1)。 */
    {
        int raw0 = 0;
        if (adc_oneshot_read(s_adc_handle, s_bat_chan, &raw0) == ESP_OK) {
            int pin_mv = -1;
            if (s_calibrated && s_cali_handle) {
                adc_cali_raw_to_voltage(s_cali_handle, raw0, &pin_mv);
            }
            ESP_LOGI(TAG, "电池自检: raw=%d 脚上=%d mV  ⇒ 按现系数(%d/%d)算出 VBAT≈%d mV"
                          "   [拿万用表量真实 VBAT 对比即可校准]",
                     raw0, pin_mv,
                     s_div_num, s_div_den,
                     pin_mv > 0 ? pin_mv * s_div_num / s_div_den : -1);
        }
    }
    return ESP_OK;
}

esp_err_t board_battery_read(board_battery_status_t *status)
{
    ESP_RETURN_ON_FALSE(status != NULL, ESP_ERR_INVALID_ARG, TAG, "status is null");
#ifdef LINTOS_EMU_MODE
    /* ★ 2026-10-30: esp-emu **没有 ADC 模型** —— adc_oneshot_read 会停在
     *   adc_oneshot_ll_get_event 里死等"转换完成"事件，表现为
     *   `Guru Meditation: Interrupt wdt timeout on CPU0`（实测 1144ms 崩溃重启，
     *   无限循环）。ESP_ERR_TIMEOUT 也等不到，因为事件位永远不会置起来。
     *   模拟器里给一个固定电压：状态栏电量图标/低电量逻辑照常走通。
     *   ⚠ 只在 -DLINTOS_EMU_MODE=1 时编入，真机固件不受影响。 */
    status->voltage_mv = 4100;
    status->percent = 80;
    return ESP_OK;
#endif
    ESP_RETURN_ON_ERROR(board_battery_init(), TAG, "init battery adc failed");

    /* ★★ 2026-10-31: 采样改法 (原来 8 次直接平均，单点尖峰能拉偏 1/8)
     *   ① **丢弃首个转换** —— oneshot 切通道后的第一次常带残留/未稳；
     *   ② 采 N 个存下来，排序后**丢最大 1 / 最小 1** 再平均；
     *   ③ 顺带记下本轮 min/max，供诊断判断"是噪声还是真压降"。 */
    {
        int discard = 0;
        adc_oneshot_read(s_adc_handle, s_bat_chan, &discard);   /* 失败也不影响后续 */
    }

    int raws[BOARD_BATTERY_SAMPLE_COUNT];
    int rmin = 0x7FFF, rmax = 0;
    for (int i = 0; i < BOARD_BATTERY_SAMPLE_COUNT; i++) {
        int raw = 0;
        ESP_RETURN_ON_ERROR(adc_oneshot_read(s_adc_handle, s_bat_chan, &raw),
                            TAG,
                            "read battery adc failed");
        raws[i] = raw;
        if (raw < rmin) rmin = raw;
        if (raw > rmax) rmax = raw;
    }
    /* 去极值平均移到 battery_model.c（纯函数，主机可测"异常值"场景） */
    int raw_avg = bm_despeckle_avg(raws, BOARD_BATTERY_SAMPLE_COUNT);
    int adc_mv = 0;

    if (s_calibrated) {
        ESP_RETURN_ON_ERROR(adc_cali_raw_to_voltage(s_cali_handle, raw_avg, &adc_mv),
                            TAG,
                            "convert battery adc voltage failed");
    } else {
        adc_mv = (raw_avg * 3300) / 4095;
    }

    uint32_t battery_mv = ((uint32_t)adc_mv * (uint32_t)s_div_num) / (uint32_t)s_div_den;
    status->voltage_mv = battery_mv;
    status->percent = bm_soc_from_mv((uint16_t)battery_mv);   /* 曲线直出, 未平滑 */

    /* ---- 诊断快照 + 异常标记 ---- */
    s_diag.raw_avg  = (uint16_t)raw_avg;
    s_diag.raw_min  = (uint16_t)rmin;
    s_diag.raw_max  = (uint16_t)rmax;
    s_diag.pin_mv   = (uint16_t)(adc_mv > 0 ? adc_mv : 0);
    s_diag.vbat_mv  = (uint16_t)(battery_mv > 0xFFFF ? 0xFFFF : battery_mv);
    s_diag.soc      = status->percent;
    s_diag.flags    = 0;
    if (raw_avg <= 0 || raw_avg >= 4095)                 s_diag.flags |= BATF_ADC_SAT;
    if (battery_mv < 2500 || battery_mv > 4500)          s_diag.flags |= BATF_RANGE;
    if ((rmax - rmin) * 3 > 240)                         s_diag.flags |= BATF_NOISY;  /* 脚上 >80mV ⇒ 电池 >240mV */
    if (!s_calibrated)                                   s_diag.flags |= BATF_UNCAL;

    return ESP_OK;
}

/* ---- 统一 60s 缓存采样 (linit 2026-09-20: 全系统只打这一个节拍器) ----
 * 之前状态栏(60s)和低电量告警(30s)各自持定时器重复打 ADC, 现合并:
 * 所有消费方都走 board_battery_read_cached(), ADC 至多 60s 采样一次. */
#define BOARD_BATTERY_CACHE_MS 60000
static board_battery_status_t s_cache;
static bool     s_cache_valid = false;
static uint32_t s_cache_ms    = 0;

/* ==== 电压历史: **1 字节/分钟**, 7 天 = 10KB (linit: "最长就保存七天") ====
 * 为什么能这么小: ① 电压只存 10mV 分辨率(1 字节, 3.00~5.54V, 254 档够画图);
 * ② **时间不存** —— 均匀每分钟一格, 只存基准时间, 第 i 格 = 基准 + i×60;
 * ③ 睡眠/关机没采的分钟 = 哨兵 0xFF("无数据"), 画图时跨过去连上。
 * 存储: 运行期数组在 **PSRAM**(10KB), 另按"天"分块持久化到 **NVS**(ns batcal, 7 个 1440B 块)。 */
#define BAT_MIN_PER_DAY  1440
#define BAT_KEEP_DAYS    7
#define BAT_SLOTS        (BAT_MIN_PER_DAY * BAT_KEEP_DAYS)   /* 10080 = 7 天 */
#define BAT_NODATA       0xFFu
#define BAT_MV_BASE      3000u        /* 编码 (mv-3000)/10 → 0..254 */
#define BAT_FLUSH_S      3600u        /* 落盘节拍 (只重写"当前这一天"那块)
                                       * ★ 2026-10-26 linit「避免频繁写 NVS」: 600s(10分钟) → 3600s(1小时)。
                                       *   代价说明 (按实情, 不是磨损):
                                       *   ① 写 NVS 期间 **flash cache 被关**, 从 flash 执行的代码会停顿十几~
                                       *      几十 ms ⇒ 界面掉帧、音频可能抖一下。10 分钟一次 = 每小时 6 次停顿。
                                       *   ② 磨损其实不是主因: NVS 分区 256KB(64 页) 会轮换使用, 144 次/天 摊到
                                       *      64 页 ≈ 每页 2 天一次擦除, 远低于 10 万次寿命。
                                       *   改成 1 小时: 停顿从 6 次/小时降到 1 次/小时; 突然掉电最多丢 1 小时波形
                                       *   (深睡期间本来就不采样)。 */
#define BAT_HIST_SKIP_BOOT_US 10000000ull  /* 开机 10s 静默期: boot 时 WiFi/SD/LCD 齐上电的真实压降不记 */
#define BAT_HMETA_NS     "batcal"
#define BAT_HMETA_KEY    "hmeta"
#define BAT_HMETA_MAGIC  0x42485431u  /* "BHT1" */

typedef struct { uint32_t magic, base_s, next_gi; } bat_hmeta_t;

static EXT_RAM_BSS_ATTR uint8_t s_hist[BAT_SLOTS];   /* PSRAM: 7 天, 每字节 1 分钟 */
static uint32_t s_hist_base_s;      /* 槽 0 对应的 RTC 秒 */
static uint32_t s_hist_next_gi;     /* 下一条要写的"全局分钟序号" */
static uint32_t s_hist_flush_s;     /* 上次落盘时刻(RTC 秒) */
static bool     s_hist_ready;       /* 元数据已就绪 */
static bool     s_hist_loaded;      /* 已从 NVS 读回数组 */

uint32_t board_battery_clock_s(void)
{
    return (uint32_t)(esp_rtc_get_time_us() / 1000000ull);
}

static uint32_t bat_gi_now(uint32_t now) { return (now - s_hist_base_s) / 60u; }

/* ---- 按天分块落盘: 只重写"当前这一天"那一块 1440B, 旧的天永不重写 ---- */
static void bat_hist_flush_day(uint32_t gi)
{
    uint32_t day = (gi / BAT_MIN_PER_DAY) % BAT_KEEP_DAYS;
    char key[8];
    snprintf(key, sizeof(key), "hd%02u", (unsigned)day);
    nvs_handle_t h;
    if (nvs_open(BAT_HMETA_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, key, &s_hist[day * BAT_MIN_PER_DAY], BAT_MIN_PER_DAY);
    nvs_commit(h);
    nvs_close(h);
}

static void bat_hist_flush(void)
{
    if (!s_hist_ready) return;
    bat_hmeta_t m = { BAT_HMETA_MAGIC, s_hist_base_s, s_hist_next_gi };
    nvs_handle_t h;
    if (nvs_open(BAT_HMETA_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, BAT_HMETA_KEY, &m, sizeof(m));
        nvs_commit(h);
        nvs_close(h);
    }
    uint32_t last_gi = (s_hist_next_gi > 0) ? (s_hist_next_gi - 1) : 0;
    bat_hist_flush_day(last_gi);
    if (s_hist_flush_s) {                        /* 跨天: 把上一天那块也补一次 */
        uint32_t prev_gi = bat_gi_now(s_hist_flush_s);
        if ((last_gi / BAT_MIN_PER_DAY) != (prev_gi / BAT_MIN_PER_DAY)) bat_hist_flush_day(prev_gi);
    }
    s_hist_flush_s = board_battery_clock_s();
}

static void bat_hist_meta_load(void)
{
    nvs_handle_t h;
    if (nvs_open(BAT_HMETA_NS, NVS_READONLY, &h) != ESP_OK) return;
    bat_hmeta_t m;
    size_t len = sizeof(m);
    if (nvs_get_blob(h, BAT_HMETA_KEY, &m, &len) == ESP_OK && len == sizeof(m) &&
        m.magic == BAT_HMETA_MAGIC) {
        uint32_t now = board_battery_clock_s();
        /* ★★ 时间基准校验 (2026-10-14 修"一直采样中") ★★
         * RTC 计时器**断电归零**: 若存下来的 base_s 比当前 now 还大, 说明中间断过电,
         * 旧时间轴已经无效 —— 直接 `now - base_s` 会**下溢**成几十亿秒,
         * 于是 next_gi 冲到天文数字, 当前分钟落到一片 0xFF 上 ⇒ 页面永远"采样中"。
         * 这时**丢弃旧时间轴、重新开始**(日档另有 NVS, 不受影响)。 */
        if (m.base_s > now || (uint32_t)(now - m.base_s) > (uint32_t)BAT_SLOTS * 60u) {
            ESP_LOGW(TAG, "电池历史时间轴失效 (base=%us now=%us) → 重新开始",
                     (unsigned)m.base_s, (unsigned)now);
            return;
        }
        s_hist_base_s  = m.base_s;
        s_hist_next_gi = m.next_gi;
        s_hist_ready   = true;
    }
    nvs_close(h);
}

/* 懒加载 (页面真正要用时才读, 不拖慢开机) */
static void bat_hist_ensure_loaded(void)
{
    if (s_hist_loaded) return;
    s_hist_loaded = true;
    memset(s_hist, BAT_NODATA, sizeof(s_hist));
    if (!s_hist_ready) {                      /* 首次上电 */
        s_hist_base_s  = board_battery_clock_s();
        s_hist_next_gi = 0;
        s_hist_ready   = true;
        return;
    }
    nvs_handle_t h;
    if (nvs_open(BAT_HMETA_NS, NVS_READONLY, &h) != ESP_OK) return;
    for (uint32_t d = 0; d < BAT_KEEP_DAYS; d++) {
        char key[8];
        snprintf(key, sizeof(key), "hd%02u", (unsigned)d);
        size_t blen = BAT_MIN_PER_DAY;
        nvs_get_blob(h, key, &s_hist[d * BAT_MIN_PER_DAY], &blen);   /* 不存在就跳过 */
    }
    nvs_close(h);
    ESP_LOGI(TAG, "电池历史已从 NVS 恢复 (%d 天 × %d 字节 = %d B)", BAT_KEEP_DAYS, BAT_MIN_PER_DAY, BAT_SLOTS);
}

/* 记录一条 (每 60s 调一次): 跳过的分钟标 0xFF, 15 分钟落一次盘 */
static void bat_hist_put(uint16_t mv)
{
    if (!s_hist_ready) bat_hist_meta_load();
    if (!s_hist_ready) { s_hist_base_s = board_battery_clock_s(); s_hist_next_gi = 0; s_hist_ready = true; }
    bat_hist_ensure_loaded();
    if (mv < 2500 || mv > 4500) return;
    uint32_t now = board_battery_clock_s();
    uint32_t gi  = bat_gi_now(now);
    if (s_hist_next_gi == 0) s_hist_next_gi = gi;
    /* 中间跳过的分钟 (睡眠/关机) 一律清成"无数据", 别留一星期前的旧值 */
    while (s_hist_next_gi < gi) {
        s_hist[s_hist_next_gi % BAT_SLOTS] = BAT_NODATA;
        s_hist_next_gi++;
    }
    uint32_t v = (mv - BAT_MV_BASE) / 10u;
    if (v > 254u) v = 254u;
    s_hist[gi % BAT_SLOTS] = (uint8_t)v;
    s_hist_next_gi = gi + 1;
    if (s_hist_flush_s == 0) {           /* 本次开机第一条: 立刻落盘, 免得还没到节拍就睡了 */
        s_hist_flush_s = now;
        bat_hist_flush();
    } else if ((uint32_t)(now - s_hist_flush_s) >= BAT_FLUSH_S) {
        bat_hist_flush();
    }
}

uint32_t board_battery_hist_span_s(void)
{
    bat_hist_ensure_loaded();
    if (!s_hist_ready || s_hist_next_gi == 0) return 0;
    uint32_t gi = bat_gi_now(board_battery_clock_s());
    if (gi < s_hist_next_gi) gi = s_hist_next_gi;
    return (gi > BAT_SLOTS) ? (uint32_t)BAT_SLOTS * 60u : gi * 60u;
}

int board_battery_wave(bat_sample_t *out, int max, uint32_t window_s)
{
    if (out == NULL || max <= 0) return 0;
    bat_hist_ensure_loaded();
    if (!s_hist_ready || s_hist_next_gi == 0) return 0;
    uint32_t gi_now = bat_gi_now(board_battery_clock_s());
    if (gi_now < s_hist_next_gi) gi_now = s_hist_next_gi;
    uint32_t mins = window_s / 60u;
    if (mins == 0) mins = 1;
    uint32_t gi_from = (mins < gi_now) ? (gi_now - mins) : 0;
    uint32_t total = gi_now - gi_from;
    if (total == 0) return 0;
    /* 点太多就等间隔抽稀 ⇒ 天然"近密远疏"(2h 窗口=1 分钟一点, 7 天窗口≈15 分钟一点) */
    uint32_t step = (total > (uint32_t)max) ? ((total + (uint32_t)max - 1) / (uint32_t)max) : 1;
    int n = 0;
    for (uint32_t g = gi_from; g < gi_now && n < max; g += step) {
        uint8_t v = s_hist[g % BAT_SLOTS];
        if (v == BAT_NODATA) continue;             /* 没数据的分钟跳过 → 画线跨过去 */
        out[n].t  = s_hist_base_s + g * 60u;
        out[n].mv = (uint16_t)(BAT_MV_BASE + (uint32_t)v * 10u);
        n++;
    }
    return n;
}

/* ---- 运行时诊断 (remcon 的 `bat` 命令) 2026-10-14 加 ----
 * 一次性打印: raw / 脚上 mV / 分压系数 / 换算 VBAT / 百分比。
 * 用途: 拿万用表同时量 BAT_ADC 脚 与 VBAT, 和这里的数字对照即可判定
 *   ① 分压系数对不对 (k = 真实 VBAT / 脚上 mV)  ② 满量程基准要不要调。 */
/* ★ 2026-10-27: 用外部仪表的真实 VBAT 标定分压比 (命令行 batcal <mV>) */
bool board_battery_calibrate(uint32_t real_mv)
{
    if (real_mv < 2000 || real_mv > 5000) {
        ESP_LOGW(TAG, "BATCAL 拒绝: 真实电压 %u mV 不在 2000~5000 范围", (unsigned)real_mv);
        return false;
    }
    if (board_battery_init() != ESP_OK) return false;

    int raw_sum = 0;
    for (int i = 0; i < BOARD_BATTERY_SAMPLE_COUNT; i++) {
        int raw = 0;
        if (adc_oneshot_read(s_adc_handle, s_bat_chan, &raw) != ESP_OK) return false;
        raw_sum += raw;
    }
    int raw_avg = raw_sum / BOARD_BATTERY_SAMPLE_COUNT;
    int pin_mv = 0;
    if (s_calibrated && s_cali_handle) adc_cali_raw_to_voltage(s_cali_handle, raw_avg, &pin_mv);
    else pin_mv = (raw_avg * 3300) / 4095;
    if (pin_mv < 200) {
        ESP_LOGW(TAG, "BATCAL 拒绝: 脚上仅 %d mV, 读数不可信", pin_mv);
        return false;
    }

    /* k = 真实VBAT / 脚上mV, 以 1/1000 精度存 */
    uint32_t num = (uint32_t)(((uint64_t)real_mv * 1000u) / (uint32_t)pin_mv);
    if (num < 1000 || num > 9000) {
        ESP_LOGW(TAG, "BATCAL 拒绝: 算出的分压比 x%d/1000 越界", (int)num);
        return false;
    }
    int old_num = s_div_num, old_den = s_div_den, old_vbat = pin_mv * s_div_num / s_div_den;
    s_div_num = (int)num;
    s_div_den = 1000;
    bat_div_save();
    ESP_LOGI(TAG, "BATCAL 完成: raw=%d 脚上=%d mV 真实=%u mV ⇒ 分压比 x%d/%d (原 x%d/%d, 原读数 %d mV) 已存 NVS",
             raw_avg, pin_mv, (unsigned)real_mv, s_div_num, s_div_den,
             old_num, old_den, old_vbat);
    return true;
}

void board_battery_diag(void)
{
    if (board_battery_init() != ESP_OK) {
        ESP_LOGI(TAG, "BAT 初始化失败");
        return;
    }

    /* ---- 现场再采一轮（同样的 8 次去极值），给万用表对照用 ---- */
    int raws[BOARD_BATTERY_SAMPLE_COUNT];
    int rmin = 0x7FFF, rmax = 0;
    for (int i = 0; i < BOARD_BATTERY_SAMPLE_COUNT; i++) {
        int raw = 0;
        if (adc_oneshot_read(s_adc_handle, s_bat_chan, &raw) != ESP_OK) return;
        raws[i] = raw;
        if (raw < rmin) rmin = raw;
        if (raw > rmax) rmax = raw;
    }
    int raw_avg = bm_despeckle_avg(raws, BOARD_BATTERY_SAMPLE_COUNT);
    int pin_mv = 0;
    if (s_calibrated && s_cali_handle) {
        if (adc_cali_raw_to_voltage(s_cali_handle, raw_avg, &pin_mv) != ESP_OK) pin_mv = 0;
    } else {
        pin_mv = (raw_avg * 3300) / 4095;      /* 未校准回退，下面会标"未校准" */
    }
    uint32_t vbat = (pin_mv > 0) ? ((uint32_t)pin_mv * s_div_num / s_div_den) : 0;

    /* 异常位 */
    uint8_t flags = 0;
    if (raw_avg <= 0 || raw_avg >= 4095)         flags |= BATF_ADC_SAT;
    if (vbat < 2500 || vbat > 4500)              flags |= BATF_RANGE;
    if ((rmax - rmin) * 3 > 240)                 flags |= BATF_NOISY;
    if (!s_calibrated)                           flags |= BATF_UNCAL;

    char fl[48] = "";
    if (flags & BATF_ADC_SAT) snprintf(fl + strlen(fl), sizeof(fl) - strlen(fl), "ADC饱和 ");
    if (flags & BATF_RANGE)   snprintf(fl + strlen(fl), sizeof(fl) - strlen(fl), "电压越界 ");
    if (flags & BATF_NOISY)   snprintf(fl + strlen(fl), sizeof(fl) - strlen(fl), "波动大 ");
    if (flags & BATF_UNCAL)   snprintf(fl + strlen(fl), sizeof(fl) - strlen(fl), "未校准 ");
    if (!fl[0]) snprintf(fl, sizeof(fl), "正常");

    /* 充电脚原始电平（不做去抖，诊断要看"现在是什么"） */
    chrg_ensure();
    const char *chrg_s = (s_chrg_pin < 0) ? "本板未接"
                       : (gpio_get_level((gpio_num_t)s_chrg_pin) == 0 ? "低(充电中)" : "高(未充电)");

    /* ★ 一段式输出（2026-10-31 精简）: 6 个必须项 + 异常位，一行看全 */
    ESP_LOGI(TAG, "BAT raw=%u[%u..%u]/4095 校准=%s 脚上=%umV 分压=x%d/%d VBAT=%umV soc=%u%% CHRG=%s | %s",
             (unsigned)raw_avg, (unsigned)rmin, (unsigned)rmax,
             s_calibrated ? "ok(curve-fitting)" : "失败(3.3V 线性回退)",
             (unsigned)pin_mv, s_div_num, s_div_den,
             (unsigned)vbat, (unsigned)board_battery_voltage_to_percent(vbat),
             chrg_s, fl);
}

/* ---- 真机测试记录: 公开接口 ---- */
bool board_battery_trace_enable(bool on)
{
    s_trace_on = on;
    ESP_LOGI(TAG, "BATLOG %s (已记 %d 条, 容量 %d)", on ? "ON" : "OFF",
             (int)s_trace_n, (int)BAT_TRACE_N);
    return s_trace_on;
}

bool board_battery_trace_is_on(void) { return s_trace_on; }
int  board_battery_trace_count(void) { return (int)s_trace_n; }

void board_battery_trace_clear(void)
{
    s_trace_n = 0;
    s_trace_i = 0;
}

int board_battery_trace_dump(void)
{
    if (s_trace_n == 0) {
        ESP_LOGI(TAG, "BATLOG 无记录 (先 `batlog on`)");
        return 0;
    }
    /* 环形缓冲：满时最旧的一格就是下一个要写的格 */
    const int start = (s_trace_n < BAT_TRACE_N) ? 0 : (int)s_trace_i;
    for (int k = 0; k < (int)s_trace_n; k++) {
        const bat_trace_t *e = &s_trace[(start + k) % BAT_TRACE_N];
        ESP_LOGI(TAG, "%u,%u,%u,%u,%u",
                 (unsigned)e->t, (unsigned)e->mv, (unsigned)e->soc,
                 (unsigned)e->flags, (unsigned)e->chg);
    }
    ESP_LOGI(TAG, "BATLOG_END %d 条", (int)s_trace_n);
    return (int)s_trace_n;
}

esp_err_t board_battery_read_cached(board_battery_status_t *status)
{
    if (status == NULL) return ESP_ERR_INVALID_ARG;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (!s_cache_valid || (uint32_t)(now - s_cache_ms) >= BOARD_BATTERY_CACHE_MS) {
        board_battery_status_t raw;
        esp_err_t err = board_battery_read(&raw);
        if (err != ESP_OK) return err;   /* 采样失败不污染旧缓存 */

        /* ★ 2026-10-31 精简: 这里原来有"采样中断 → 重置 9 点滤波/5 点峰窗"的一整段。
         *   现在电压每轮独立计算、电量纯查表，**没有任何跨轮历史**，
         *   所以深睡唤醒后的第一次采样天然就是新值，不需要重置逻辑。 */

        /* ---- 充电识别: 只读 CHRG 硬件脚（保留 3 次去抖），不再用电压趋势推断 ---- */
        int chrg_lvl = -1;
        chrg_ensure();
        if (s_chrg_pin >= 0)
            chrg_lvl = (gpio_get_level((gpio_num_t)s_chrg_pin) == 0) ? 1 : 0;
        bool charging = bm_chrg_update(&s_chg, chrg_lvl);

        /* ---- ④ 查表得电量：纯函数，无跨轮状态 ----
         *   电压就是本轮 8 次去极值的平均值，不做任何历史平滑；
         *   所以**休眠唤醒后的第一次采样天然就是新值**，没有旧状态要清。 */
        uint8_t soc = board_battery_voltage_to_percent(raw.voltage_mv);

        s_cache.voltage_mv = raw.voltage_mv;
        s_cache.percent    = soc;
        s_cache_ms    = now;
        s_cache_valid = true;

        /* 诊断快照 */
        s_diag.soc      = soc;
        s_diag.charging = charging;

        /* 真机测试记录（关闭时只判一个 bool，不采样不分配） */
        bat_trace_push((uint16_t)raw.voltage_mv, soc, s_diag.flags, (uint8_t)(charging ? 1 : 0));

        /* ★ 开机 10s 静默期: boot 期间 WiFi/BT/SD/LCD/音频一起上电, 电池端电压被负载拉低,
         *   这段时间记进去就是 linit 问的"刚开机电压为什么波动"(那是真实压降, 但不该污染趋势图)。 */
        if (esp_timer_get_time() > (int64_t)BAT_HIST_SKIP_BOOT_US)
            bat_hist_put((uint16_t)raw.voltage_mv);
    }
    *status = s_cache;
    return ESP_OK;
}
