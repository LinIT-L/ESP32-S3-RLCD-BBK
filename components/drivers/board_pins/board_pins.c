/* board_pins.c — 三张板型表 + 运行时当前表（2026-10-18 定案）
 *
 * ★ 一个固件通吃三块板：本文件只是"三张引脚表"，**不做识别**。
 *   识别在 `services/hw_cfg`（开机探 I2C 指纹）→ 命中后调 `board_pins_set_id()` 切表。
 *   ⇒ 同一份固件换板即生效，不用重编；`BOARD_IS_WEIXUE` 只在"探测全失败"时兜底默认。
 *
 * ⛔ 凡按板型不同的引脚，一律进这张表；任何 .c 不许写死 GPIO 号，也不许写
 *   `BOARD_IS_WEIXUE ? A : B`（编译期常量，运行时换板不变 —— 震动/触摸/GPS 都踩过）。
 */
#include "board_pins.h"
#include "esp_log.h"

#define TAG "BOARD"

/* ==================== ① 小智一体版 ====================
 * 模块自带充电/降压/功放/解码/麦克风；屏 RST 不接；触摸与 codec 分两条 I2C。 */
static const board_pins_t BP_XIAOZHI = {
    .id = BOARD_ID_XIAOZHI, .name = "小智一体版",
    .lcd_dc = 7, .lcd_cs = 11, .lcd_sck = 9, .lcd_mosi = 10, .lcd_rst = -1,
    .lcd_freq_mhz = 33,          /* ★ 2026-10-24 linit 定: 保守用 33MHz(= ST7305 spec 上限)。
                                  *   ST7305 写时钟最小周期 30ns ⇒ 上限 ≈33MHz; 微雪官方也用 24MHz。
                                  *   ⚠ S3 的 SPI 时钟由 80MHz APB 分频: 请求 33 → 实际取 80/3 ≈ 26.7MHz。
                                  *   整帧 15000B ≈ 4.5ms, 但送屏已异步(与绘制重叠), 体感不受影响。 */
    .tp_sda = 5, .tp_scl = 13, .tp_int = 12, .tp_rst = 6,
    .btn_pwr = 1, .btn_right = 0, .btn_left = 4,
    .bat_adc_chan = 2,                     /* ADC1_CH2 = GPIO3 (模块内部电池分压) */
    .chrg = 42,                            /* 模组 29 脚 CHRG → GPIO42(模组 35 脚)。
                                            *   (41 已被 GPS-RXD 占用, 42 是最后一个干净的脚)
                                            *   ★ 按模组手册: **42 = ITMS(MTMS/JTAG-TMS)**; 41 = ATDI(JTAG-TDI);
                                            *     U0TXD = GPIO43(37 脚) / U0RXD = GPIO44(36 脚)。
                                            *   ★ 实测(输入+内部上拉): 18/14/2 都读到 0 —— 模块侧被下拉,
                                            *     拿它读"低=充电中"会永远报充电中; 41/42 读到 1, 干净。
                                            *   ⚠ 41(GPS-RXD) + 42(CHRG) 两脚都是 JTAG ⇒ **外部 JTAG 彻底不可用**;
                                            *     本机烧录/调试走 USB-Serial-JTAG(19/20), 日常不受影响。 */
    .i2s_mclk = 40, .i2s_bclk = 39, .i2s_ws = 45, .i2s_dout = 48, .i2s_din = 21,
    .codec_sda = 38, .codec_scl = 47, .pa_pin = 46,
    .vib = 8,
    /* GPS 实际接线 (linit 2026-10-24): GPS-TXD→GPIO14(模组 22 脚) · GPS-RXD→GPIO41(34 脚)
     *   · EXTINT→GPIO18(11 脚)。
     * ⚠ 方向别搞反: gps_tx/gps_rx 是**ESP32 自己的** TX/RX 脚
     *   (uart_set_pin(tx, rx)) ⇒ gps_tx = GPS 的 RXD 那根 = 41, gps_rx = GPS 的 TXD 那根 = 14。 */
    .gps_tx = 41, .gps_rx = 14, .gps_extint = 18,
    .sd_bus = 0,                                   /* SDMMC 1-bit */
    .sd_clk = 16, .sd_cmd = 15, .sd_d0 = 17,
};

/* ==================== ② 微雪原版 ====================
 * ESP32-S3-RLCD-4.2 原板：无触摸；板载 ES8311(0x18) + ES7210(0x40/0x42) + SHTC3 + PCF85063A。
 * ★ 三个物理键，但**只有两个是 GPIO**（据官方原理图 + ESPHome 教程 + Zephyr 移植三处一致）：
 *     BOOT = GPIO0（可读）· KEY = GPIO18（可读）· **PWR 走电源锁存 IC U3/ECJ23001，不是 GPIO**
 *   ⇒ PWR 长按由**硬件直接断电**，固件既读不到也不该管；所以这里 `btn_pwr` 虽填 18，
 *      实际只用来做 **EXT1 唤醒掩码 / 唤醒门控**（EXT1 只能挂可读的 KEY），
 *      不是"固件电源键"—— input 层检测到与 KEY 同脚会自动禁用 PWR 语义。 */
static const board_pins_t BP_WEIXUE = {
    .id = BOARD_ID_WEIXUE, .name = "微雪原版",
    .lcd_dc = 5, .lcd_cs = 40, .lcd_sck = 11, .lcd_mosi = 12, .lcd_rst = 41,   /* RST 必须驱动并拉高 */
    .lcd_freq_mhz = 33,          /* 同小智: 33MHz 请求 → 实际 80/3≈26.7MHz, 见上 */
    /* 触摸: 原板没有；这两个字段留给"微雪 + 自己加触摸"(见标准扩展版) */
    .tp_sda = 15, .tp_scl = 7, .tp_int = 17, .tp_rst = 2,
    .btn_pwr = 18, .btn_right = 0, .btn_left = 18,     /* PWR 非 GPIO（硬件锁存），18 = KEY，仅用于唤醒 */
    .bat_adc_chan = 3,                     /* ADC1_CH3 = GPIO4 */
    .chrg = -1,                            /* 微雪原版: 无此信号 */
    .i2s_mclk = 16, .i2s_bclk = 9, .i2s_ws = 45, .i2s_dout = 8, .i2s_din = 10,
    .codec_sda = 13, .codec_scl = 14, .pa_pin = 46,
    .vib = 48,
    .gps_tx = 43, .gps_rx = 44, .gps_extint = 47,
    .sd_bus = 0, .sd_clk = 38, .sd_cmd = 21, .sd_d0 = 39,
};

/* ==================== ③ 标准扩展版 ====================
 * 官方标准 ESP32-S3 模块 + 按需扩展：屏幕同微雪；有触摸(15/7/17/2)；
 * 音频走 NS4168 纯功放（**无 MCLK、无录音**）；三个键各自独立（电源 1 / 返回 0 / 确认 18）。 */
static const board_pins_t BP_STD = {
    .id = BOARD_ID_STD, .name = "标准扩展版",
    .lcd_dc = 5, .lcd_cs = 40, .lcd_sck = 11, .lcd_mosi = 12, .lcd_rst = 41,
    .lcd_freq_mhz = 33,          /* 同小智: 33MHz 请求 → 实际 80/3≈26.7MHz, 见上 */
    .tp_sda = 15, .tp_scl = 7, .tp_int = 17, .tp_rst = 2,
    .btn_pwr = 1, .btn_right = 0, .btn_left = 18,     /* 电源 / 返回 / 确认（三脚独立, linit 2026-10-18 更正） */
    .bat_adc_chan = 3,                                 /* 同微雪: GPIO4 */
    .chrg = -1,                                        /* 标准扩展版: 按需自己加 */
    .i2s_mclk = -1, .i2s_bclk = 9, .i2s_ws = 45, .i2s_dout = 8, .i2s_din = -1,
    .codec_sda = -1, .codec_scl = -1, .pa_pin = 46,
    .vib = 48,
    .gps_tx = 43, .gps_rx = 44, .gps_extint = 47,
    .sd_bus = 0, .sd_clk = 38, .sd_cmd = 21, .sd_d0 = 39,
};

/* 当前生效的表：开机先用"探测全失败时的兜底默认"，hw_cfg 探到就覆盖 */
static const board_pins_t *s_cur = (BOARD_DEFAULT_ID == BOARD_ID_WEIXUE) ? &BP_WEIXUE : &BP_XIAOZHI;

void board_pins_set_id(board_id_t id) { s_cur = board_pins_of(id); }

const board_pins_t *board_pins_of(board_id_t id) {
    switch (id) {
        case BOARD_ID_XIAOZHI: return &BP_XIAOZHI;
        case BOARD_ID_WEIXUE:  return &BP_WEIXUE;
        case BOARD_ID_STD:     return &BP_STD;
        default:               return s_cur ? s_cur : &BP_XIAOZHI;
    }
}

const board_pins_t *board_pins_get(void) { return s_cur; }

bool board_pins_select(board_id_t id) {
    const board_pins_t *p = board_pins_of(id);
    if (p == s_cur) return false;
    s_cur = p;
    ESP_LOGI(TAG, "板型切换到: %s", p->name);
    return true;
}

uint64_t board_wake_mask(void) {
    uint64_t m = 0;
    if (s_cur->btn_pwr  >= 0) m |= (1ULL << s_cur->btn_pwr);
    if (s_cur->btn_left >= 0) m |= (1ULL << s_cur->btn_left);
    return m;
}
