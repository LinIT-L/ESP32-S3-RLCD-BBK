/**
 * gps.c — u-blox MAX-M10S 驱动实现 (UART + UBX). 见 gps.h 的接线与设计约定.
 *
 * 关键协议依据 (2026-09-29 查证):
 *   - MAX-M10S (SPG 5.00/5.10) **不支持** UBX-CFG-RXM 的 powerSaveMode;
 *   - 软件待机只能用 **UBX-RXM-PMREQ**, 且 **必须置 force 标志**
 *     (MAX-M10S Integration Manual §2.7.3.2 "Software standby mode");
 *   - 唤醒源由 UBX-RXM-PMREQ 指定: UART RX / EXTINT / SPI CS;
 *   - 参考开源: SparkFun_u-blox_GNSS_Arduino_Library 的 powerOff()/powerOffWithInterrupt()
 *     (常量 VAL_RXM_PMREQ_WAKEUPSOURCE_UARTRX=0x08, EXTINT0=0x20 …)。
 *
 * UBX-RXM-PMREQ 长版 (16B payload, class 0x02 id 0x41):
 *   [0..3]  duration      = 0            不限时, 直到唤醒源触发
 *   [4..7]  flags         = 0x02 | 0x04  bit1 backup + bit2 force
 *   [8..11] wakeupSources = 0x08         UART RX
 *   [12..15] reserved     = 0
 */
#include "gps.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "GPS";

#define UBX_SYNC1       0xB5
#define UBX_SYNC2       0x62
#define UBX_CLS_RXM     0x02
#define UBX_ID_PMREQ    0x41
#define UBX_CLS_MON     0x0A
#define UBX_ID_MONVER   0x04

#define GPS_RX_BUF      1024
#define GPS_TX_BUF      1024

static gps_config_t       s_cfg;
static bool               s_ready = false;
static gps_power_state_t  s_state = GPS_POWER_UNKNOWN;

/* ==== 默认配置 ====
 * ★ 2026-10-05 小智模块板 (分支 board/xiaozhi-s3-core):
 *   TX=43 / RX=44 不变 —— 模块脚 36/37 正是 `RXD0(TXD0)` = **GPIO44/GPIO43**，都在 24 个可用脚内 ✓
 *   ⛔ **EXTINT 从 47 改成 -1(不接)**: GPIO47 在本模块上**没有引出** ——
 *      模块内部拿它当 `codec 的 I2C_SCL`，同时焊盘 24 是 `VSYS_IN`(电源开关进)。
 *      固件再去驱动 47 = 一边搅 codec 控制总线、一边扯电源通路 ⇒ 必须弃用。
 * ⇒ EXTINT 挪到 **GPIO8**(linit 2026-10-05 定)。选它的理由:
 *    8/9/16 是模块上仅剩的三个空闲脚, 其中 **16 = `XTAL_32K_N`**(和 15 = `XTAL_32K_P` 是一对)，
 *    **留给以后上 32.768kHz 晶振做 RTC 低功耗时钟**，所以不用它；
 *    8 是纯普通 IO(TOUCH8 / ADC1_CH7 / SUBSPICS1，没有第二身份)，拿来当输入最省事。
 *    (备选 GPIO9; 若以后要撤, 把下面改回 -1 即可。) */
static const gps_config_t k_default_cfg = {
    .uart_num    = UART_NUM_1,   /* console 是 USB-Serial-JTAG, UART0/1 都空; 用 1 避开 ROM 默认口 */
    .pin_tx      = 43,           /* 模块脚 37 (TXD0) */
    .pin_rx      = 44,           /* 模块脚 36 (RXD0) */
    .pin_extint  = 8,            /* 原 47 在模块上不可用 ⇒ 挪到 8 */
    .baud        = 921600,
    .drive_extint = false,       /* 默认不驱动 EXTINT: 极性未确认, 悬空/误电平会自己叫醒模块 */
};

const gps_config_t *gps_default_config(void) { return &k_default_cfg; }
bool gps_is_ready(void) { return s_ready; }
gps_power_state_t gps_power_state(void) { return s_state; }

/* ---- UBX 组帧 (含 8-bit Fletcher 校验) ---- */
static size_t ubx_build(uint8_t *out, uint8_t cls, uint8_t id, const uint8_t *pl, uint16_t n)
{
    size_t i = 0;
    out[i++] = UBX_SYNC1;
    out[i++] = UBX_SYNC2;
    out[i++] = cls;
    out[i++] = id;
    out[i++] = (uint8_t)(n & 0xFF);
    out[i++] = (uint8_t)(n >> 8);
    if (n && pl) { memcpy(&out[i], pl, n); i += n; }
    uint8_t a = 0, b = 0;
    for (size_t k = 2; k < i; k++) { a = (uint8_t)(a + out[k]); b = (uint8_t)(b + a); }
    out[i++] = a;
    out[i++] = b;
    return i;
}

esp_err_t gps_send_ubx(uint8_t cls, uint8_t id, const uint8_t *payload, uint16_t len)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    uint8_t frame[64];
    if ((size_t)len + 8 > sizeof(frame)) return ESP_ERR_INVALID_SIZE;
    size_t n = ubx_build(frame, cls, id, payload, len);
    int w = uart_write_bytes(s_cfg.uart_num, (const char *)frame, n);
    uart_wait_tx_done(s_cfg.uart_num, pdMS_TO_TICKS(50));
    return (w == (int)n) ? ESP_OK : ESP_FAIL;
}

int gps_read(uint8_t *buf, size_t max_len, uint32_t timeout_ms)
{
    if (!s_ready || !buf) return -1;
    int n = uart_read_bytes(s_cfg.uart_num, buf, max_len, pdMS_TO_TICKS(timeout_ms));
    return n;
}

esp_err_t gps_init(const gps_config_t *cfg)
{
    s_cfg = cfg ? *cfg : k_default_cfg;

    esp_err_t e = uart_driver_install(s_cfg.uart_num, GPS_RX_BUF, GPS_TX_BUF, 0, NULL, 0);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "UART%d 驱动安装失败: %s", (int)s_cfg.uart_num, esp_err_to_name(e));
        return e;
    }
    const uart_config_t uc = {
        .baud_rate  = (int)s_cfg.baud,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(s_cfg.uart_num, &uc));
    uart_set_pin(s_cfg.uart_num, s_cfg.pin_tx, s_cfg.pin_rx,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    if (s_cfg.pin_extint >= 0) {
        gpio_config_t g = {
            .pin_bit_mask = (1ULL << s_cfg.pin_extint),
            .mode = s_cfg.drive_extint ? GPIO_MODE_OUTPUT : GPIO_MODE_INPUT,
            .pull_up_en = s_cfg.drive_extint ? GPIO_PULLUP_DISABLE : GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&g);
        if (s_cfg.drive_extint) gpio_set_level(s_cfg.pin_extint, 0);   /* 不驱动时留低, 见 gps_wakeup */
    }

    s_ready = true;
    s_state = GPS_POWER_ACTIVE;
    ESP_LOGI(TAG, "初始化完成: TX=G%d RX=G%d EXTINT=%s%d @%u bps",
             s_cfg.pin_tx, s_cfg.pin_rx, s_cfg.drive_extint ? "驱动" : "监听",
             s_cfg.pin_extint, (unsigned)s_cfg.baud);
    return ESP_OK;
}

esp_err_t gps_sleep(void)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    uint8_t pl[16];
    memset(pl, 0, sizeof(pl));
    /* duration = 0 */
    pl[4] = 0x02 | 0x04;   /* backup + force (M10 必须 force) */
    pl[8] = 0x08;          /* wakeupSources: UART RX */

    /* 模块可能还在启动/输出 NMEA → 等一会再连发 3 次.
     * 每次发送会先唤醒"已睡下"的模块, 但紧接着的 PMREQ 立刻送回待机, 最终态必为待机. */
    vTaskDelay(pdMS_TO_TICKS(200));
    uart_flush_input(s_cfg.uart_num);
    esp_err_t last = ESP_OK;
    for (int k = 0; k < 3; k++) {
        last = gps_send_ubx(UBX_CLS_RXM, UBX_ID_PMREQ, pl, sizeof(pl));
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    if (last == ESP_OK) {
        s_state = GPS_POWER_SLEEP;
        ESP_LOGI(TAG, "已送 UBX-RXM-PMREQ ×3 → 软件待机 (backup+force, 唤醒源=UART RX; 待机电流 46µA)");
    } else {
        ESP_LOGW(TAG, "UBX-RXM-PMREQ 发送失败: %s", esp_err_to_name(last));
    }
    return last;
}

esp_err_t gps_wakeup(void)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    /* (1) UART RX 唤醒源: 送一个无害的 UBX-MON-VER 轮询 (待机时收到字节即醒) */
    esp_err_t e = gps_send_ubx(UBX_CLS_MON, UBX_ID_MONVER, NULL, 0);

    /* (2) 若 EXTINT 配成输出: 补一个"低→高"脉冲, 覆盖极高/极低两种有效极性 */
    if (s_cfg.pin_extint >= 0 && s_cfg.drive_extint) {
        gpio_set_level(s_cfg.pin_extint, 1);
        vTaskDelay(pdMS_TO_TICKS(50));
        gpio_set_level(s_cfg.pin_extint, 0);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (e == ESP_OK) {
        s_state = GPS_POWER_ACTIVE;
        ESP_LOGI(TAG, "已唤醒 (UBX-MON-VER%s)", s_cfg.drive_extint ? " + EXTINT 脉冲" : "");
    }
    return e;
}
