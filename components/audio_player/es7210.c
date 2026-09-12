/* ES7210 四声道 ADC 驱动 (微雪 ESP32-S3-RLCD-4.2 双麦克风阵列)
 * 参考: ESPHome es7210 组件 / esp-adf es7210 驱动
 * 配置: I2S 从模式, 16kHz, 16bit, 立体声 (SDOUT1 = MIC1/2)
 * I2C 地址: 0x40 (AD0 低) / 0x41 (AD0 高), 自动探测 */

#include "es7210.h"

#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ES7210";

/* 寄存器 */
#define ES7210_RESET_REG00       0x00
#define ES7210_CLOCK_OFF_REG01   0x01
#define ES7210_MAINCLK_REG02     0x02
#define ES7210_LRCK_DIVH_REG04   0x04
#define ES7210_LRCK_DIVL_REG05   0x05
#define ES7210_POWER_DOWN_REG06  0x06
#define ES7210_OSR_REG07         0x07
#define ES7210_MODE_CONFIG_REG08 0x08
#define ES7210_TIME_CONTROL0_REG09 0x09
#define ES7210_TIME_CONTROL1_REG0A 0x0A
#define ES7210_SDP_INTERFACE1_REG11 0x11
#define ES7210_SDP_INTERFACE2_REG12 0x12
#define ES7210_ADC34_HPF2_REG20  0x20
#define ES7210_ADC34_HPF1_REG21  0x21
#define ES7210_ADC12_HPF1_REG22  0x22
#define ES7210_ADC12_HPF2_REG23  0x23
#define ES7210_ANALOG_REG40      0x40
#define ES7210_MIC12_BIAS_REG41  0x41
#define ES7210_MIC34_BIAS_REG42  0x42
#define ES7210_MIC1_GAIN_REG43   0x43
#define ES7210_MIC2_GAIN_REG44   0x44
#define ES7210_MIC3_GAIN_REG45   0x45
#define ES7210_MIC4_GAIN_REG46   0x46
#define ES7210_MIC1_POWER_REG47  0x47
#define ES7210_MIC2_POWER_REG48  0x48
#define ES7210_MIC3_POWER_REG49  0x49
#define ES7210_MIC4_POWER_REG4A  0x4A
#define ES7210_MIC12_POWER_REG4B 0x4B
#define ES7210_MIC34_POWER_REG4C 0x4C

static i2c_port_t s_port = I2C_NUM_0;
static uint8_t   s_addr = 0;
static bool      s_initialized = false;

static int es7210_write_reg(uint8_t reg, uint8_t val) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (s_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_write_byte(cmd, val, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(s_port, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C 写 0x%02x=0x%02x 失败: %s", reg, val, esp_err_to_name(ret));
    }
    return (ret == ESP_OK) ? 0 : -1;
}

static int es7210_read_reg(uint8_t reg, uint8_t *val) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (s_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(s_port, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    if (ret != ESP_OK) return -1;

    cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (s_addr << 1) | I2C_MASTER_READ, true);
    i2c_master_read_byte(cmd, val, I2C_MASTER_LAST_NACK);
    i2c_master_stop(cmd);
    ret = i2c_master_cmd_begin(s_port, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return (ret == ESP_OK) ? 0 : -1;
}

static int es7210_update_reg_bit(uint8_t reg, uint8_t mask, uint8_t data) {
    uint8_t v = 0;
    if (es7210_read_reg(reg, &v) != 0) return -1;
    v = (uint8_t)((v & ~mask) | (mask & data));
    return es7210_write_reg(reg, v);
}

int es7210_probe(int i2c_port) {
    i2c_port_t old = s_port;
    s_port = i2c_port;
    s_addr = 0;
    for (uint8_t a = 0x40; a <= 0x43; a++) {
        s_addr = a;   /* 必须先设置地址, es7210_read_reg 用它 */
        uint8_t v = 0;
        if (es7210_read_reg(0x00, &v) == 0) {
            s_addr = a;
            break;
        }
    }
    uint8_t found = s_addr;
    s_addr = 0;
    s_port = old;
    return found;
}

int es7210_init(int i2c_port) {
    if (s_initialized) return 0;
    s_port = i2c_port;

    /* 探测地址 0x40..0x43 (读寄存器, 比空写 ACK 更可靠) */
    s_addr = (uint8_t)es7210_probe(i2c_port);
    if (s_addr == 0) {
        ESP_LOGE(TAG, "未找到 ES7210 (0x40-0x43 均无响应)");
        return -1;
    }
    ESP_LOGI(TAG, "ES7210 地址 0x%02X, 初始化 16kHz ADC...", s_addr);

    /* 软件复位 */
    es7210_write_reg(ES7210_RESET_REG00, 0xFF);
    es7210_write_reg(ES7210_RESET_REG00, 0x32);
    es7210_write_reg(ES7210_CLOCK_OFF_REG01, 0x3F);

    /* 上电初始化时间 */
    es7210_write_reg(ES7210_TIME_CONTROL0_REG09, 0x30);
    es7210_write_reg(ES7210_TIME_CONTROL1_REG0A, 0x30);

    /* HPF (高通滤波) */
    es7210_write_reg(ES7210_ADC12_HPF2_REG23, 0x2A);
    es7210_write_reg(ES7210_ADC12_HPF1_REG22, 0x0A);
    es7210_write_reg(ES7210_ADC34_HPF2_REG20, 0x0A);
    es7210_write_reg(ES7210_ADC34_HPF1_REG21, 0x2A);

    /* 从模式 */
    es7210_update_reg_bit(ES7210_MODE_CONFIG_REG08, 0x01, 0x00);

    /* 模拟电源 + 麦克风偏置 */
    es7210_write_reg(ES7210_ANALOG_REG40, 0xC3);
    es7210_write_reg(ES7210_MIC12_BIAS_REG41, 0x70);
    es7210_write_reg(ES7210_MIC34_BIAS_REG42, 0x70);

    /* I2S 格式: 16bit, 非 TDM (MIC1/2 → SDOUT1) */
    es7210_write_reg(ES7210_SDP_INTERFACE1_REG11, 0x60);
    es7210_write_reg(ES7210_SDP_INTERFACE2_REG12, 0x00);

    /* 16kHz @ MCLK=256*16k=4.096MHz:
     * adc_div=0x01 | doubler<<6=0x40 | dll<<7=0x80 */
    es7210_write_reg(ES7210_MAINCLK_REG02, 0xC1);
    es7210_write_reg(ES7210_OSR_REG07, 0x20);
    es7210_write_reg(ES7210_LRCK_DIVH_REG04, 0x01);
    es7210_write_reg(ES7210_LRCK_DIVL_REG05, 0x00);

    /* 增益: 30dB (0x0A | 0x10) */
    for (uint8_t r = ES7210_MIC1_GAIN_REG43; r <= ES7210_MIC4_GAIN_REG46; r++) {
        es7210_write_reg(r, 0x1A);
    }

    /* 上电: MIC 电源 + ADC/PGA + 偏置 */
    es7210_write_reg(ES7210_MIC1_POWER_REG47, 0x08);
    es7210_write_reg(ES7210_MIC2_POWER_REG48, 0x08);
    es7210_write_reg(ES7210_MIC3_POWER_REG49, 0x08);
    es7210_write_reg(ES7210_MIC4_POWER_REG4A, 0x08);
    es7210_write_reg(ES7210_POWER_DOWN_REG06, 0x04);
    es7210_write_reg(ES7210_MIC12_POWER_REG4B, 0x0F);
    es7210_write_reg(ES7210_MIC34_POWER_REG4C, 0x0F);

    /* 使能 */
    es7210_write_reg(ES7210_RESET_REG00, 0x71);
    es7210_write_reg(ES7210_RESET_REG00, 0x41);

    s_initialized = true;
    ESP_LOGI(TAG, "ES7210 初始化完成");
    return 0;
}

void es7210_deinit(void) {
    if (!s_initialized) return;
    es7210_write_reg(ES7210_RESET_REG00, 0x32);
    es7210_write_reg(ES7210_CLOCK_OFF_REG01, 0x3F);
    s_initialized = false;
    ESP_LOGI(TAG, "ES7210 已关闭");
}
