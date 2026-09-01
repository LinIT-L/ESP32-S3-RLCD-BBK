/**
 * @file vibrator.c
 * @brief 震动马达驱动 (V1.3.x: TM6604 线性驱动, PWM 直驱 LRA)
 *
 * TM6604 为国产线性马达驱动 IC (SOT23-6, PWM 输入, 差动桥式输出).
 * 输入 PWM 频率应等于 LRA 谐振频率 (常见 150~250Hz), 占空比控制输出强度;
 * EN 高有效, 拉低即关断输出省电. 芯片无寄存器/无探测手段, 上电即用.
 *
 * 接线 (TM6604 SOT23-6):
 *   Pin1 OUT+ ──┐        ┌── Pin3 OUT-
 *   Pin2 GND  ── GND
 *   Pin4 VDD  ── 3.3V
 *   Pin5 PWM  ── GPIO42 (原 DRV2605L SDA 焊盘, LEDC 190Hz 方波)
 *   Pin6 EN   ── GPIO48 (原 EN IO, 保持不变)
 * 注: 原 SCL=GPIO47 空置; 原 GPIO6/LEDC 兜底路径已并入 PWM 主路径.
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

/* ============ 驱动方式 ============ */
typedef enum { VIB_NONE = 0, VIB_TM6604 = 1 } vib_mode_t;

/* ---- TM6604 PWM 直驱 LRA ---- */
#define VIB_PWM_PIN   GPIO_NUM_42  /* 原 DRV2605L SDA 焊盘; 避开 strapping/ADC 脚 */
#define VIB_EN_PIN    GPIO_NUM_48  /* 保持原 EN IO (新板震动开关) */
#define VIB_TIMER     LEDC_TIMER_1
#define VIB_CHANNEL   LEDC_CHANNEL_1
#define VIB_RES       LEDC_TIMER_8_BIT
#define VIB_RES_MAX   255
/* PWM 频率 = LRA 谐振频率: 常见 150~250Hz, 不同马达按实际谐振点调整. */
#define VIB_RES_FREQ  190

static vib_mode_t s_mode = VIB_NONE;
static bool       s_enabled = true;   /* 硬件总开关: ui||game 任一开则开 */
static bool       s_vib_ui   = true;  /* UI 震动开关 (拖拉/菜单导航/设置调整) */
static bool       s_vib_game = true;  /* 游戏震动开关 (游戏内按键) */
static TimerHandle_t s_tap_timer = NULL;

/* ================= TM6604 EN (GPIO48, 保持原 IO) ================= */
/* 引脚有效性防护: 仅操作本模组(ESP32-S3-WROOM-1)引出的普通 IO.
 * 22-34 未引出 (含 26), 33-37 为 PSRAM/Flash 专用. */
static bool vib_pin_supported(int pin) {
    if (pin < 0 || pin > 48) return false;
    if (pin >= 22 && pin <= 34) return false;
    if (pin >= 33 && pin <= 37) return false;
    return true;
}
static void drv_enable(bool on) {
    if (!vib_pin_supported(VIB_EN_PIN)) return;
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << VIB_EN_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = false, .pull_down_en = false,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(VIB_EN_PIN, on ? 1 : 0);
}

/* ================= PWM 输出 (LEDC) ================= */
static bool ledc_init(void) {
    /* 先复位引脚为纯输出, 清除原 SDA 焊盘可能残留的 OD/上拉配置 */
    gpio_reset_pin(VIB_PWM_PIN);
    ledc_timer_config_t tc = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .timer_num = VIB_TIMER,
        .duty_resolution = VIB_RES, .freq_hz = VIB_RES_FREQ, .clk_cfg = LEDC_AUTO_CLK,
    };
    if (ledc_timer_config(&tc) != ESP_OK) return false;
    ledc_channel_config_t ch = {
        .gpio_num = VIB_PWM_PIN, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = VIB_CHANNEL,
        .timer_sel = VIB_TIMER, .duty = 0, .hpoint = 0,
    };
    if (ledc_channel_config(&ch) != ESP_OK) return false;
    ledc_update_duty(LEDC_LOW_SPEED_MODE, VIB_CHANNEL);
    return true;
}

/* ================= 实际输出 ================= */
static void out_set(uint8_t strength01) {   /* strength 0..100 */
    if (s_mode == VIB_NONE) return;
    uint32_t duty = ((uint32_t)strength01 * VIB_RES_MAX) / 100;
    if (duty > VIB_RES_MAX) duty = VIB_RES_MAX;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, VIB_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, VIB_CHANNEL);
}
static void out_off(void) {
    if (s_mode == VIB_NONE) return;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, VIB_CHANNEL, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, VIB_CHANNEL);
}

/* 定时到点关闭震动 (非阻塞 tap 用) */
static void tap_timer_cb(TimerHandle_t t) { (void)t; out_off(); }

/* ================= 对外接口 ================= */
void vibrator_init(void) {
    if (s_mode != VIB_NONE) return;
    drv_enable(true);      /* TM6604 EN 高使能 (GPIO48) */
    if (!ledc_init()) {
        ESP_LOGE(TAG, "LEDC PWM 配置失败 (GPIO%d), 震动不可用", (int)VIB_PWM_PIN);
        return;
    }
    s_mode = VIB_TM6604;
    s_tap_timer = xTimerCreate("vib_tap", pdMS_TO_TICKS(50), pdFALSE, NULL, tap_timer_cb);
    ESP_LOGI(TAG, "震动驱动就绪: TM6604 PWM=GPIO%d(%dHz) EN=GPIO%d", (int)VIB_PWM_PIN,
             VIB_RES_FREQ, (int)VIB_EN_PIN);
}

bool vibrator_ready(void) { return s_mode != VIB_NONE; }

/* V1.1.0: 总开关 (由「震动反馈」设置调用) */
void vibrator_set_enabled(bool en) {
    s_enabled = en;
    if (!en) {
        out_off();
        drv_enable(false);   /* 关震动开关: 一并拉低 TM6604 EN, 省电 */
    } else {
        drv_enable(true);    /* 开: 使能 TM6604 */
    }
}

/* V1.2.x: 两个独立震动通道 (UI / 游戏) 的开关与触发.
 * 任一通道开 → 硬件总开关开; 两者都关 → 关断 TM6604 EN 省电.
 * 上层 (设置-声音震动) 调用这两个 setter 而非 vibrator_set_enabled. */
void vibrator_set_ui_enabled(bool en) {
    s_vib_ui = en;
    vibrator_set_enabled(s_vib_ui || s_vib_game);
}
void vibrator_set_game_enabled(bool en) {
    s_vib_game = en;
    vibrator_set_enabled(s_vib_ui || s_vib_game);
}
void vibrator_tap_ui(uint16_t ms, uint8_t strength) {
    if (s_vib_ui) vibrator_tap(ms, strength);
}
void vibrator_tap_game(uint16_t ms, uint8_t strength) {
    if (s_vib_game) vibrator_tap(ms, strength);
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
