/**
 * @file vibrator.c
 * @brief 震动马达驱动 (V1.1.0 重构: 优先 DRV2605L 驱动 LRA 线性马达, 兜底 GPIO6/LEDC)
 *
 * 两级驱动:
 *  1) DRV2605L (I2C 0x5A, 独立软件I2C): 闭环驱动 LRA, 使用 Real-Time 播放模式
 *     - 写 0x02 实时幅度即起振, 写 0x00 即停, 时长/强度由主控精确控制.
 *     - 接线: DRV2605L SDA/SDA -> GPIO, 见下方宏; 马达接 OUT+/OUT-; EN 接高.
 *  2) 兜底 (探测不到 DRV2605L): GPIO6 --LEDC PWM(谐振频率方波)--> N-MOS --> 裸 LRA.
 *
 * 提供非阻塞的单次震动 vibrator_tap(ms, strength) 供按键/闹钟/计时器调用.
 */
#include "vibrator.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>

#define TAG "VIB"

/* ============ 驱动方式选择 ============ */
typedef enum { VIB_NONE = 0, VIB_DRV2605 = 1, VIB_LEDC = 2 } vib_mode_t;

/* ---- DRV2605L 独立软件 I2C ---- (两块硬件 I2C 控制器已被音频/触摸占用) */
#define DRV_SDA_PIN   GPIO_NUM_42  /* 空闲非strapping脚; 避开 GPIO3/45/46(strapping) 与 GPIO16(电池ADC) */
#define DRV_SCL_PIN   GPIO_NUM_47
#define DRV_ADDR      0x5A
/* DRV2605L 使能脚 (EN) = GPIO48: 新板震动开关. 模组已引出, 实机验证安全可用.
 * (此前误用不存在的 IO26 曾致卡; 48 为正确引脚.) */
#define DRV_EN_PIN    GPIO_NUM_48
/* Real-Time 播放模式所用寄存器 */
#define DRV_REG_STATUS 0x00
#define DRV_REG_MODE   0x01   /* bit6=STANDBY, bit0.. 模式 */
#define DRV_REG_RTP    0x02   /* Real-Time Playback 幅度 (写0停) */
#define DRV_REG_LIB    0x03
#define DRV_REG_FBACK  0x1A   /* bit7: 1=LRA, 0=ERM; [6:4]刹车;[3:2]环路增益 */
#define DRV_REG_RATEDV 0x16   /* 额定电压 */
#define DRV_REG_ODCLAMP 0x17  /* 过驱限幅 */

#define DRV_MODE_RTP   0x05   /* Real-Time 播放 */
#define DRV_MAX_AMP    127    /* RTP 满幅 (0~127) */

/* ---- 兜底 LEDC (原 Taptic 单极性驱动) ---- */
#define VIB_GPIO        GPIO_NUM_6
#define VIB_TIMER       LEDC_TIMER_1
#define VIB_CHANNEL     LEDC_CHANNEL_1
#define VIB_RES         LEDC_TIMER_8_BIT
#define VIB_RES_MAX     255
#define VIB_RES_FREQ    190
#define VIB_DEFAULT_DUTY 200

static vib_mode_t s_mode = VIB_NONE;
static bool       s_enabled = true;   /* 总开关: 由「震动反馈」设置控制 */
static TimerHandle_t s_tap_timer = NULL;
static uint8_t     s_now_amp = 0;     /* 当前实时幅度 (DRV) */

/* ================= 软件 I2C ================= */
#define I2C_HALF_US 5   /* ~100kHz */

static void i2c_delay(void) { esp_rom_delay_us(I2C_HALF_US); }
static void sda_out(bool high) { gpio_set_level(DRV_SDA_PIN, high); }
static void scl_out(bool high) { gpio_set_level(DRV_SCL_PIN, high); }
static int  sda_in(void)        { return gpio_get_level(DRV_SDA_PIN); }

static void sw_i2c_start(void) {
    sda_out(1); scl_out(1); i2c_delay();
    sda_out(0); i2c_delay();           /* SDA 在 SCL 高时拉低 = START */
    scl_out(0); i2c_delay();
}
static void sw_i2c_stop(void) {
    sda_out(0); scl_out(1); i2c_delay();
    sda_out(1); i2c_delay();           /* SCL 高时 SDA 拉高 = STOP */
}
static bool sw_i2c_write_byte(uint8_t b) {
    for (int i = 7; i >= 0; i--) {
        sda_out((b >> i) & 1); i2c_delay();
        scl_out(1); i2c_delay();
        scl_out(0); i2c_delay();
    }
    sda_out(1); i2c_delay();           /* 释放 SDA 等 ACK */
    scl_out(1); i2c_delay();
    bool ack = (sda_in() == 0);
    scl_out(0); i2c_delay();
    return ack;
}
static uint8_t sw_i2c_read_byte(bool last) {
    uint8_t b = 0;
    gpio_set_direction(DRV_SDA_PIN, GPIO_MODE_INPUT);   /* 释放读数据 */
    gpio_set_pull_mode(DRV_SDA_PIN, GPIO_PULLUP_ONLY);
    for (int i = 0; i < 8; i++) {
        scl_out(1); i2c_delay();
        b = (uint8_t)((b << 1) | (sda_in() ? 1 : 0));
        scl_out(0); i2c_delay();
    }
    sda_out(last ? 1 : 0);             /* NACK(主在末字节)/ACK */
    gpio_set_direction(DRV_SDA_PIN, GPIO_MODE_INPUT_OUTPUT_OD);
    i2c_delay();
    scl_out(1); i2c_delay(); scl_out(0); i2c_delay();
    return b;
}
static void drv_write_reg(uint8_t reg, uint8_t val) {
    sw_i2c_start();
    sw_i2c_write_byte((DRV_ADDR << 1) | 0);
    sw_i2c_write_byte(reg);
    sw_i2c_write_byte(val);
    sw_i2c_stop();
}
static bool drv_read_reg(uint8_t reg, uint8_t *out) {
    sw_i2c_start();
    if (!sw_i2c_write_byte((DRV_ADDR << 1) | 0)) { sw_i2c_stop(); return false; }
    if (!sw_i2c_write_byte(reg)) { sw_i2c_stop(); return false; }
    sw_i2c_start();                       /* 重复 START 转读 */
    if (!sw_i2c_write_byte((DRV_ADDR << 1) | 1)) { sw_i2c_stop(); return false; }
    *out = sw_i2c_read_byte(true);
    sw_i2c_stop();
    return true;
}

/* 探测 DRV2605L 是否存在: 读 STATUS, DeviceID[7:5] 应为 111 (0xE0) */
static bool drv2605_probe(void) {
    uint8_t s = 0;
    if (!drv_read_reg(DRV_REG_STATUS, &s)) return false;
    return ((s >> 5) == 0x07);   /* bits[7:5] == 111 */
}

static void drv2605_init_lra(void) {
    drv_write_reg(DRV_REG_MODE, DRV_MODE_RTP);          /* Real-Time 播放, 退出待机 */
    drv_write_reg(DRV_REG_FBACK, 0xB4);                 /* LRA + 刹车3x + 环路中增益 */
    drv_write_reg(DRV_REG_LIB, 0x00);
    drv_write_reg(DRV_REG_RATEDV, 0x50);                /* 额定电压安全中值 */
    drv_write_reg(DRV_REG_ODCLAMP, 0x68);               /* 过驱限幅 */
    drv_write_reg(DRV_REG_RTP, 0x00);                   /* 先停 */
    ESP_LOGI(TAG, "DRV2605L ready (LRA real-time)");
}

/* ================= 兜底 LEDC ================= */
static void ledc_init(void) {
    ledc_timer_config_t tc = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .timer_num = VIB_TIMER,
        .duty_resolution = VIB_RES, .freq_hz = VIB_RES_FREQ, .clk_cfg = LEDC_AUTO_CLK,
    };
    if (ledc_timer_config(&tc) != ESP_OK) return;
    ledc_channel_config_t ch = {
        .gpio_num = VIB_GPIO, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = VIB_CHANNEL,
        .timer_sel = VIB_TIMER, .duty = 0, .hpoint = 0,
    };
    if (ledc_channel_config(&ch) != ESP_OK) return;
    ledc_update_duty(LEDC_LOW_SPEED_MODE, VIB_CHANNEL);
}

/* ================= 实际输出 ================= */
static void out_set(uint8_t strength01) {   /* strength 0..100 */
    if (s_mode == VIB_DRV2605) {
        s_now_amp = (uint8_t)((strength01 * DRV_MAX_AMP) / 100);
        if (s_now_amp > DRV_MAX_AMP) s_now_amp = DRV_MAX_AMP;
        drv_write_reg(DRV_REG_RTP, s_now_amp);
    } else if (s_mode == VIB_LEDC) {
        uint32_t duty = (strength01 * VIB_RES_MAX) / 100;
        ledc_set_duty(LEDC_LOW_SPEED_MODE, VIB_CHANNEL, duty);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, VIB_CHANNEL);
    }
}
static void out_off(void) {
    if (s_mode == VIB_DRV2605) { s_now_amp = 0; drv_write_reg(DRV_REG_RTP, 0x00); }
    else if (s_mode == VIB_LEDC) {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, VIB_CHANNEL, 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, VIB_CHANNEL);
    }
}

/* 定时到点关闭震动 (非阻塞 tap 用) */
static void tap_timer_cb(TimerHandle_t t) { (void)t; out_off(); }

/* ================= 对外接口 ================= */
/* 引脚有效性防护: 仅操作本模组(ESP32-S3-WROOM-1)引出的普通 IO.
 * 22-34 未引出 (含 26), 33-37 为 PSRAM/Flash 专用. */
static bool vib_pin_supported(int pin) {
    if (pin < 0 || pin > 48) return false;
    if (pin >= 22 && pin <= 34) return false;
    if (pin >= 33 && pin <= 37) return false;
    return true;
}
/* 使能/关断 DRV2605L (EN=GPIO48, 新板震动开关). 校验不过则跳过. */
static void drv_enable(bool on) {
    if (!vib_pin_supported(DRV_EN_PIN)) return;
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << DRV_EN_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = false, .pull_down_en = false,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(DRV_EN_PIN, on ? 1 : 0);
}

void vibrator_init(void) {
    if (s_mode != VIB_NONE) return;
    drv_enable(true);     /* 上电先使能 DRV (GPIO48), 否则 probe 读不到. 旧板无 DRV 时无害. */
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << DRV_SDA_PIN) | (1ULL << DRV_SCL_PIN),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD, .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    sda_out(1); scl_out(1);

    if (drv2605_probe()) {
        drv2605_init_lra();
        s_mode = VIB_DRV2605;
    } else {
        ESP_LOGW(TAG, "未检测到 DRV2605L (焊盘 SDA=GPIO%d SCL=GPIO%d), 回退 GPIO%d/LEDC",
                 DRV_SDA_PIN, DRV_SCL_PIN, VIB_GPIO);
        ledc_init();
        s_mode = VIB_LEDC;
    }
    s_tap_timer = xTimerCreate("vib_tap", pdMS_TO_TICKS(50), pdFALSE, NULL, tap_timer_cb);
    ESP_LOGI(TAG, "震动驱动就绪: mode=%d", (int)s_mode);
}

bool vibrator_ready(void) { return s_mode != VIB_NONE; }

/* V1.1.0: 总开关 (由「震动反馈」设置调用) */
void vibrator_set_enabled(bool en) {
    s_enabled = en;
    if (!en) {
        out_off();
        drv_enable(false);   /* 关震动开关: 一并关断 DRV EN, 省电 */
    } else {
        drv_enable(true);    /* 开: 使能 DRV */
    }
}

void vibrator_set(uint8_t strength) {
    if (s_mode == VIB_NONE || !s_enabled) return;
    out_set(strength);
}

void vibrator_stop(void) {
    if (s_mode == VIB_NONE) return;
    if (s_tap_timer) xTimerStop(s_tap_timer, 0);
    out_off();
}

void vibrator_buzz(uint16_t ms) {
    if (s_mode == VIB_NONE || !s_enabled) return;
    if (ms == 0) { vibrator_stop(); return; }
    out_set(100);
    vTaskDelay(pdMS_TO_TICKS(ms));
    out_off();
}

/* V1.1.0: 非阻塞单次震动: ms 时长, strength 0~100 强度 */
void vibrator_tap(uint16_t ms, uint8_t strength) {
    if (s_mode == VIB_NONE || !s_enabled) return;
    out_set(strength);
    if (s_tap_timer) {
        xTimerStop(s_tap_timer, 0);
        xTimerChangePeriod(s_tap_timer, pdMS_TO_TICKS(ms), 0);
        xTimerStart(s_tap_timer, 0);
    } else {
        vTaskDelay(pdMS_TO_TICKS(ms));
        out_off();
    }
}