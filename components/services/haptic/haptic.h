/**
 * @file haptic.h
 * @brief 触觉效果库 (驱动无关) —— 效果表 + 播放引擎 + 后端抽象.
 *
 * 分层:
 *   effects (本文件/haptic.c)  ──只认"强度 0..100 + 时长"两个量, 不认芯片
 *   backend (haptic_backend_pwm.c) ──把"强度"翻译成具体驱动 (当前 TM6604 PWM)
 *   bridge  (luat_lib_haptic.c)    ──Lua `app.haptic.*`
 *
 * 为什么分层: 当前硬件只有"一路 PWM 占空比 + 开/关", 将来换 DRV2605L 只需再写一个
 * backend 并调 haptic_set_backend(); 22 个效果名与调用方一行都不用改。
 *
 * 结构参考 (只借鉴组织方式, 未复制任何代码):
 *   · Adafruit_DRV2605_Library (MIT): "按编号/名字播一段" 的极简调用面 (play/stop)
 *   · SparkFun Haptic Motor Driver 例程: 效果名按用途命名的习惯
 *   · DRV2605 数据手册的 sequence slots + pause → 本库的"段", 静音段用 strength=0 表示
 *   ⛔ 未复制任何厂商波形数据; 本机也没有 DRV2605, 不引入其 ROM 效果表。
 *
 * ⛔ 本头文件不引入任何 vibrator/驱动头 —— 效果库的**接口**不暴露引脚与 LEDC。
 *   (唯一例外在 .c: 注册"基础反馈→效果"的语义钩子需要 vibrator.h, 见 haptic_set_rich。)
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 一段: 持续 ms, 强度 0..100 (0 = 静默段, 用来隔开两下) */
typedef struct {
    uint16_t ms;
    uint8_t  strength;
} haptic_seg_t;

/* 绑定到具体驱动的后端. strength=0 必须等价于"停". */
typedef struct {
    const char *name;              /* "tm6604-pwm" / "drv2605l" ... */
    bool (*init)(void);            /* 幂等; 返回是否可用 */
    bool (*ready)(void);           /* 只查状态, 无副作用 */
    void (*set)(uint8_t strength); /* 0..100, 立即生效 */
    void (*stop)(void);            /* 立即停, 幂等 */
    /* 可选: 播放期间**独占**马达, 让旧的 tap/花样入口让路 (避免播放中途被清零).
     * 可为 NULL (旧后端/无此概念时不调用)。必须成对: true 起播, false 收尾。 */
    void (*claim)(bool on);
} haptic_backend_t;

/* 一个具名效果 */
typedef struct {
    const char *name;
    const haptic_seg_t *segs;
    uint8_t  n;           /* 段数 */
    uint8_t  priority;    /* 0..255, 大者优先; 同级不互相打断 */
    uint16_t cooldown_ms; /* 同名效果两次触发的最小间隔 */
} haptic_fx_t;

/* 拒绝原因 (给 Lua 返回第二个值, 便于演示页显示"为什么没震") */
typedef enum {
    HAPTIC_OK = 0,
    HAPTIC_REJECT_UNKNOWN,    /* 没有这个效果名 */
    HAPTIC_REJECT_NO_BACKEND, /* 硬件不可用 / 未初始化 */
    HAPTIC_REJECT_BUSY,       /* 正在播更高优先级的效果 */
    HAPTIC_REJECT_COOLDOWN,   /* 同名效果冷却中 */
    HAPTIC_REJECT_RATE,       /* 全局最小间隔未到 (防连点刷马达) */
    HAPTIC_REJECT_ARG,        /* 参数非法 (段数/时长越界) */
    HAPTIC_REJECT_DISABLED,   /* 「丰富震动」开关关闭 (回退基础震动) */
} haptic_reject_t;

#define HAPTIC_MAX_SEGS 32   /* 自定义序列上限 (静态数组, 不 malloc) */

/* ---- 生命周期 ---- */
void haptic_init(void);                                  /* 幂等; 注册默认后端 */
bool haptic_set_backend(const haptic_backend_t *b);      /* 换后端 (DRV2605L 预留) */
const haptic_backend_t *haptic_backend(void);
bool haptic_ready(void);

/* ---- 播放 ---- */
bool haptic_play(const char *name, haptic_reject_t *why);
bool haptic_sequence(const haptic_seg_t *segs, int n, haptic_reject_t *why);
void haptic_stop(void);
bool haptic_active(void);
uint8_t haptic_active_priority(void);

/* ---- 「丰富震动」开关 (设置 → 触摸与反馈) ----
 * 持久化: NVS `os_settings` / `haptic_rich`, 缺省 **1 = 开**; 本库单一归属读写
 * (设置页只调 set/查询, 不自己写 NVS)。
 *
 * 开: 系统反馈按语义映射成效果(**调用点不用改**):
 *       TICK(滚动/轮播/音量) → slider_tick   CONFIRM(确认/点中) → confirm
 *       BACK(返回)            → back           DELETE(删除/卸载/取消收藏) → delete
 *       FAV(收藏)             → fav            BOUNDARY(到头) → boundary
 *       GAME(游戏)            → game_hit
 *     Lua 仍可 app.haptic.play(<任意效果>) 显式指定。
 * 关: ① 立刻停掉正在播的序列并交还马达(不留残余), ② play/sequence 一律返回
 *     HAPTIC_REJECT_DISABLED, ③ 三类基础反馈落回 vibrator 的原 tap 路径
 *     —— 即"基础震动行为", 触摸/游戏/系统功能一概不受影响。
 * ⚠ 与「菜单震动 / 游戏震动」是两个独立维度: 那两个是**要不要震**, 这个是**震得简单还是丰富**。 */
void haptic_set_rich(bool on);
bool haptic_rich_enabled(void);

/* ---- 查询 (给演示页/编辑器列参数) ---- */
int  haptic_effect_count(void);
const haptic_fx_t *haptic_effect_at(int i);
const haptic_fx_t *haptic_effect_find(const char *name);
const char *haptic_reject_name(haptic_reject_t why);

/* 内置后端: TM6604 + 1508 LRA (PWM 占空比) */
const haptic_backend_t *haptic_backend_pwm(void);

/* 应用退出兜底: 停播 + 清冷却/限频表 (page_lua 退出时调) */
void haptic_reset_app(void);

#ifdef __cplusplus
}
#endif
