/**
 * @file vibrator.c
 * @brief 震动马达驱动 (V1.4.x: TM6604 线性驱动, PWM 直驱 LRA)
 *
 * TM6604 为国产线性马达驱动 IC (SOT23-6, PWM 输入, 差动桥式输出).
 * 输入 PWM 频率应等于 LRA 谐振频率 (常见 150~250Hz), 占空比控制输出强度;
 * EN 高有效, 拉低即关断输出省电. 芯片无寄存器/无探测手段, 上电即用.
 *
 * 接线 (2026-10-04 分支 board/xiaozhi-s3-core, 小智 ESP32-S3-AI-2E 核心模块板):
 *   Pin1 OUT+ ──┐        ┌── Pin3 OUT-
 *   Pin2 GND  ── GND
 *   Pin4 VDD  ── 3.3V
 *   Pin5 PWM  ── **GPIO6**  (旧固件里彻底闲置 —— tone_player 改走 I2S 后释放; 原 42→48 两次都作废)
 *   Pin6 EN   ── (新板震动电源默认常开, 不再由 MCU 控制)
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
/* ★★ 2026-10-04 (分支 board/xiaozhi-s3-core) —— 引脚来源已核准: **厂商规格书 v1.0**
 *
 * 模块 `ESP32-S3-AI-2E` 只有 **24 个可用 GPIO**:
 *   0,1,2,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,41,42,43,44
 * 另有 **12 个被模块内部占用, 一律不可用**:
 *   IO3=VOP(功放+)      IO21=VBAT            IO35=LGS4056_DONE   IO36=LGS4056_CHRG
 *   IO37=MIC1P          IO38=MIC1N+codec I2C_SDA                IO39=MIC2P
 *   IO40=MIC2N          IO45=VBUS_5V         IO46=VON(功放−)
 *   IO47=VSYS_IN+codec I2C_SCL                                  IO48=VSYS_OUT
 *
 * ⛔⛔ **上一版曾把 PWM 挪到 GPIO48 —— 那是错的**: 48 = `VSYS_OUT`, 是模块"电池→系统"
 *   的**电源通路输出脚**(不接开关时它和 VSYS_IN 要短接)。把它接到 LEDC 输出等于
 *   拿 PWM 去斩电源输入, 会直接把板子带进不稳定态。
 * ✅ 现用 **GPIO6** —— 旧固件里**彻底闲置**(`user_config.h`: tone_player 改走 I2S 后 GPIO6 释放;
 *   全仓无任何引用), 且在模块 24 个可用脚内。
 *   ⛔ 不要用 GPIO4: 它在旧固件里是**电池 ADC (ADC1_CH3)**(`board_battery.c`),
 *      拿 LEDC 去驱动会把电量采样搅成垃圾。
 * ⚠ 原 GPIO42 让给了 LCD 片选(42 同样在 24 个可用脚里 ✓)。 */
#define VIB_PWM_PIN   GPIO_NUM_6
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

/* ================= PWM 输出 (LEDC) =================
 * ★ 2026-10-04: 原来这里还有一条 "EN 引脚(GPIO48) 随输出开关" 的省电路径。
 *   新板震动电源默认常开 + GPIO42 归了屏 CS ⇒ EN 与 PWM 合并到 GPIO48 一根线,
 *   drv_enable()/VIB_EN_PIN 整条删掉 (停止震动 = duty 归 0)。 */
static bool ledc_init(void) {
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
    if (!ledc_init()) {
        ESP_LOGE(TAG, "LEDC PWM 配置失败 (GPIO%d), 震动不可用", (int)VIB_PWM_PIN);
        return;
    }
    s_mode = VIB_TM6604;
    s_tap_timer = xTimerCreate("vib_tap", pdMS_TO_TICKS(50), pdFALSE, NULL, tap_timer_cb);
    s_pat_timer = xTimerCreate("vib_pat", pdMS_TO_TICKS(50), pdFALSE, NULL, pat_timer_cb);
    ESP_LOGI(TAG, "震动驱动就绪: PWM=GPIO%d (%dHz), 电源默认常开 (无 EN 脚)",
             (int)VIB_PWM_PIN, VIB_RES_FREQ);
}

bool vibrator_ready(void) { return s_mode != VIB_NONE; }

/* V1.1.0: 总开关 (由「震动反馈」设置调用) */
void vibrator_set_enabled(bool en) {
    s_enabled = en;
    if (!en) {
        s_music_on = false;            /* 音乐震动任务下个循环退出 */
        vibrator_stop_pattern();
        out_off();                     /* 停震: duty 归 0 */
    } else {
        /* 开开关不立刻输出, 等首次震动时再设 duty (省电, 且避免上电就抖一下) */
    }
}

/* V1.2.x: 三个独立震动通道 (UI / 游戏 / 按键模拟) 的开关与触发.
 * 任一通道开 → 硬件总开关开; 全部关 → duty 归 0 (新板电源常开, 无法靠 EN 断电). */
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
    /* 2026-09-29 用户规范: UI 能震、游戏不震的割裂体验不可接受.
     * 原来游戏震动单独由"设置→游戏震动"门控, 一旦被关 (或在别处被误关) 游戏里就永远不震,
     * 而 UI 照常震 —— 用户实测正是这个现象. 现改为: **UI/游戏任一通道开启即生效**,
     * 要单独关掉"游戏内的按键震动"请用各引擎的「引擎优化→触摸震动」(per-engine 开关). */
    if (s_vib_game || s_vib_ui) vibrator_tap(ms, strength);
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
