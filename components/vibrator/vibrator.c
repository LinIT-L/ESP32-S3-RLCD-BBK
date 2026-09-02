/**
 * @file vibrator.c
 * @brief 震动马达驱动 (V1.4.x: TM6604 线性驱动, PWM 直驱 LRA)
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
 * V1.4.x: 触发分三档, 互斥仲裁 (后发起停前者):
 *   - 单次 tap  (vibrator_tap): 按键/菜单/拖动反馈
 *   - 预设花样 (vibrator_play_pattern): 段式序列定时器逐段推进
 *   - 随音乐震动 (vibrator_music_mode): 内部任务采样音频振幅表实时映射
 */
#include "vibrator.h"
#include "audio_player.h"
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
static bool       s_enabled = true;   /* 硬件总开关: ui||game||key_sim 任一开则开 */
static bool       s_vib_ui   = true;  /* UI 震动开关 (拖拉/菜单导航/设置调整) */
static bool       s_vib_game = true;  /* 游戏震动开关 (游戏内按键) */
/* V1.4.x: 按键模拟开关 (触控板式点击反馈, NVS vib_key, 内存默认开) —
 * 与 UI/游戏震动三开关互不重叠: 关 → 触摸点击完全不震. */
static bool       s_key_sim  = true;
static TimerHandle_t s_tap_timer = NULL;
static TimerHandle_t s_pat_timer = NULL;

/* V1.4.x 前向声明: 花样/音乐震动实现位于文件后部, init/set_enabled 先引用 */
static void pat_timer_cb(TimerHandle_t t);
static volatile bool s_music_on;

/* ================= TM6604 EN (GPIO48, 保持原 IO) ================= */
/* 引脚有效性防护: 仅操作本模组(ESP32-S3-WROOM-1)引出的普通 IO.
 * 22-34 未引出 (含 26), 33-37 为 PSRAM/Flash 专用. */
static bool vib_pin_supported(int pin) {
    if (pin < 0 || pin > 48) return false;
    if (pin >= 22 && pin <= 34) return false;
    if (pin >= 33 && pin <= 37) return false;
    return true;
}
static bool s_en_cfg = false;   /* EN 引脚仅配置一次, 电平随用随切 */
static void drv_enable(bool on) {
    if (!vib_pin_supported(VIB_EN_PIN)) return;
    if (!s_en_cfg) {
        gpio_config_t io = {
            .pin_bit_mask = (1ULL << VIB_EN_PIN),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = false, .pull_down_en = false,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io);
        s_en_cfg = true;
    }
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
    drv_enable(duty > 0);   /* V1.5.x 省电: EN 随输出开关, 有输出才使能, 空闲拉低 */
}
static void out_off(void) {
    if (s_mode == VIB_NONE) return;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, VIB_CHANNEL, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, VIB_CHANNEL);
    drv_enable(false);      /* V1.5.x 省电: 停震立即拉低 TM6604 EN */
}

/* 定时到点关闭震动 (非阻塞 tap 用) */
static void tap_timer_cb(TimerHandle_t t) { (void)t; out_off(); }

/* ================= 对外接口 ================= */
void vibrator_init(void) {
    if (s_mode != VIB_NONE) return;
    drv_enable(false);     /* V1.5.x 省电: EN 默认拉低, 震动输出时才拉起 (GPIO48) */
    if (!ledc_init()) {
        ESP_LOGE(TAG, "LEDC PWM 配置失败 (GPIO%d), 震动不可用", (int)VIB_PWM_PIN);
        return;
    }
    s_mode = VIB_TM6604;
    s_tap_timer = xTimerCreate("vib_tap", pdMS_TO_TICKS(50), pdFALSE, NULL, tap_timer_cb);
    s_pat_timer = xTimerCreate("vib_pat", pdMS_TO_TICKS(50), pdFALSE, NULL, pat_timer_cb);
    ESP_LOGI(TAG, "震动驱动就绪: TM6604 PWM=GPIO%d(%dHz) EN=GPIO%d", (int)VIB_PWM_PIN,
             VIB_RES_FREQ, (int)VIB_EN_PIN);
}

bool vibrator_ready(void) { return s_mode != VIB_NONE; }

/* V1.1.0: 总开关 (由「震动反馈」设置调用) */
void vibrator_set_enabled(bool en) {
    s_enabled = en;
    if (!en) {
        s_music_on = false;            /* 音乐震动任务下个循环退出 */
        vibrator_stop_pattern();
        out_off();                     /* 停震: 内部已拉低 EN */
    } else {
        /* V1.5.x 省电: 开开关不立即拉高 EN, 等首次震动输出时再使能 */
    }
}

/* V1.2.x: 三个独立震动通道 (UI / 游戏 / 按键模拟) 的开关与触发.
 * 任一通道开 → 硬件总开关开; 全部关 → 关断 TM6604 EN 省电.
 * 上层 (设置-声音震动) 调用这三个 setter 而非 vibrator_set_enabled. */
void vibrator_set_ui_enabled(bool en) {
    s_vib_ui = en;
    vibrator_set_enabled(s_vib_ui || s_vib_game || s_key_sim);
}
void vibrator_set_game_enabled(bool en) {
    s_vib_game = en;
    vibrator_set_enabled(s_vib_ui || s_vib_game || s_key_sim);
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
    vibrator_stop_pattern();
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
    vibrator_stop_pattern();   /* 单次震动停掉在播花样 */
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

/* ================= V1.4.x: 预设花样 (段式序列) =================
 * 状态机: 播放时立即输出段[0], 软件定时器到时推进下一段; 播满 repeat 遍后停.
 * repeat=0 无限循环, 由调用方 vibrator_stop_pattern 显式停止 (闹铃用). */
static const vib_pattern_t *s_pat = NULL;
static uint8_t s_pat_idx = 0;
static uint8_t s_pat_rep = 0;

static void pat_apply_seg(void) {
    const vib_seg_t *seg = &s_pat->segs[s_pat_idx];
    if (seg->strength) out_set(seg->strength);
    else out_off();
}
static void pat_timer_cb(TimerHandle_t t) {
    (void)t;
    if (!s_pat) return;
    s_pat_idx++;                       /* 推进到下一段 */
    if (s_pat_idx >= s_pat->n) {
        s_pat_idx = 0;
        s_pat_rep++;
        if (s_pat->repeat != 0 && s_pat_rep >= s_pat->repeat) {
            s_pat = NULL;              /* 播放完成 */
            out_off();
            return;
        }
    }
    pat_apply_seg();
    xTimerChangePeriod(s_pat_timer, pdMS_TO_TICKS(s_pat->segs[s_pat_idx].ms), 0);
    xTimerStart(s_pat_timer, 0);
}

void vibrator_play_pattern(const vib_pattern_t *pat) {
    if (s_mode == VIB_NONE || !s_enabled) return;
    if (!pat || pat->n == 0) return;
    vibrator_music_mode(false);        /* 花样优先于音乐震动 */
    if (s_tap_timer) xTimerStop(s_tap_timer, 0);   /* 停掉在途 tap */
    s_pat = pat; s_pat_idx = 0; s_pat_rep = 0;
    pat_apply_seg();
    if (s_pat_timer) {
        xTimerChangePeriod(s_pat_timer, pdMS_TO_TICKS(s_pat->segs[0].ms), 0);
        xTimerStart(s_pat_timer, 0);
    }
}

void vibrator_stop_pattern(void) {
    if (!s_pat) return;
    s_pat = NULL;
    if (s_pat_timer) xTimerStop(s_pat_timer, 0);
    out_off();
}

bool vibrator_pattern_active(void) { return s_pat != NULL; }

/* ================= V1.4.x: 随音乐震动 =================
 * 内部任务每 20ms 采样 audio_player 振幅表 (0..1000), 超过死区后
 * 线性映射到 PWM 强度 1..100; 振幅回落即停. 开启时停掉在播花样. */
#define VIB_MUSIC_DEADZONE  60    /* 振幅千分比死区: 低于此不震 (静音/低噪) */
#define VIB_MUSIC_PERIOD_MS 20
static TaskHandle_t s_music_task = NULL;
static volatile bool s_music_on = false;

static void music_task(void *arg) {
    (void)arg;
    int32_t last = -1;
    for (;;) {
        if (!s_music_on) break;
        int32_t m = audio_player_get_meter();
        int32_t s = 0;
        if (m > VIB_MUSIC_DEADZONE) {
            s = (m - VIB_MUSIC_DEADZONE) * 100 / (1000 - VIB_MUSIC_DEADZONE);
            if (s > 100) s = 100;
            if (s < 1)   s = 1;
        }
        if (s != last) {
            last = s;
            if (s > 0) out_set((uint8_t)s);
            else       out_off();
        }
        vTaskDelay(pdMS_TO_TICKS(VIB_MUSIC_PERIOD_MS));
    }
    s_music_task = NULL;
    vTaskDelete(NULL);
}

void vibrator_music_mode(bool en) {
    if (s_mode == VIB_NONE) return;
    if (en == (s_music_on && s_music_task != NULL)) return;
    s_music_on = en;
    if (en) {
        vibrator_stop_pattern();
        if (s_music_task == NULL) {
            if (xTaskCreate(music_task, "vib_music", 2048, NULL, 3, &s_music_task) != pdPASS) {
                s_music_on = false;
                ESP_LOGE(TAG, "音乐震动任务创建失败");
            }
        }
    } else {
        out_off();   /* 任务在下个循环自检 s_music_on=false 退出 */
    }
}

bool vibrator_music_active(void) { return s_music_on && s_music_task != NULL; }

/* ================= V1.4.x: 内置花样库 ================= */
static const vib_seg_t s_seg_tick[]   = { { 60, 80 } };
static const vib_seg_t s_seg_double[] = { { 50, 75 }, { 60, 0 }, { 50, 75 } };
static const vib_seg_t s_seg_heart[]  = { { 90, 60 }, { 60, 0 }, { 140, 95 } };
static const vib_seg_t s_seg_succ[]   = { { 80, 70 }, { 70, 0 }, { 80, 70 }, { 70, 0 }, { 90, 90 } };
static const vib_seg_t s_seg_err[]    = { { 160, 45 }, { 130, 0 }, { 160, 45 }, { 130, 0 }, { 300, 75 } };
static const vib_seg_t s_seg_sos[]    = {
    /* 3 短 */ { 120, 80 }, { 120, 0 }, { 120, 80 }, { 120, 0 }, { 120, 80 }, { 240, 0 },
    /* 3 长 */ { 420, 80 }, { 240, 0 }, { 420, 80 }, { 240, 0 }, { 420, 80 }, { 240, 0 },
    /* 3 短 */ { 120, 80 }, { 120, 0 }, { 120, 80 }, { 120, 0 }, { 120, 80 },
};
static const vib_seg_t s_seg_up[]     = { { 60, 15 }, { 60, 35 }, { 60, 55 }, { 60, 75 }, { 60, 95 } };
static const vib_seg_t s_seg_down[]   = { { 60, 95 }, { 60, 75 }, { 60, 55 }, { 60, 35 }, { 60, 15 } };
static const vib_seg_t s_seg_alarm[]  = { { 300, 100 }, { 180, 0 } };
static const vib_seg_t s_seg_click[]  = { { 18, 100 } };  /* V1.5.x: 单段短脉冲 (修复"震好多下") */

const vib_pattern_t VIB_PAT_TICK      = { s_seg_tick,   1, 1 };
const vib_pattern_t VIB_PAT_DOUBLE    = { s_seg_double, 3, 1 };
const vib_pattern_t VIB_PAT_HEARTBEAT = { s_seg_heart,  3, 2 };
const vib_pattern_t VIB_PAT_SUCCESS   = { s_seg_succ,   5, 1 };
const vib_pattern_t VIB_PAT_ERROR     = { s_seg_err,    5, 1 };
const vib_pattern_t VIB_PAT_SOS       = { s_seg_sos,   17, 1 };
const vib_pattern_t VIB_PAT_RAMP_UP   = { s_seg_up,     5, 1 };
const vib_pattern_t VIB_PAT_RAMP_DOWN = { s_seg_down,   5, 1 };
const vib_pattern_t VIB_PAT_ALARM     = { s_seg_alarm,  2, 0 };   /* 无限循环 */
const vib_pattern_t VIB_PAT_CLICK     = { s_seg_click,  1, 1 };   /* 按键模拟: 一次=一下短震 */

/* ================= V1.4.x: 按键模拟 (触摸点击反馈) =================
 * 苹果触控板式: 无实体按键, 用单段 18ms 短脉冲模拟机械按键"哒"一下.
 * V1.5.x: 去掉"按下+释放"双脉冲 (一次手势 4 个脉冲是"震好多下"根因).
 * 长按(>0.5s)松手补震由 input.c 释放分支再调一次 vibrator_click (按下1次+弹起1次).
 * V1.5.x: 归"按键模拟"开关独立控制 (s_key_sim, NVS vib_key, 内存默认开) —
 * 与 UI震动/游戏震动三开关互不重叠: 按键模拟关 → 触摸点击完全不震 (不降级);
 * UI震动/游戏震动不影响本通道. 硬件总开关 s_enabled 含本通道. */

void vibrator_set_key_sim(bool en) { s_key_sim = en; vibrator_set_enabled(s_vib_ui || s_vib_game || s_key_sim); }
bool vibrator_key_sim_enabled(void) { return s_key_sim; }

void vibrator_click(void) {
    if (s_mode == VIB_NONE || !s_enabled || !s_key_sim) return;
    vibrator_play_pattern(&VIB_PAT_CLICK);
}
