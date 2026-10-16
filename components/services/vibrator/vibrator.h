/**
 * @file vibrator.h
 * @brief 震动马达驱动 (V1.4.x: TM6604 线性驱动, PWM 直驱 LRA)
 *
 * 硬件 (TM6604 SOT23-6):
 *  - Pin5 PWM <- GPIO42 (LEDC 190Hz 方波, 频率= LRA 谐振点)
 *  - Pin6 EN  <- GPIO48 (原 EN IO, 保持不变; 高使能)
 *  - Pin1/3 OUT+/OUT- -> LRA 线性马达; Pin2 GND; Pin4 VDD=3.3V
 *  - 原 SCL=GPIO47 空置; 原 GPIO6/LEDC 兜底路径已并入 PWM 主路径.
 *
 * V1.4.x 新增两套高级触发:
 *  - 预设花样 vibrator_play_pattern: 段式序列 (时长+强度), 非阻塞定时器逐段
 *    推进, 内置 TICK/DOUBLE/HEARTBEAT/SUCCESS/ERROR/SOS/RAMP/ALARM 花样库.
 *  - 随音乐震动 vibrator_music_mode: 内部任务每 20ms 采样 audio_player 振幅表,
 *    按响度实时映射 PWM 占空比, 听歌/游戏时马达随节奏震动.
 *
 * 冲突仲裁: 花样/音乐震动/单次 tap 三者互斥, 后发起者停掉前者; 闹钟无限花样
 * 由调用方在关铃时 vibrator_stop_pattern 停止.
 */
#ifndef VIBRATOR_H
#define VIBRATOR_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化: 配置 TM6604 (EN 使能 + LEDC PWM). 幂等. */
void vibrator_init(void);

/* 总开关 (由「震动反馈」设置调用; false 立即停止震动).
 * 注意: 设置页请改用 vibrator_set_ui_enabled / vibrator_set_game_enabled. */
void vibrator_set_enabled(bool en);

/* UI 震动开关 (拖拉/菜单导航/设置调整). 任一通道开 → 硬件总开关开. */
void vibrator_set_ui_enabled(bool en);
/* 游戏震动开关 (游戏内按键). 任一通道开 → 硬件总开关开. */
void vibrator_set_game_enabled(bool en);
/* UI 震动: 仅当 UI 震动开启时触发一次 (ms 时长, strength 0~100). */
void vibrator_tap_ui(uint16_t ms, uint8_t strength);
/* 游戏震动: 仅当游戏震动开启时触发一次 (ms 时长, strength 0~100). */
void vibrator_tap_game(uint16_t ms, uint8_t strength);

/* 设置输出强度 strength=0~100 (实时). 0 静止. */
void vibrator_set(uint8_t strength);

/* 彻底停止输出. */
void vibrator_stop(void);

/* 单次震动指定时长 (阻塞, 期间最强输出后停止). ms<=0 仅停止. */
void vibrator_buzz(uint16_t ms);

/* V1.1.0: 非阻塞单次震动: 震动 ms 毫秒、strength 0~100 强度后自动停止.
 * 供按键/触摸反馈、闹钟、番茄钟/计时器调用. */
void vibrator_tap(uint16_t ms, uint8_t strength);

/* ============ V1.4.x: 预设震动花样 ============ */
/* 花样段: 持续 ms 毫秒, strength 强度 0~100 (0=停). */
typedef struct {
    uint16_t ms;        /* 段时长 (ms) */
    uint8_t  strength;  /* 强度 0~100, 0=停 */
} vib_seg_t;

/* 花样: 段序列 + 循环次数. repeat=0 表示无限循环 (闹铃用, 由调用方停止). */
typedef struct {
    const vib_seg_t *segs;
    uint8_t  n;         /* 段数 */
    uint8_t  repeat;    /* 播放遍数: 1=一遍, 0=无限循环 */
} vib_pattern_t;

/* 播放预设花样 (非阻塞, 内部定时器逐段推进; 与在途 tap/音乐震动互斥). */
void vibrator_play_pattern(const vib_pattern_t *pat);
/* 停止在播花样 (无限循环花样必须显式停止). */
void vibrator_stop_pattern(void);
/* 花样是否在播放 (供页面状态显示). */
bool vibrator_pattern_active(void);

/* V1.4.x: 随音乐震动模式 — 内部任务每 20ms 采样 audio_player 振幅表,
 * 按响度实时映射 PWM 强度 (死区 6%). 开启会停掉在播花样. */
void vibrator_music_mode(bool en);
/* 音乐震动是否开启 (供页面状态显示). */
bool vibrator_music_active(void);

/* ============ V1.4.x: 内置花样库 (顶层直接引用) ============ */
extern const vib_pattern_t VIB_PAT_TICK;       /* 短促 1 下 */
extern const vib_pattern_t VIB_PAT_DOUBLE;     /* 双敲 */
extern const vib_pattern_t VIB_PAT_HEARTBEAT;  /* 心跳 (lub-dub 强弱) */
extern const vib_pattern_t VIB_PAT_SUCCESS;    /* 成功三连 */
extern const vib_pattern_t VIB_PAT_ERROR;      /* 错误低鸣 */
extern const vib_pattern_t VIB_PAT_SOS;        /* 摩尔斯求救 */
extern const vib_pattern_t VIB_PAT_RAMP_UP;    /* 渐强 */
extern const vib_pattern_t VIB_PAT_RAMP_DOWN;  /* 渐弱 */
extern const vib_pattern_t VIB_PAT_ALARM;      /* 闹铃: 无限循环 */
extern const vib_pattern_t VIB_PAT_CLICK;       /* 按键模拟: 按下+释放双脉冲 (苹果触控板式) */

/* V1.4.x: 按键模拟 — 触摸点击时用 CLICK 双脉冲模拟机械按键"按下/释放"手感.
 * 由设置页"声音震动 → 按键模拟"开关控制 (NVS 由设置页持久化). */
void vibrator_set_key_sim(bool en);
bool vibrator_key_sim_enabled(void);
/* 点击反馈统一入口 (触摸点击判定处调用): 开启按键模拟 → CLICK 双脉冲,
 * 关闭 → 保持普通 UI tap. 遵循 UI 震动开关. */
void vibrator_click(void);

/* 是否已初始化 (任一种驱动就绪). */
bool vibrator_ready(void);

#ifdef __cplusplus
}
#endif

#endif /* VIBRATOR_H */