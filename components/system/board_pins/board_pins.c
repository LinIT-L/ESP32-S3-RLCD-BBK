/* board_pins.c — 板型表 + 开机自己识别 (2026-10-16)
 *
 * 目的: **换板不刷固件** —— 开机最早识别板型, 之后所有外设按识别出的表初始化。
 * 识别手段: I2C 上的**器件指纹**(两块板的器件组合互补):
 *   小智模块板 总线 IO38/47 → 有 ES8311(0x18) + **ES7210**(0x40..0x43, 四路麦 ADC)
 *   微雪原板   总线 IO13/14 → 有 ES8311(0x18) + **QMI8658C**(0x6A/0x6B) 或 RTC(0x51/0x32)
 * 只用 ES8311 不够 —— 两块板都有它; 所以用"另一类器件"来做判别。
 *
 * ⛔ 必须在任何外设初始化**之前**调用:
 *   - 只在 I2C 上扫描, 探完立刻 i2c_driver_delete() 释放;
 *   - 不驱动任何高电平输出、不碰电源/功放/strapping 脚。
 */
#include "board_pins.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/i2c.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

#define TAG "BOARD"

/* ==================== 两张板型表 ==================== */
static const board_pins_t BP_XIAOZHI = {
    .id = BOARD_ID_XIAOZHI, .name = "小智模块+新载板",
    .lcd_dc = 7, .lcd_cs = 11, .lcd_sck = 9, .lcd_mosi = 10, .lcd_rst = -1,
    .lcd_freq_mhz = 60,   /* ★2026-10-16 与微雪统一为 60MHz(同一块 ST7305 面板) */,
    .tp_sda = 5, .tp_scl = 13, .tp_int = 12, .tp_rst = 6,
    .btn_pwr = 1, .btn_right = 0, .btn_left = 4,
    .bat_adc_chan = 2,                     /* ADC1_CH2 = GPIO3 (模块内部电池分压) */
    .i2s_mclk = 40, .i2s_bclk = 39, .i2s_ws = 45, .i2s_dout = 48, .i2s_din = 21,
    .codec_sda = 38, .codec_scl = 47, .pa_pin = 46,
    .sd_bus = 0,                                   /* SDMMC 1-bit */
    .sd_clk = 16, .sd_cmd = 15, .sd_d0 = 17,
};

static const board_pins_t BP_WEIXUE = {
    .id = BOARD_ID_WEIXUE, .name = "微雪 ESP32-S3-RLCD-4.2",
    /* ★★★ 2026-10-16 **完整性修复**: 逐条照抄 linit 那个能点亮的 v1.3 版
     *   (仓库 LinIT-L/ESP32-S3-RLCD-BBK 的 v1.3 标签, 从驱动默认值/头文件里逐行抓出来的),
     *   一处不漏、不与 LCD 撞脚。 */
    /* 微雪 LCD: 照 v1.3(能亮那版)的值; **实际接法待 linit 逐根确认, 不猜** */
    .lcd_dc = 5, .lcd_cs = 40, .lcd_sck = 11, .lcd_mosi = 12, .lcd_rst = 41,
    .lcd_freq_mhz = 60,
    /* 触摸: 15/7/17/2 (与 LCD 的 5/40/11/12 不冲突) */
    .tp_sda = 15, .tp_scl = 7, .tp_int = 17, .tp_rst = 2,
    /* 按键: 板载 KEY=18(左/确认) · BOOT=0(右) */
    .btn_pwr = 18, .btn_right = 0, .btn_left = 18,
    /* 电池 ADC: ADC1_CH3 = GPIO4 */
    .bat_adc_chan = 3,
    /* 音频: MCLK16 BCLK9 WS45 DOUT8 DIN10 · codec I2C 13/14 · PA 46 */
    .i2s_mclk = 16, .i2s_bclk = 9, .i2s_ws = 45, .i2s_dout = 8, .i2s_din = 10,
    .codec_sda = 13, .codec_scl = 14, .pa_pin = 46,
    /* SD: SDMMC 1-bit, CLK38 CMD21 D0 39 */
    .sd_bus = 0, .sd_clk = 38, .sd_cmd = 21, .sd_d0 = 39,
};




static const board_pins_t *s_cur = BOARD_IS_WEIXUE ? &BP_WEIXUE : &BP_XIAOZHI;

const board_pins_t *board_pins_of(board_id_t id) {
    return (id == BOARD_ID_WEIXUE) ? &BP_WEIXUE : &BP_XIAOZHI;
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
    if (s_cur->btn_pwr   >= 0) m |= (1ULL << s_cur->btn_pwr);
    if (s_cur->btn_left  >= 0) m |= (1ULL << s_cur->btn_left);
    return m;
}

/* ==================== NVS 缓存 ==================== */
#define NVS_NS   "board"
#define NVS_KEY  "id"

static int nvs_load(void) {
    nvs_handle_t h; int32_t v = BOARD_ID_UNKNOWN;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return BOARD_ID_UNKNOWN;
    if (nvs_get_i32(h, NVS_KEY, &v) != ESP_OK) v = BOARD_ID_UNKNOWN;
    nvs_close(h);
    return (int)v;
}
void board_pins_save(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, NVS_KEY, (int32_t)s_cur->id);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "板型已记入 NVS: %d", (int)s_cur->id);
}

/* ==================== I2C 指纹 ==================== */
/* 在某组脚上探一个从机地址; 探完**立刻**释放总线 (绝不长期占脚) */
static bool probe_addr(int sda, int scl, uint8_t addr) {
    if (sda < 0 || scl < 0) return false;
    i2c_config_t c = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = sda, .scl_io_num = scl,
        .sda_pullup_en = true, .scl_pullup_en = true,
        .master.clk_speed = 100000,
    };
    if (i2c_param_config(I2C_NUM_0, &c) != ESP_OK) return false;
    if (i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0) != ESP_OK) return false;
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (uint8_t)((addr << 1) | I2C_MASTER_WRITE), true);
    i2c_master_stop(cmd);
    esp_err_t r = ESP_FAIL;
    for (int t = 0; t < 3 && r != ESP_OK; t++) {      /* ★ 重试 3 次: 上电早期器件可能还没就绪 */
        i2c_cmd_handle_t c2 = i2c_cmd_link_create();
        i2c_master_start(c2);
        i2c_master_write_byte(c2, (uint8_t)((addr << 1) | I2C_MASTER_WRITE), true);
        i2c_master_stop(c2);
        r = i2c_master_cmd_begin(I2C_NUM_0, c2, pdMS_TO_TICKS(80));
        i2c_cmd_link_delete(c2);
        if (r != ESP_OK) vTaskDelay(pdMS_TO_TICKS(20));
    }
    i2c_cmd_link_delete(cmd);
    i2c_driver_delete(I2C_NUM_0);          /* ★ 立刻释放, 别占着引脚 */
    return (r == ESP_OK);
}
static bool probe_any(int sda, int scl, const uint8_t *addrs, int n) {
    for (int i = 0; i < n; i++)
        if (probe_addr(sda, scl, addrs[i])) return true;
    return false;
}
/* 音频芯片指纹 (linit 2026-10-16: 就用音频芯片判断):
 *   两块板**都有** ES8311(0x18) —— 光凭它分不开;
 *   小智模块还多一颗 **ES7210 四路麦 ADC**(地址 0x40~0x43, 由 AD1/AD0 决定);
 *   微雪原板没有 ES7210(它的麦走 ES8311 自带 ADC)。
 *   ⇒ **有 ES8311 + ES7210 = 小智; 只有 ES8311 = 微雪**(再拿板载 IMU/RTC 做二次确认)。 */
/* 音频芯片地址: ES8311 的 0x18/0x19 由 CE 脚决定 ⇒ **两个都试** */
static bool has_es8311(int sda, int scl) {
    return probe_addr(sda, scl, 0x18) || probe_addr(sda, scl, 0x19);
}
/* 小智独有: ES7210 四路麦 ADC (地址由 AD1/AD0 决定, 0x40~0x43) */
static bool has_es7210(int sda, int scl) {
    static const uint8_t a[] = { 0x40, 0x41, 0x42, 0x43 };
    return probe_any(sda, scl, a, (int)(sizeof(a) / sizeof(a[0])));
}
/* 微雪: 音频芯片(ES8311) 或 板载 QMI8658C IMU / RTC */
static bool sig_weixue(int sda, int scl) {
    static const uint8_t aux[] = { 0x6A, 0x6B, 0x51, 0x32 };
    if (has_es8311(sda, scl)) return true;
    return probe_any(sda, scl, aux, (int)(sizeof(aux) / sizeof(aux[0])));
}
/* 小智: ES7210 (它独有的音频芯片) */
static bool sig_xiaozhi(int sda, int scl) {
    return has_es7210(sda, scl);
}

board_id_t board_identify(void) {
    /* ★ 2026-10-16 linit 要求删除板型识别: 不做任何 I2C 扫描/NVS, 直接用编译期开关。 */
    ESP_LOGI(TAG, "板型(编译期固定): %s", s_cur->name);
    return s_cur->id;
}
