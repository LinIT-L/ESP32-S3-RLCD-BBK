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

/* 点击反馈 (触摸点击判定处调用): 归 **UI 震动** 通道 (2026-10-24: 原独立的
 * "触摸反馈"开关已合并进来) —— 关掉 UI 震动, 点击就确实不震. */
void vibrator_click(void);

/* ★ 2026-11-22: UI 点击静音闸门 —— 「书库 → 阅读优化 → 震动」关掉、以及
 *   关机确认框弹出期间置位. 挡 vibrator_click() 与 vibrator_tap_ui() 两种
 *   UI 反馈(阅读器点按/翻页、关机确认的最后一震), 其它页面的点击反馈不受影响
 *   (设置页的「UI 震动」开关状态不会被它带偏). 由调用方成对下发/复位.
 *   ⚠ 只挡 UI 反馈, 不影响游戏震动与长按花样. */
void vibrator_set_reader_mute(bool mute);   /* 旧名保留(阅读页调用点), 语义即 click_mute */
bool vibrator_reader_muted(void);

/* 是否已初始化 (任一种驱动就绪). */
bool vibrator_ready(void);

/* ★ 2026-11-24: 语义钩子 —— 让触觉效果库在不改任何调用方的前提下接管
 *   UI/游戏的基础反馈 ("丰富震动"开关打开时)。
 *   sem 说明这次反馈的**语义**, 由效果库映射成具体效果 (见 haptic.c:rich_hook);
 *   返回 true = 已接管(不要再走基础 tap); false = 走原来的基础震动。
 *   ⚠ 钩子只在效果库真正**起播**时返回 true; 因冷却/忙被拒时返回 false,
 *     这样"连点"仍会有基础反馈, 不会出现点了没反应。
 *
 * ⚠ sem 只决定"用哪个效果"; ms/strength 仍是**基础震动**的参数(丰富震动关闭时用它,
 *   与旧固件手感一致)。要调整手感优先改 haptic.c 的效果表, 不要改这里。 */
typedef enum {
    VIB_SEM_TICK = 0,   /* 移动一格: 列表上下 / 桌面轮播跨图标 */
    VIB_SEM_STEP,       /* 调值一格: 音量/音调/WPM 等档位滚轮 (比 tick 更实, 要能明显听到) */
    VIB_SEM_CONFIRM,    /* 确认 / 进入 / 触摸点中 */
    VIB_SEM_BACK,       /* 返回 / 取消 */
    VIB_SEM_DELETE,     /* 删除 / 清空 / 卸载 / 取消收藏 */
    VIB_SEM_FAV,        /* 收藏 / 加入 */
    VIB_SEM_BOUNDARY,   /* 到头 / 越界 (音量 0 或满、列表首尾) */
    VIB_SEM_ALERT,      /* 重提示: 关机确认框这类"要不要真做"的弹窗 */
    VIB_SEM_GAME,       /* 游戏内按键 */
} vib_sem_t;
typedef bool (*vib_rich_hook_t)(vib_sem_t sem, uint16_t ms, uint8_t strength);
void vibrator_set_rich_hook(vib_rich_hook_t fn);

/* 按语义触发一次反馈 (带每类的默认基础参数). 调用点优先用它:
 *   vibrator_sem(VIB_SEM_BACK) / (VIB_SEM_DELETE) / (VIB_SEM_FAV) / (VIB_SEM_TICK) ...
 * 内部顺序: 开关判定 → 独占闸门 → 效果库钩子 → 基础 tap 兜底。 */
void vibrator_sem(vib_sem_t sem);

/* ★ 2026-11-24: 马达独占闸门 (haptic 效果库用, 最小侵入).
 *   on=true : 期间 vibrator_tap / tap_ui / tap_game / click / buzz / play_pattern **全部直接返回**,
 *             不会再起 tap 定时器, 也就不会在效果播到一半时把占空比清零。
 *   on=false: 恢复原行为 (必须与 true 成对, haptic 播完/停止/退出时都会复位)。
 *   ⚠ vibrator_set()/vibrator_stop() 不受闸门影响 —— 它们正是 haptic 的输出通道。
 *   ⚠ 音乐随动 (vibrator_music_mode) 也不受闸门影响: 那条路径自己写 set(), 本轮不处理。 */
void vibrator_set_owner_haptic(bool on);

#ifdef __cplusplus
}
#endif

#endif /* VIBRATOR_H */