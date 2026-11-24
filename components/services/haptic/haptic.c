/**
 * @file haptic.c
 * @brief 触觉效果库核心: 效果表 + 段式播放引擎 + 优先级/冷却/限频.
 *
 * 引擎只做三件事:
 *   ① 把"段序列"按时间推进 (esp_timer 单次定时器, 每段各起一次);
 *   ② 到一段就把 strength 交给后端 (0 = 停);
 *   ③ 仲裁: 优先级 / 同名冷却 / 全局最小间隔.
 *
 * 为什么不用 FreeRTOS 软件定时器: vibrator.c 那边已经踩过 "命令队列满 → 改周期失败 →
 * 马达常振" 的坑。esp_timer 是独立任务 + 直接 ARM, 没有命令队列, 且这里只用 one-shot。
 *
 * ⛔ 本文件不 include 任何驱动头。
 */
#include "haptic.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "svc_cfg.h"      /* 「丰富震动」开关持久化 (os_settings / haptic_rich) */
#include "vibrator.h"     /* 仅为注册"基础反馈→效果"语义钩子, 不直接写 LEDC */
#include <string.h>

#define TAG "HAPTIC"

/* 全局最小间隔: 连点/连滑时保护马达, 也避免"每帧一次"的无意义刷新 (单位 us) */
#define HAPTIC_MIN_GAP_US   25000      /* 25 ms */

/* 自定义序列的优先级: 用户显式组合, 视为最高 (但仍要让 error 之类抢不过 ? 见下) */
#define HAPTIC_SEQ_PRIORITY 100

/* ==================== 「丰富震动」开关 ====================
 * 与「菜单震动 / 游戏震动」正交:
 *   那两个回答"要不要震"(vibrator 侧), 这一个回答"震得简单还是丰富"(效果库侧)。
 * 键放独立的 `haptic` 命名空间 (不与设置页共用 os_settings), **缺省开**。
 * 单一归属: 只有本库读写它 —— 设置页只调 haptic_set_rich()/haptic_rich_enabled()。
 * 懒加载: 首次用到时读一次 NVS; main 里的 haptic_init() 会在开机时提前读,
 *   所以"重启后保持"不依赖用户先进一次设置页。
 */
#define HAPTIC_CFG_NS  "haptic"    /* 独立命名空间: 一个 NS 一个主人, 不蹭 os_settings */
#define HAPTIC_CFG_KEY "rich"      /* u8: 1=开(缺省) 0=关 */
static bool s_rich        = true;
static bool s_rich_loaded = false;

static void rich_load(void)
{
    if (s_rich_loaded) return;
    s_rich_loaded = true;
    s_rich = svc_cfg_get_u8(HAPTIC_CFG_NS, HAPTIC_CFG_KEY, 1) ? true : false;
}

bool haptic_rich_enabled(void) { rich_load(); return s_rich; }

void haptic_set_rich(bool on)
{
    rich_load();
    const bool changed = (s_rich != on);
    s_rich = on;
    if (changed) svc_cfg_set_u8(HAPTIC_CFG_NS, HAPTIC_CFG_KEY, on ? 1 : 0);
    if (!on) {
        /* 关: 立刻掐掉在播序列 —— play_finish() 会先停输出再放闸门,
         * 所以不会留下半截节奏, 也不会把马达锁在某个占空比上。 */
        haptic_stop();
        ESP_LOGI(TAG, "丰富震动: 关 (回退基础震动)");
    } else if (changed) {
        ESP_LOGI(TAG, "丰富震动: 开");
    }
}

/* ==================== 效果表 ====================
 * 只用两个量描述: ms + strength(0..100)。
 * 静默段用 strength=0 表示 —— 这一段就是"间隔", 不需要额外参数。
 * priority / cooldown 是"策略参数", 底层不支持调速/波形, 所以这里也不开放那些参数。
 */
static const haptic_seg_t S_LIGHT[]   = { { 8, 45 } };
static const haptic_seg_t S_SOFT[]    = { { 16, 85 } };
static const haptic_seg_t S_SHARP[]   = { { 22, 100 } };
/* 确认: 22ms 实心一下 —— 比旧的 18ms 更"按实", 与 tick(6ms) 拉开距离 */
static const haptic_seg_t S_CONFIRM[] = { { 26, 100 } };
/* 返回: 强→弱的双击 ("退一寸"): 12ms/75 + 45ms 间隔 + 8ms/45 */
static const haptic_seg_t S_BACK[]    = { { 16, 85 }, { 45, 0 }, { 10, 55 } };
/* 删除: 两记等强重击 (否定感, 比 warning 更实) */
static const haptic_seg_t S_DEL[]     = { { 60, 100 }, { 55, 0 }, { 60, 100 } };
/* 收藏: 由弱到强的双击 (肯定感, "收进去") */
static const haptic_seg_t S_FAV[]     = { { 18, 85 }, { 30, 0 }, { 32, 100 } };
static const haptic_seg_t S_CANCEL[]  = { { 30, 55 }, { 70, 0 }, { 30, 55 } };
static const haptic_seg_t S_DOUBLE[]  = { { 30, 100 }, { 60, 0 }, { 30, 100 } };
static const haptic_seg_t S_TRIPLE[]  = { { 25, 100 }, { 55, 0 }, { 25, 100 }, { 55, 0 }, { 25, 100 } };
static const haptic_seg_t S_SSL[]     = { { 25, 80 }, { 60, 0 }, { 25, 80 }, { 60, 0 }, { 90, 100 } };
static const haptic_seg_t S_RAMP_UP[] = { { 55, 20 }, { 55, 40 }, { 55, 60 }, { 55, 80 }, { 55, 100 } };
static const haptic_seg_t S_RAMP_DN[] = { { 55, 100 }, { 55, 80 }, { 55, 60 }, { 55, 40 }, { 55, 20 } };
static const haptic_seg_t S_PULSE[]   = { { 250, 100 } };
static const haptic_seg_t S_SUCCESS[] = { { 55, 70 }, { 55, 0 }, { 55, 70 }, { 55, 0 }, { 80, 100 } };
static const haptic_seg_t S_ERROR[]   = { { 120, 45 }, { 100, 0 }, { 120, 45 }, { 100, 0 }, { 160, 60 } };
static const haptic_seg_t S_WARNING[] = { { 90, 55 }, { 70, 0 }, { 90, 55 } };
static const haptic_seg_t S_LONG[]    = { { 60, 60 }, { 60, 80 }, { 80, 100 } };
static const haptic_seg_t S_SLIDER[]  = { { 10, 70 } };  /* 列表/轮播移动一格: 短促但听得见 */
static const haptic_seg_t S_STEP[]    = { { 14, 90 } };  /* 调值一格(音量/音调): 比滚动更实 */
static const haptic_seg_t S_BOUND[]   = { { 22, 85 }, { 45, 0 }, { 22, 85 } };
/* 重提示(关机确认): 两下 90ms 满强度, 中间 90ms 空 —— 像"喂、确认下",
 * 比 delete(60ms×2, 间隔 55ms) 更长更沉稳, 不会听成"删除那种碎击"。 */
static const haptic_seg_t S_ALERT[]   = { { 90, 100 }, { 90, 0 }, { 90, 100 } };
static const haptic_seg_t S_HIT[]     = { { 35, 100 } };
static const haptic_seg_t S_PICKUP[]  = { { 18, 70 }, { 40, 0 }, { 30, 90 } };

static const haptic_fx_t s_fx[] = {
    /* name              segs        n   prio  cooldown */
    { "light_tap",       S_LIGHT,    1,  20,   40  },
    { "soft_click",      S_SOFT,     1,  25,   40  },
    { "sharp_click",     S_SHARP,    1,  30,   50  },
    { "confirm",         S_CONFIRM,  1,  45,   60  },
    { "cancel",          S_CANCEL,   3,  40,   220 },
    { "back",            S_BACK,     1,  40,   60  },
    { "double_click",    S_DOUBLE,   3,  35,   180 },
    { "triple_click",    S_TRIPLE,   5,  35,   220 },
    { "short_short_long",S_SSL,      5,  45,   260 },
    { "ramp_up",         S_RAMP_UP,  5,  40,   300 },
    { "ramp_down",       S_RAMP_DN,  5,  40,   300 },
    { "pulse",           S_PULSE,    1,  50,   400 },
    { "success",         S_SUCCESS,  5,  60,   400 },
    { "error",           S_ERROR,    5,  75,   600 },
    { "warning",         S_WARNING,  3,  65,   500 },
    { "long_press",      S_LONG,     3,  55,   400 },
    { "slider_tick",     S_SLIDER,   1,  10,   25  },
    { "boundary",        S_BOUND,    3,  50,   200 },
    { "game_hit",        S_HIT,      1,  45,   40  },
    { "game_pickup",     S_PICKUP,   3,  40,   80  },
    { "delete",          S_DEL,      3,  70,   400 },   /* 删除/清空/卸载/取消收藏 */
    { "fav",             S_FAV,      3,  50,   300 },   /* 收藏/加入 */
    { "step",            S_STEP,     1,  20,   30  },   /* 调值一格: 音量/音调/WPM */
    { "alert",           S_ALERT,    3,  80,   900 },   /* 重提示: 关机确认框 (优先级最高) */
};

#define FX_N ((int)(sizeof(s_fx) / sizeof(s_fx[0])))

/* ==================== 播放状态 ==================== */
static const haptic_backend_t *s_be;
static esp_timer_handle_t s_timer;
static bool s_inited;

static bool s_active;
static uint8_t s_prio;
static const haptic_seg_t *s_segs;
static uint8_t s_n;
static uint8_t s_idx;
static haptic_seg_t s_custom[HAPTIC_MAX_SEGS];   /* 自定义序列副本 (静态, 不 malloc) */

static int64_t s_last_start_us;
static int64_t s_fx_last_us[FX_N];

/* 起播代号: 每次 play_start +1。esp_timer 回调跑在独立任务上, 与 UI 线程是两个任务;
 * 抢占瞬间"旧序列的回调"可能已经在跑。回调开头比对 `s_armed_epoch`(本段定时器属于
 * 哪次起播) 与 `s_epoch`(当前起播), 不一致就**碰都不碰状态**直接返回 —— 否则新效果的
 * 段号会被旧回调推进/提前收尾(表现为"被截断")。
 * 代价: 1 次自增 + 2 次比较, 无锁、无任务、无动态内存。 */
static volatile uint32_t s_epoch;         /* 当前起播代号 */
static volatile uint32_t s_armed_epoch;   /* 已武装的定时器属于哪次起播 */

static void play_apply_seg(void)
{
    if (!s_be || !s_be->set || !s_segs) return;
    s_be->set(s_segs[s_idx].strength);
}

static void play_finish(void)
{
    s_active = false;
    s_segs   = NULL;
    s_n      = 0;
    s_idx    = 0;
    s_prio   = 0;
    /* 先停输出再放闸门: 反过来的话, 刚放行的一次点击会被这行 stop 顺手掐掉 */
    if (s_be && s_be->stop)  s_be->stop();
    if (s_be && s_be->claim) s_be->claim(false);
}

/* 起下一段的定时器。⚠ 失败必须就地收尾: 否则马达会停在当前占空比一直震 */
static bool arm_ms(uint32_t ms)
{
    if (!s_timer) return false;
    s_armed_epoch = s_epoch;          /* 这段定时器归当前这次起播 */

    if (esp_timer_start_once(s_timer, (uint64_t)ms * 1000ULL) != ESP_OK) {
        ESP_LOGW(TAG, "段定时器启动失败 (%ums) ⇒ 立即停震", (unsigned)ms);
        play_finish();
        return false;
    }
    return true;
}

static void on_timer(void *arg)
{
    (void)arg;
    if (!s_active) return;
    /* 抢占检查必须在动 s_idx 之前: 旧回调若推进段号, 会把新序列跳过一段 */
    if (s_armed_epoch != s_epoch) return;
    s_idx++;
    if (s_idx >= s_n) { play_finish(); return; }
    play_apply_seg();
    arm_ms(s_segs[s_idx].ms);
}

/* 启动播放; 调用方已完成仲裁 */
static bool play_start(const haptic_seg_t *segs, int n, uint8_t prio)
{
    if (!segs || n <= 0 || n > HAPTIC_MAX_SEGS) return false;
    if (!s_timer) return false;

    s_epoch++;                          /* 先换代: 让"已在跑"的旧回调失效 (见 on_timer) */
    esp_timer_stop(s_timer);            /* 停掉上一条 (抢占时) */

    /* ★ 2026-11-24 抢马达: 先上闸门(挡新 tap/花样), 再清掉**在途** tap 定时器与花样。
     *   顺序不能反 —— 反了会有一段窗口让新点击挤进来。 */
    if (s_be && s_be->claim) s_be->claim(true);
    if (s_be && s_be->stop)  s_be->stop();

    s_segs  = segs;
    s_n     = (uint8_t)n;
    s_idx   = 0;
    s_prio  = prio;
    s_active = true;
    play_apply_seg();
    if (s_segs[0].ms == 0) {
        /* 0ms 首段: 直接推进, 不启 0 定时器。
         * 先把本条标记为"已武装", 否则 on_timer 的抢占检查(armed != epoch)会把它判成旧回调。 */
        s_armed_epoch = s_epoch;
        on_timer(NULL);
    } else if (!arm_ms(s_segs[0].ms)) {
        return false;                   /* arm_ms 内已 play_finish() 收尾 */
    }
    s_last_start_us = esp_timer_get_time();
    return s_active;
}

/* ==================== 生命周期 ==================== */
/* 语义钩子: 注册给 vibrator, 让系统 UI/点击/游戏在"丰富震动"开启时用效果库出效果
 * (定义在 haptic_play 之后, 这里先声明)。 */
static bool rich_hook(vib_sem_t sem, uint16_t ms, uint8_t strength);

void haptic_init(void)
{
    if (s_inited) return;
    s_inited = true;
    rich_load();

    if (!s_be) s_be = haptic_backend_pwm();
    if (s_be && s_be->init) {
        if (!s_be->init()) ESP_LOGW(TAG, "后端 %s 初始化失败 ⇒ 触觉效果不可用",
                                    s_be->name ? s_be->name : "?");
    }
    const esp_timer_create_args_t args = {
        .callback = on_timer,
        .arg      = NULL,
        .name     = "haptic",
    };
    if (esp_timer_create(&args, &s_timer) != ESP_OK) {
        s_timer = NULL;
        ESP_LOGW(TAG, "esp_timer 创建失败 ⇒ 触觉效果不可用");
    }
    /* 注册语义钩子: 调用点(菜单/点击/游戏)**一行不改**, 由本库决定用效果还是基础震动 */
    vibrator_set_rich_hook(rich_hook);

    ESP_LOGI(TAG, "触觉效果库就绪: %d 个效果, 后端=%s, 丰富震动=%s", FX_N,
             (s_be && s_be->name) ? s_be->name : "none", s_rich ? "开" : "关");
}

bool haptic_set_backend(const haptic_backend_t *b)
{
    if (!b) return false;
    haptic_stop();
    s_be = b;
    if (b->init && !b->init()) return false;
    return true;
}

const haptic_backend_t *haptic_backend(void) { return s_be; }

bool haptic_ready(void)
{
    if (!s_be) return false;
    if (s_be->ready) return s_be->ready();
    return true;
}

/* ==================== 播放 ==================== */
static bool prio_allows(uint8_t prio)
{
    if (!s_active) return true;
    return prio > s_prio;               /* 同级不打断, 防抖 */
}

bool haptic_play(const char *name, haptic_reject_t *why)
{
    haptic_reject_t w = HAPTIC_OK;
    haptic_init();

    const haptic_fx_t *fx = haptic_effect_find(name);
    if (!fx) {
        w = HAPTIC_REJECT_UNKNOWN;
    } else if (!s_rich) {
        w = HAPTIC_REJECT_DISABLED;      /* 「丰富震动」关: 交由调用方回退基础震动 */
    } else if (!haptic_ready() || !s_timer) {
        w = HAPTIC_REJECT_NO_BACKEND;
    } else {
        const int64_t now = esp_timer_get_time();
        const int idx = (int)(fx - s_fx);
        const bool preempt = s_active && fx->priority > s_prio;

        if (fx->cooldown_ms &&
            (now - s_fx_last_us[idx]) < (int64_t)fx->cooldown_ms * 1000) {
            w = HAPTIC_REJECT_COOLDOWN;
        } else if (!prio_allows(fx->priority)) {
            w = HAPTIC_REJECT_BUSY;
        } else if (!preempt && (now - s_last_start_us) < HAPTIC_MIN_GAP_US) {
            w = HAPTIC_REJECT_RATE;
        } else if (!play_start(fx->segs, fx->n, fx->priority)) {
            w = HAPTIC_REJECT_ARG;
        } else {
            s_fx_last_us[idx] = now;
        }
    }

    if (why) *why = w;
    return w == HAPTIC_OK;
}

/* 基础反馈 → 效果 的语义映射 (只在「丰富震动」开启时接管).
 * 返回 true = 已由效果库起播; false = 交给调用方走原来的 vibrator tap。
 * ★ 关键: 被冷却/优先级拒绝时返回 false —— 连点仍有基础反馈(不会"点了没反应"),
 *   而真正在播序列期间, vibrator 的独占闸门会先一步挡住新点击, 效果不会被清零。 */
static bool rich_hook(vib_sem_t sem, uint16_t ms, uint8_t strength)
{
    (void)ms; (void)strength;      /* 语义决定效果; 这两个量只给"基础震动"兜底用 */
    if (!s_rich) return false;

    const char *name;
    switch (sem) {
    case VIB_SEM_TICK:     name = "slider_tick"; break;   /* 列表/轮播移动一格 */
    case VIB_SEM_STEP:     name = "step";        break;   /* 调值一格: 音量等, 更明显 */
    case VIB_SEM_BACK:     name = "back";        break;   /* 返回: 强→弱双击 */
    case VIB_SEM_DELETE:   name = "delete";      break;   /* 删除: 两记等强重击 */
    case VIB_SEM_FAV:      name = "fav";         break;   /* 收藏: 由弱到强双击 */
    case VIB_SEM_BOUNDARY: name = "boundary";    break;   /* 到头: 轻双击 */
    case VIB_SEM_ALERT:    name = "alert";       break;   /* 关机确认: 两下重击 */
    case VIB_SEM_GAME:     name = "game_hit";    break;   /* 游戏: 短促一击 */
    case VIB_SEM_CONFIRM:
    default:               name = "confirm";     break;   /* 确认/点中: 实心一下 */
    }
    return haptic_play(name, NULL);
}

bool haptic_sequence(const haptic_seg_t *segs, int n, haptic_reject_t *why)
{
    haptic_reject_t w = HAPTIC_OK;
    haptic_init();

    if (!segs || n <= 0 || n > HAPTIC_MAX_SEGS) {
        w = HAPTIC_REJECT_ARG;
    } else if (!s_rich) {
        w = HAPTIC_REJECT_DISABLED;
    } else if (!haptic_ready() || !s_timer) {
        w = HAPTIC_REJECT_NO_BACKEND;
    } else if (!prio_allows(HAPTIC_SEQ_PRIORITY)) {
        w = HAPTIC_REJECT_BUSY;
    } else {
        memcpy(s_custom, segs, (size_t)n * sizeof(segs[0]));
        if (!play_start(s_custom, n, HAPTIC_SEQ_PRIORITY)) w = HAPTIC_REJECT_ARG;
    }

    if (why) *why = w;
    return w == HAPTIC_OK;
}

void haptic_stop(void)
{
    if (s_timer) esp_timer_stop(s_timer);
    /* 只管自己的播放: 没在播就**什么都不做**, 免得把别人的花样/在途 tap 一起掐掉
     * (haptic.stop() 被应用随意调用, 不该误伤其它功能)。 */
    if (s_active) play_finish();
}

bool haptic_active(void) { return s_active; }
uint8_t haptic_active_priority(void) { return s_active ? s_prio : 0; }

void haptic_reset_app(void)
{
    haptic_stop();
    /* 退出兜底: 即使状态错乱(崩溃/超时被杀)也把马达交回去, 否则整机点击反馈会永久失灵 */
    if (s_be && s_be->stop)  s_be->stop();
    if (s_be && s_be->claim) s_be->claim(false);
    memset(s_fx_last_us, 0, sizeof(s_fx_last_us));
    s_last_start_us = 0;
}

/* ==================== 查询 ==================== */
int haptic_effect_count(void) { return FX_N; }

const haptic_fx_t *haptic_effect_at(int i)
{
    if (i < 0 || i >= FX_N) return NULL;
    return &s_fx[i];
}

const haptic_fx_t *haptic_effect_find(const char *name)
{
    if (!name) return NULL;
    for (int i = 0; i < FX_N; i++) {
        if (strcmp(s_fx[i].name, name) == 0) return &s_fx[i];
    }
    return NULL;
}

const char *haptic_reject_name(haptic_reject_t why)
{
    switch (why) {
    case HAPTIC_OK:                 return "ok";
    case HAPTIC_REJECT_UNKNOWN:     return "unknown";
    case HAPTIC_REJECT_NO_BACKEND:  return "no_backend";
    case HAPTIC_REJECT_BUSY:        return "busy";
    case HAPTIC_REJECT_COOLDOWN:    return "cooldown";
    case HAPTIC_REJECT_RATE:        return "rate";
    case HAPTIC_REJECT_ARG:         return "args";
    case HAPTIC_REJECT_DISABLED:    return "disabled";
    }
    return "?";
}
