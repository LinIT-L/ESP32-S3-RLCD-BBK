/* ltc2954.c — LTC2954 硬件电源开关控制器 (2026-11-13)
 *
 * 职责边界（很窄，故意的）:
 *   本模块**不**关外设、不刷屏、不碰 SD / 音频 / GPS。安全关机流程仍然是
 *   power_mgr_off() 那一条（停射频 → 关音频 → GPS 停靠 → 卸 SD → 关屏 → 深睡），
 *   本模块只在 power_mgr_off() 的**末尾**补一步"真断电"。
 *
 * 时序:
 *   ① 开机 ltc2954_init(): GPIO42 = 开漏输出 + 释放(高) → 上电绝不断电
 *   ② /INT 实物接 GPIO44, **由 input.c 当普通电源键轮询**(btn_pwr=44)做 0.5s/2s 判定;
 *      本模块既不配置也不消费这个脚 —— 一个脚不能两个主人, 且"按下即断电"体验等于重启。
 *   ③ 2s 长按 → 关机确认框 → power_mgr_off():
 *      快速收尾(音频/功放/震动/屏 Sleep In/日志落盘/SD 卸载, ~260ms) → ltc2954_power_cut()
 *   ④ ltc2954_power_cut() → GPIO42 拉低 → LTC2954 断开系统电源(不再走到深睡)
 *      若 800ms 内没断（未接硬件/开关关闭），power_mgr 回落深睡兜底。
 *
 * 电气约定（与 LTC2954 数据手册一致）:
 *   KILL 低有效: 拉低并保持超过芯片内部定时（-1 型 1ms / -2 型 512ms）后，
 *   芯片拉低 EN 完成断电。因此:
 *     · 空闲/安全电平 = **高（开漏释放）**，绝不主动驱动高电平（避免与外部上拉打架）
 *     · 触发断电     = 拉低并保持 CUT_HOLD_MS
 *   /INT(GPIO44) 是 2954 的按键镜像输出, 由 input.c 当电源键读; 本模块不碰（见 ②）。
 */
#include "ltc2954.h"

#include <stdbool.h>
#include <stddef.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "esp_log.h"

#include "board_pins.h"
#include "svc_cfg.h"
#include "hw_cfg.h"      /* 2026-11-22: 用"时钟芯片在不在"区分两代小智一体版 */

#define TAG "LTC2954"

/* KILL 有效电平: 0 = 拉低断电（LTC2954 标准用法）。若将来换高有效的变体只改这一处。 */
#define KILL_ACTIVE_LEVEL   0
#define KILL_IDLE_LEVEL     (KILL_ACTIVE_LEVEL ? 0 : 1)

#define CUT_HOLD_MS         800     /* 拉低 KILL 后等待断电的时间（> 512ms 最长内部定时） */

static bool s_inited   = false;
static bool s_hw_ok    = false;   /* 引脚与当前板型不冲突 */
static bool s_pin_ok   = false;   /* 引脚不冲突（与 s_hw_ok 分开：无 2954 的旧版小智
                                   *   引脚也不冲突，但硬件不存在，两者必须能区分） */
static bool s_enabled  = false;   /* 设置页开关（NVS） */
static bool s_force    = false;   /* ★ 本次关机强制真断电（确认框点的确认，不落盘） */

/* ---- 板型冲突检查 ----
 * 任务要求"检查两个 GPIO 是否与现有工程功能冲突"。
 * 事实: 三张板型表里 ——
 *   小智一体版（本次目标板）: tp_rst=6, gps_tx=41, chrg=-1 ⇒ GPIO42 在固件视角空闲
 *     (GPIO44 硬件上是 2954 的 /INT, 固件不配置它, 所以也算空闲)
 *   微雪原版 / 标准扩展版  : tp_rst=2, gps_tx=43 ⇒ 仍需逐脚比对
 * 所以不能写死"永远可用"：换板后必须自动让位，绝不能去抢触摸复位脚或 GPS 的 TX。
 * 这里把 board_pins_t 里所有"功能脚"字段逐个比对，命中即判冲突并整体禁用本模块。 */
static bool pin_used_by_board(const board_pins_t *bp, int pin)
{
    if (!bp || pin < 0) return false;
    const int v[] = {
        bp->lcd_dc, bp->lcd_cs, bp->lcd_sck, bp->lcd_mosi, bp->lcd_rst,
        bp->tp_sda, bp->tp_scl, bp->tp_int, bp->tp_rst,
        bp->btn_pwr, bp->btn_right, bp->btn_left, bp->chrg,
        bp->i2s_mclk, bp->i2s_bclk, bp->i2s_ws, bp->i2s_dout, bp->i2s_din,
        bp->codec_sda, bp->codec_scl, bp->pa_pin, bp->vib,
        bp->gps_tx, bp->gps_rx, bp->gps_extint,
        bp->sd_clk, bp->sd_cmd, bp->sd_d0,
        bp->sd_cs, bp->sd_sck, bp->sd_mosi, bp->sd_miso,
    };
    for (size_t i = 0; i < sizeof(v) / sizeof(v[0]); i++)
        if (v[i] == pin) return true;
    return false;
}

static const char *pin_owner(const board_pins_t *bp, int pin)
{
    if (!bp) return "?";
    if (bp->lcd_dc == pin) return "LCD_DC";
    if (bp->lcd_cs == pin) return "LCD_CS";
    if (bp->lcd_sck == pin) return "LCD_SCK";
    if (bp->lcd_mosi == pin) return "LCD_MOSI";
    if (bp->lcd_rst == pin) return "LCD_RST";
    if (bp->tp_sda == pin) return "TP_SDA";
    if (bp->tp_scl == pin) return "TP_SCL";
    if (bp->tp_int == pin) return "TP_INT";
    if (bp->tp_rst == pin) return "TP_RST";
    if (bp->btn_pwr == pin) return "BTN_PWR";
    if (bp->btn_right == pin) return "BTN_RIGHT";
    if (bp->btn_left == pin) return "BTN_LEFT";
    if (bp->chrg == pin) return "CHRG";
    if (bp->i2s_mclk == pin) return "I2S_MCLK";
    if (bp->i2s_bclk == pin) return "I2S_BCLK";
    if (bp->i2s_ws == pin) return "I2S_WS";
    if (bp->i2s_dout == pin) return "I2S_DOUT";
    if (bp->i2s_din == pin) return "I2S_DIN";
    if (bp->codec_sda == pin) return "CODEC_SDA";
    if (bp->codec_scl == pin) return "CODEC_SCL";
    if (bp->pa_pin == pin) return "PA_EN";
    if (bp->vib == pin) return "VIB";
    if (bp->gps_tx == pin) return "GPS_TX";
    if (bp->gps_rx == pin) return "GPS_RX";
    if (bp->gps_extint == pin) return "GPS_EXTINT";
    if (bp->sd_clk == pin) return "SD_CLK";
    if (bp->sd_cmd == pin) return "SD_CMD";
    if (bp->sd_d0 == pin) return "SD_D0";
    if (bp->sd_cs == pin) return "SD_CS";
    if (bp->sd_sck == pin) return "SD_SCK";
    if (bp->sd_mosi == pin) return "SD_MOSI";
    if (bp->sd_miso == pin) return "SD_MISO";
    return "?";
}

/* GPIO42 KILL: 开漏输出 + 内部上拉 = 安全电平(高)。
 * ⚠ 两条都是必须的，少一条就可能是"莫名其妙断电"或"关不掉机":
 *   · 开漏(OD)而不是推挽: 释放时不主动驱动高，板上有上拉就跟着上拉，
 *     不会与 LTC2954 的 KILL 输入结构对顶；只有关机那一刻才主动拉低。
 *   · **必须开内部上拉**: 开漏释放 = 高阻。若既不上拉又开漏，引脚就是浮空的，
 *     噪声一拉低就可能触发 LTC2954 断电 —— 这是最危险的一种"偶发断电"。
 * ★ 抽成函数: 旧版小智首探不到时钟时先不配脚，等 svc_rtc 后置确认到 DS3231
 *   （= 确实是 PCB1_3）再调本函数补配，见 ltc2954_autoconf_from_rtc()。 */
static esp_err_t kill_pin_setup(void)
{
    gpio_config_t kc = {
        .pin_bit_mask = 1ULL << LTC2954_KILL_GPIO,
        .mode         = GPIO_MODE_OUTPUT_OD,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&kc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO%d(KILL) 配置失败: %s ⇒ 硬件关机禁用",
                 LTC2954_KILL_GPIO, esp_err_to_name(err));
        return err;
    }
    gpio_set_level((gpio_num_t)LTC2954_KILL_GPIO, KILL_IDLE_LEVEL);
    return ESP_OK;
}

esp_err_t ltc2954_init(void)
{
    if (s_inited) return s_hw_ok ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
    s_inited = true;

    const board_pins_t *bp = board_pins_get();

    /* ---- ① 冲突检查（先查再动脚，冲突时一个寄存器都不写） ---- */
    const bool c_kill = pin_used_by_board(bp, LTC2954_KILL_GPIO);
    if (c_kill) {
        s_hw_ok = false;
        /* 只检查 KILL: /INT(GPIO44) 归 input.c, 本模块从不碰它, 无需判冲突 */
        ESP_LOGW(TAG, "GPIO%d 与当前板型[%s]冲突 (KILL:%s 占用=%s)"
                      " ⇒ 硬件关机功能禁用, 不触碰该引脚",
                 LTC2954_KILL_GPIO, bp ? bp->name : "?",
                 pin_owner(bp, LTC2954_KILL_GPIO), "是");
        /* ★ 2026-11-22 linit: 设置页不再暴露"硬件关机"开关，改为**有硬件就自动开**。
         *   引脚与本板型冲突 ⇒ 这块板上根本没有可用的 LTC2954 ⇒ 一律关闭，
         *   不再从 NVS 读用户选择（那个开关已经从 UI 移除）。 */
        s_enabled = false;
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* ---- ①.5 两代小智一体版的分辨（2026-11-22 linit 要求） ----
     * 引脚表完全相同，但只有 PCB1_3 焊了 LTC2954 + DS3231MZ+；
     * 旧版 PCB1_0~1_2 的 42/44 是空脚，断电靠"深睡模拟关机"。
     * 2954 没有任何可探的通道，而 DS3231 能用 I2C 问 0x68，两片永远同板出现
     * ⇒ **以时钟芯片为判据**：没时钟 = 旧版 = 不碰 GPIO42，关机走休眠模拟。
     * ⚠ 首探可能因触摸总线还没建好而漏（见 hw_cfg.h 对 hw_cfg_note_rtc 的说明），
     *   所以这里只"暂不启用"、不清除引脚可用性；4 秒后 svc_rtc 挂上芯片会回调
     *   ltc2954_autoconf_from_rtc() 补配引脚并启用，不会把新版永久判成旧版。 */
    s_pin_ok = true;
    if (hw_cfg_board_id() == HW_BOARD_XIAOZHI && !hw_cfg_detected(HW_RTC)) {
        /* ★ 2026-11-22 linit: 「有识别到时钟就自动开启, 没有就自动关闭」——
         *   无 0x68 ⇒ 旧版小智(无 LTC2954) ⇒ 硬件关机**关闭**，不再看 NVS。
         *   设置页的开关已移除，用户无从手动干预，这里必须自洽。 */
        s_enabled = false;
        ESP_LOGW(TAG, "未检测到时钟芯片(0x68) ⇒ 判定旧版小智一体版(无 LTC2954): "
                      "GPIO%d 不驱动, 关机走休眠模拟", LTC2954_KILL_GPIO);
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* ---- ② GPIO42 KILL: 开漏输出 + 内部上拉 = 安全电平(高) ----
     * ⚠ 两条都是必须的，少一条就可能是"莫名其妙断电"或"关不掉机":
     *   · 开漏(OD)而不是推挽: 释放时不主动驱动高，板上有上拉就跟着上拉，
     *     不会与 LTC2954 的 KILL 输入结构对顶；只有关机那一刻才主动拉低。
     *   · **必须开内部上拉**: 开漏释放 = 高阻。若既不上拉又开漏，引脚就是浮空的，
     *     噪声一拉低就可能触发 LTC2954 断电 —— 这是最危险的一种"偶发断电"。
     *     配了上拉之后，"安全电平"才是确定的 3.3V。 */
    esp_err_t err = kill_pin_setup();
    if (err != ESP_OK) return err;

    s_hw_ok   = true;
    /* ★ 2026-11-22 linit: 走到这里说明**已识别到时钟芯片(0x68)** ⇒ 这块板就是
     *   带 LTC2954 的 PCB1_3 ⇒ 硬件关机自动开启，不再从 NVS 读、也不再让用户手选。
     *   没有时钟的旧版在前面 ①.5 就 return 了，不会到这里。 */
    s_enabled = true;

    ESP_LOGI(TAG, "初始化完成: KILL=GPIO%d(开漏, 安全电平=释放/高) | /INT=GPIO%d(归 input.c "
                  "当电源键, 本模块不接管) | 板型=%s | 硬件关机=%s",
             LTC2954_KILL_GPIO, LTC2954_INT_GPIO, bp ? bp->name : "?",
             s_enabled ? "自动开启(已识别时钟芯片)" : "关闭");
    return ESP_OK;
}

bool ltc2954_is_hw_available(void) { return s_hw_ok; }

bool ltc2954_is_enabled(void) { return s_enabled; }

/* ★ 2026-11-20: 确认框点"确认" ⇒ 本次关机走硬件断电 (不写 NVS)。 */
void ltc2954_force_cut_once(void) { s_force = true; }
bool ltc2954_is_cut_requested(void) { return s_enabled || s_force; }

void ltc2954_set_enabled(bool en)
{
    /* ⚠ 关键: 无论开关怎么变，GPIO42 一律回到"安全电平"。
     *   关掉开关只是让 ltc2954_power_cut() 不再拉低 KILL(改走深睡), 绝不能顺手断电。 */
    if (s_hw_ok) {
        gpio_hold_dis((gpio_num_t)LTC2954_KILL_GPIO);
        gpio_set_level((gpio_num_t)LTC2954_KILL_GPIO, KILL_IDLE_LEVEL);
    }
    s_enabled = en;
    svc_cfg_set_u8(LTC2954_CFG_NS, LTC2954_CFG_KEY, en ? 1 : 0);
    /* 历史键 hw_off_manual: 现在没人读(自动启用只看时钟芯片), 仍写一次只为不留下
     * 与旧固件相反的值; 将来若恢复"用户手选优先"再从这里取值。 */
    svc_cfg_set_u8(LTC2954_CFG_NS, LTC2954_CFG_MANUAL, en ? 0 : 1);
    if (!s_hw_ok) {
        ESP_LOGW(TAG, "硬件关机开关=%s (本板型无 LTC2954 硬件, 仅记录设置)", en ? "开" : "关");
        return;
    }
    ESP_LOGI(TAG, "硬件关机=%s (GPIO%d 保持安全电平, NVS %s.%s=%d)",
             en ? "开" : "关", LTC2954_KILL_GPIO,
             LTC2954_CFG_NS, LTC2954_CFG_KEY, en ? 1 : 0);
}

/* ★ 2026-11-22 linit「有识别到时钟就自动开启, 没有就自动关闭」
 *   判据唯一: **时钟芯片 DS3231(0x68) 在不在**。两片永远同板出现
 *   （PCB1_3 焊 DS3231 + LTC2954；PCB1_0~1_2 都不焊）。
 *   ⛔ 不再读 NVS 里的用户开关，也不再检查 MANUAL 标志 —— 设置页的
 *     「硬件关机」开关已按 linit 要求移除，用户无从手动干预，
 *     留着 MANUAL 判断只会让状态与 UI 不一致。
 *   本函数负责"开机首探漏了、后来 svc_rtc 真把芯片挂上"的补配路径。 */
void ltc2954_autoconf_from_rtc(void)
{
    if (!s_pin_ok) return;                   /* 引脚冲突(微雪/标准) ⇒ 与本板无关 */
    if (!s_hw_ok) {
        /* 开机首探没认到 0x68 ⇒ 上面按"旧版小智"暂不启用；现在 svc_rtc 真把芯片挂上了
         * ⇒ 就是 PCB1_3，补配引脚再启用（否则新版会被永久判成旧版，失去硬件断电）。 */
        if (kill_pin_setup() != ESP_OK) return;
        s_hw_ok = true;
        ESP_LOGW(TAG, "后置确认到时钟芯片(DS3231 0x68) ⇒ 补配 GPIO%d, 启用硬件断电",
                 LTC2954_KILL_GPIO);
    }
    if (s_enabled) return;                   /* 已开, 不重复动作 */
    /* kill_pin_setup() 已把 KILL 置为安全电平(高) ⇒ 这里只翻状态位，不写 NVS。 */
    s_enabled = true;
    ESP_LOGW(TAG, "检测到时钟芯片(DS3231 0x68) ⇒ 自动开启硬件断电");
}

void ltc2954_power_cut(void)
{
    if (!s_hw_ok) return;      /* 板型冲突/未接入: 一个脚都不碰 */

    /* GPIO42 可能在前面的"引脚隔离 / hold"里被冻结，先解冻再驱动 */
    gpio_hold_dis((gpio_num_t)LTC2954_KILL_GPIO);
    gpio_set_direction((gpio_num_t)LTC2954_KILL_GPIO, GPIO_MODE_OUTPUT_OD);

    if (!ltc2954_is_cut_requested()) {
        /* 开关关闭且本次没强制: 回到安全电平后直接返回 —— 关机仍由调用方的深睡完成 */
        gpio_set_level((gpio_num_t)LTC2954_KILL_GPIO, KILL_IDLE_LEVEL);
        ESP_LOGI(TAG, "硬件关机开关=关 ⇒ 不拉 KILL (GPIO%d 保持安全电平), 走原深睡流程",
                 LTC2954_KILL_GPIO);
        return;
    }

    ESP_LOGW(TAG, "安全关机已完成 ⇒ 拉低 GPIO%d 触发 LTC2954 硬件断电%s", LTC2954_KILL_GPIO,
             s_enabled ? "" : " (本次由确认框强制)");
    gpio_set_level((gpio_num_t)LTC2954_KILL_GPIO, KILL_ACTIVE_LEVEL);
    gpio_hold_en((gpio_num_t)LTC2954_KILL_GPIO);   /* 万一没立刻断电，电平也保持不变 */

    /* 等芯片内部定时器到点断开 EN。正常情况这里不会返回。 */
    vTaskDelay(pdMS_TO_TICKS(CUT_HOLD_MS));
    ESP_LOGW(TAG, "%dms 后仍未断电(未接硬件/开关刚关) ⇒ 交给 esp_deep_sleep_start 兜底",
             CUT_HOLD_MS);
}
