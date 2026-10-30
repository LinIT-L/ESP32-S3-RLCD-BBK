#include "board_battery.h"

#include <stdbool.h>
#include <stdint.h>

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rtc_time.h"   /* esp_rtc_get_time_us(): 窗口"天"计时, 跨深睡继续走 */
#include "nvs_flash.h"      /* 一个月电池日档持久化 (跨天写一次) */
#include "driver/gpio.h"
#include "board_pins.h"     /* board_pins_get()->chrg: 充电中检测脚 */

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

/* ==== ★★ "满电压" = 最近 30 天的电压峰值 (linit 2026-10-14 定案) ★★
 * 需求原话: "我需要一个月以内电压最高状态 作为电池充满状态。"
 * 做法: 按"天"记电压峰值, 环形保留最近 31 个槽位; 百分比基准 = 窗口内的**最高值**。
 *   ⇒ 每次充电到顶, 峰值被自然抬高; 超过 30 天的老峰值自动过期掉出窗口, 不会被旧数据长期绑架。
 * 天的来源: **RTC 计时器** `esp_rtc_get_time_us()` —— 深睡期间继续走, 只有全断电才归零
 *   (那时窗口本就该重来, 所以状态放 RTC 内存就够, 不需要 NVS)。
 *   ⛔ 不用墙上时钟: 本机没有 RTC 后备电池, 断电后 `time()` 会回到 1970。
 * 护栏: 峰值 < 4000mV 时不认 (说明这段时间压根没充过电), 退回固定基准 4100mV。 */
#define BAT_WIN_DAYS      31          /* 窗口 = 最近 31 天 */
#define BAT_WIN_MIN_MV    4000        /* 峰值下限: 低于此值不足以当"满" */
#define BAT_WIN_MAX_MV    4320        /* 单点上限: 超过判为异常读数, 丢弃 */
#define BAT_WIN_MAGIC     0x42415457u /* "BATW": 校验 RTC 内存是否为本模块写过 */

typedef struct {
    uint32_t magic;
    uint32_t day[BAT_WIN_DAYS];   /* 各槽的"日号+1"; 0 = 空槽 (日号从 0 起, 故 +1 避开 0) */
    uint16_t mv [BAT_WIN_DAYS];   /* 各槽当天的电压峰值 (mV) */
    uint16_t mvmin[BAT_WIN_DAYS]; /* 各槽当天的电压谷值 (mV) —— 一个月日档用 */
} bat_win_t;

/* 日档持久化: ns/key, 只在跨天时写一次 (一个月 30 次, 无磨损顾虑) */
#define BAT_DAY_NVS_NS   "batcal"
#define BAT_DAY_NVS_KEY  "daywin"

static RTC_NOINIT_ATTR bat_win_t s_win;

static uint32_t bat_day_now(void)
{
    return (uint32_t)(esp_rtc_get_time_us() / 86400000000ull);   /* 天 */
}

/* ---- 一个月日档: NVS 读写 (每天一条 min/max; 只在跨天写, 一个月 ~30 次) ---- */
static void bat_days_save(void)
{
    if (s_win.magic != BAT_WIN_MAGIC) return;
    nvs_handle_t h;
    if (nvs_open(BAT_DAY_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_blob(h, BAT_DAY_NVS_KEY, (const void *)&s_win, sizeof(s_win)) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

static void bat_days_load(void)
{
    nvs_handle_t h;
    if (nvs_open(BAT_DAY_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    bat_win_t tmp;
    size_t len = sizeof(tmp);
    if (nvs_get_blob(h, BAT_DAY_NVS_KEY, &tmp, &len) == ESP_OK &&
        len == sizeof(tmp) && tmp.magic == BAT_WIN_MAGIC) {
        memcpy((void *)&s_win, &tmp, sizeof(s_win));   /* 断电后恢复一个月日档 */
        int ok = 0;
        uint32_t d1 = bat_day_now() + 1;
        for (int i = 0; i < BAT_WIN_DAYS; i++)
            if (s_win.day[i] && d1 >= s_win.day[i] && (d1 - s_win.day[i]) < BAT_WIN_DAYS) ok++;
        ESP_LOGI(TAG, "恢复电池日档: %d 天有效 (最长 %d 天)", ok, BAT_WIN_DAYS);
    }
    nvs_close(h);
}

/* 每次**真采样**调一次: 把当天峰值抬到当前读数 (只记"够高"的读数, 低电量不参与) */
static void bat_win_add(uint16_t mv)
{
    if (mv < BAT_WIN_MIN_MV || mv > BAT_WIN_MAX_MV) return;
    if (s_win.magic != BAT_WIN_MAGIC) { memset(&s_win, 0, sizeof(s_win)); s_win.magic = BAT_WIN_MAGIC; }
    uint32_t d1 = bat_day_now() + 1;              /* +1: 0 留作"空槽"标记 */
    int slot = (int)((d1 - 1) % BAT_WIN_DAYS);
    if (s_win.day[slot] != d1) {                 /* 新的一天: 重开, 并把刚结束的一天落盘 */
        s_win.day[slot]   = d1;
        s_win.mv[slot]    = mv;
        s_win.mvmin[slot] = mv;
        bat_days_save();
    } else {
        if (mv > s_win.mv[slot])    s_win.mv[slot]    = mv;   /* 同一天: 抬峰 */
        if (mv < s_win.mvmin[slot]) s_win.mvmin[slot] = mv;   /* 同一天: 压谷 */
    }
}

/* 窗口内 (最近 BAT_WIN_DAYS 天) 的最高电压; 0 = 窗口里没有有效数据 */
static uint16_t bat_win_full_mv(void)
{
    if (s_win.magic != BAT_WIN_MAGIC) return 0;
    uint32_t d1 = bat_day_now() + 1;
    uint16_t best = 0;
    for (int i = 0; i < BAT_WIN_DAYS; i++) {
        if (s_win.day[i] == 0) continue;
        if (d1 < s_win.day[i]) continue;                                  /* 日号来自上一次上电 → 过期 */
        if ((d1 - s_win.day[i]) >= BAT_WIN_DAYS) continue;                /* 超过窗口 → 过期 */
        if (s_win.mv[i] > best) best = s_win.mv[i];
    }
    return best;
}

/* 取日档 (旧→新; day = 日号, 非日历日期 —— 计时走 RTC, 断电重来) */
int board_battery_days_get(bat_day_t *out, int max)
{
    if (out == NULL || max <= 0 || s_win.magic != BAT_WIN_MAGIC) return 0;
    uint32_t d1 = bat_day_now() + 1;
    bat_day_t tmp[BAT_WIN_DAYS];
    int c = 0;
    for (int i = 0; i < BAT_WIN_DAYS; i++) {
        if (s_win.day[i] == 0 || d1 < s_win.day[i]) continue;
        if ((d1 - s_win.day[i]) >= BAT_WIN_DAYS) continue;
        tmp[c].day    = s_win.day[i] - 1;
        tmp[c].min_mv = s_win.mvmin[i];
        tmp[c].max_mv = s_win.mv[i];
        c++;
    }
    for (int i = 1; i < c; i++) {                 /* 按日号插入排序 */
        bat_day_t k = tmp[i];
        int j = i - 1;
        while (j >= 0 && tmp[j].day > k.day) { tmp[j + 1] = tmp[j]; j--; }
        tmp[j + 1] = k;
    }
    int n = (c < max) ? c : max;
    for (int i = 0; i < n; i++) out[i] = tmp[i];
    return n;
}

static uint32_t battery_full_mv(void)
{
    uint16_t w = bat_win_full_mv();
    return (w >= BAT_WIN_MIN_MV) ? w : BOARD_BATTERY_FULL_MV;
}

static adc_oneshot_unit_handle_t s_adc_handle = NULL;
static adc_cali_handle_t s_cali_handle = NULL;
static bool s_calibrated = false;
static bool s_initialized = false;

static void bat_hist_meta_load(void);   /* 前向声明: init 里先调用, 定义在下方 */

static uint8_t board_battery_voltage_to_percent(uint32_t mv)
{
    const uint32_t full = battery_full_mv();   /* ★ 满电压优先用"自学习"到的值 */
    if (mv >= full) {
        return 100;
    }
    if (mv <= BOARD_BATTERY_EMPTY_MV) {
        return 0;
    }

    return (uint8_t)(((mv - BOARD_BATTERY_EMPTY_MV) * 100U) /
                     (full - BOARD_BATTERY_EMPTY_MV));
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
    bat_days_load();       /* 恢复一个月日档 (NVS, 断电不丢) */
    bat_hist_meta_load();  /* 电压历史元数据 (数组懒加载, 不拖慢开机) */

    s_initialized = true;
    ESP_LOGI(TAG, "Battery ADC initialized: %s ADC1_CH%d, divider x%d/%d, 满电压 %u mV",
             BOARD_BATTERY_ADC_GPIO_NAME, BOARD_BATTERY_ADC_CH,
             s_div_num, s_div_den,
             (unsigned)battery_full_mv());

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

    int raw_sum = 0;
    for (int i = 0; i < BOARD_BATTERY_SAMPLE_COUNT; i++) {
        int raw = 0;
        ESP_RETURN_ON_ERROR(adc_oneshot_read(s_adc_handle, s_bat_chan, &raw),
                            TAG,
                            "read battery adc failed");
        raw_sum += raw;
    }

    int raw_avg = raw_sum / BOARD_BATTERY_SAMPLE_COUNT;
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
    status->percent = board_battery_voltage_to_percent(battery_mv);

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
        ESP_LOGI(TAG, "BATDIAG 初始化失败");
        return;
    }
    int raw_sum = 0;
    for (int i = 0; i < BOARD_BATTERY_SAMPLE_COUNT; i++) {
        int raw = 0;
        if (adc_oneshot_read(s_adc_handle, s_bat_chan, &raw) != ESP_OK) return;
        raw_sum += raw;
    }
    int raw_avg = raw_sum / BOARD_BATTERY_SAMPLE_COUNT;
    int pin_mv = -1;
    if (s_calibrated && s_cali_handle) adc_cali_raw_to_voltage(s_cali_handle, raw_avg, &pin_mv);
    uint32_t vbat = (pin_mv > 0)
        ? (uint32_t)pin_mv * s_div_num / s_div_den : 0;
    {   /* 一个月日档: 有效天数 + 今日 min/max */
        bat_day_t db[BOARD_BATTERY_DAYS];
        int dn = board_battery_days_get(db, BOARD_BATTERY_DAYS);
        if (dn > 0)
            ESP_LOGI(TAG, "BATDAYS %d 天归档 (最长 %d 天, 存NVS断电不丢); 最近一天 min=%umV max=%umV",
                     dn, BOARD_BATTERY_DAYS, (unsigned)db[dn-1].min_mv, (unsigned)db[dn-1].max_mv);
        else
            ESP_LOGI(TAG, "BATDAYS 暂无日档 (跨天后自动归档)");
    }
    {   /* 历史环形状态 (验证"跨深睡保留"): 条数 / 最旧-最新时间戳 / 容量 */
        ESP_LOGI(TAG, "BATHIST 1字节/分钟 %u天=%dB base=%us next_gi=%u now=%us 跨度=%us ok=%d ld=%d",
                 (unsigned)BAT_KEEP_DAYS, BAT_SLOTS, (unsigned)s_hist_base_s,
                 (unsigned)s_hist_next_gi, (unsigned)board_battery_clock_s(),
                 (unsigned)board_battery_hist_span_s(), (int)s_hist_ready, (int)s_hist_loaded);
    }
    ESP_LOGI(TAG, "BATDIAG %s raw=%d/%d 脚上=%d mV 系数=%d/%d => VBAT=%u mV (%u%%) "
                  "[满=%u(30天峰值%s) 空=%d mV]",
             BOARD_BATTERY_ADC_GPIO_NAME, raw_avg, 4095, pin_mv,
             s_div_num, s_div_den,
             (unsigned)vbat, (unsigned)board_battery_voltage_to_percent(vbat),
             (unsigned)battery_full_mv(), bat_win_full_mv() ? "命中" : "默认",
             BOARD_BATTERY_EMPTY_MV);
}

esp_err_t board_battery_read_cached(board_battery_status_t *status)
{
    if (status == NULL) return ESP_ERR_INVALID_ARG;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (!s_cache_valid || (uint32_t)(now - s_cache_ms) >= BOARD_BATTERY_CACHE_MS) {
        esp_err_t err = board_battery_read(&s_cache);
        if (err != ESP_OK) return err;   /* 采样失败不污染旧缓存 */
        s_cache_ms = now;
        s_cache_valid = true;
        /* ★ 开机 10s 静默期: boot 期间 WiFi/BT/SD/LCD/音频一起上电, 电池端电压被负载拉低,
         *   这段时间记进去就是 linit 问的"刚开机电压为什么波动"(那是真实压降, 但不该污染趋势图)。 */
        if (esp_timer_get_time() > (int64_t)BAT_HIST_SKIP_BOOT_US)
            bat_hist_put((uint16_t)s_cache.voltage_mv);
        bat_win_add((uint16_t)s_cache.voltage_mv);         /* 记录"最近 30 天电压峰值" */
    }
    *status = s_cache;
    return ESP_OK;
}
