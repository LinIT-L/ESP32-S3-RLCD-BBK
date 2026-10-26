/**
 * page_pal.c — 仙剑奇侠传（SDLPal 引擎入口页）
 *
 * 这个页面只做三件事：把游戏拉起来、每帧送一次屏、退出时停干净。
 * 游戏数据在 /sdcard/pal（不在这里）—— 由商店的"资源包"解压过去。
 * 应用条目带 .requires="/sdcard/pal"：数据没装时整条不出现在"游戏"分类里。
 */
#include "os_app_api.h"
#include "esp_log.h"
#include <errno.h>
#include <dirent.h>
#include <stdio.h>
#include <sys/stat.h>
#include "pal_entry.h"
#include "pal_port.h"    /* 首帧检测: 载入进度条 */
#include "esp_timer.h"
#include "ui_common.h"
#include "wallpapers.h"   /* wp_idle_hold: 游戏中不自动息屏 */

#define TAG "PAL"

static bool s_running     = false;
/* ★ 2026-10-25 linit「有时候提醒仙剑资源未安装」:
 *   原来 render 里只要 !s_running 就画"资源未安装" —— 退出后/启动中/来回切页都会误报。
 *   现在只有**确实 stat 不到数据目录**时才置位, 其余情况画"正在启动…"。 */
static bool s_res_missing = false;
/* ★★★ 2026-10-25 linit「载入仙剑前面空白太难看, 加进度条; 手柄载入完再显示」:
 *   pal_launch() 只起任务, 第一帧要等 SDLPal 读完资源才出 (实测好几秒)。
 *   这段时间屏幕保持上一个画面/空白 ⇒ 画一个载入页(标题+进度条), 首帧真正上屏后
 *   再切回游戏并把虚拟手柄打开。进度按时间推进到 95%, 首帧到达即消失(不假装 100% 卡住)。 */
static bool    s_loading = false;
/* ★ 2026-10-25: 载入收尾标志 —— 第二次载入比第一次快很多, 进度条按时间插值
 *   往往只走到 30~60% 就被"画面有内容"判定切走 ⇒ 用户看到"进度条不全"。
 *   现在检测到可以进游戏时, 先补画一帧 100%, 下一帧才真正切换。 */
static bool    s_load_finish = false;
static int64_t s_load_us = 0;
static int     s_load_pct = 0, s_last_st = -1;   /* 进度条当前值/阶段, 变化才重画 */

/* ★★★ 2026-10-25 linit「有把握就显示进度条」:
 *   百分比不是凭感觉拍的时间曲线, 而是**真机实测出来的锚点**(本机 TF 卡, stage 日志实测):
 *       0ms    启动            0%
 *     218ms    视频 + 建屏完成   5%
 *    4715ms    资源 + 音频就绪   90%   ← 大头在这里(InitUI/Text/Font/Resources)
 *    4817ms    首场景有内容    100%
 *   两锚点之间线性插值, 过了最后一个锚点**停在 99%**, 直到真正"首场景有内容"才 100%
 *   ⇒ 绝不会出现"条满了但还没进游戏"的假进度; 卡慢时只是停在 99% 等着。
 *   各锚点耗时用 `remcon` 日志里的 `STAGE n @xxms` 可随时复测重标。 */
typedef struct { int ms; int pct; } pal_anchor_t;
static const pal_anchor_t s_anchor[] = {
    {    0,   0 },
    {  220,   5 },
    { 4700,  90 },
    { 4850, 100 },
};

/* ★ 阶段封顶: 进度**不许跑到真实状态前面** —— 每个阶段的插值上限由"下一阶段真实事件"
 *   决定, 事件没到就停在 cap-1 等着。这样日志里 pct 与 stage 永远自洽:
 *   stage=2(资源) 最多 89%, 只有 STAGE 3(音频就绪) 真到了才允许进 90%+。 */
static int pal_stage_cap(int st)
{
    switch (st) {
    case 0:  return 3;
    case 1:  return 5;
    case 2:  return 89;
    case 3:  return 97;
    default: return 99;
    }
}

static int pal_load_pct(int64_t el_ms)
{
    const int n = (int)(sizeof(s_anchor) / sizeof(s_anchor[0]));
    if (el_ms <= s_anchor[0].ms) return s_anchor[0].pct;
    for (int i = 1; i < n; i++) {
        if (el_ms < s_anchor[i].ms) {
            const int span = s_anchor[i].ms - s_anchor[i - 1].ms;
            const int dp   = s_anchor[i].pct - s_anchor[i - 1].pct;
            return s_anchor[i - 1].pct + (int)((el_ms - s_anchor[i - 1].ms) * dp / (span > 0 ? span : 1));
        }
    }
    return 99;                       /* 超时: 停在 99, 不假装完成 */
}

static const char *pal_stage_text(int st)
{
    switch (st) {
    case 1:  return "初始化视频…";
    case 2:  return "载入游戏资源…";
    case 3:  return "准备音频…";
    case 5:  return "马上就好…";
    default: return "正在启动…";
    }
}

static void pal_bar_px(st7305_handle_t *lcd, int x, int y)
{
    if (x < 0 || y < 0 || x >= ST7305_WIDTH || y >= ST7305_HEIGHT) return;
    st7305_draw_pixel(lcd, x, y, ST7305_COLOR_BLACK);
}

static void pal_draw_loading(ui_ctx_t *ctx, int pct)
{
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return;
    st7305_clear(lcd, ST7305_COLOR_WHITE);
    ui_text16c(lcd, ST7305_WIDTH / 2, 108, "仙剑奇侠传", false);
    ui_text16c(lcd, ST7305_WIDTH / 2, 186, pal_stage_text(pal_port_load_stage()), false);

    /* 进度条: 外框 240×14, 内填按 pct */
    const int x0 = 80, y0 = 150, w = 240, h = 14;
    for (int i = 0; i < w; i++) { pal_bar_px(lcd, x0 + i, y0); pal_bar_px(lcd, x0 + i, y0 + h - 1); }
    for (int j = 0; j < h; j++) { pal_bar_px(lcd, x0, y0 + j); pal_bar_px(lcd, x0 + w - 1, y0 + j); }
    int fill = (w - 6) * pct / 100;
    for (int j = 2; j < h - 2; j++)
        for (int i = 0; i < fill; i++) pal_bar_px(lcd, x0 + 3 + i, y0 + j);

    {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d%%", pct);
        ui_text16c(lcd, ST7305_WIDTH / 2, 210, buf, false);
    }
}

/* 物理动作 -> 仙剑按键 (OS 动作只有"按下"事件, 松开由 s_hold 计时补) */
static pal_key_t s_key  = PAL_KEY_NONE;
static int       s_hold = 0;

static pal_key_t map_action(os_action_t a)
{
    switch (a) {
    case OS_ACTION_UP:      return PAL_KEY_UP;
    case OS_ACTION_DOWN:    return PAL_KEY_DOWN;
    case OS_ACTION_LEFT:    return PAL_KEY_LEFT;
    case OS_ACTION_RIGHT:   return PAL_KEY_RIGHT;
    case OS_ACTION_CONFIRM: return PAL_KEY_OK;
    default:                return PAL_KEY_NONE;
    }
}

static void pal_on_enter(ui_ctx_t *ctx)
{
    (void)ctx;
    /* ★ 已在运行: 只是页面重入(切页/remcon page pal), 不要重置载入进度和送屏开关,
     *   否则会二次显示载入页、并把 pause_output 又打开。 */
    /* ★★★ 2026-10-25 修「第二次进游戏没有加载进度条」:
     *   原来只判页面自己的 s_running —— 只要它残留 true 就直接 return:
     *   既不重启游戏、也不重置 s_loading ⇒ 第二次进来**没有进度条、也没有新一局**。
     *   现在用引擎侧的真实状态交叉验证: pal_is_running() 为 false 说明上一局任务
     *   已经不在了, 页面这个 s_running 就是脏状态, 必须复位后重新走完整启动流程。 */
    if (s_running && pal_is_running()) return;   /* 真的在跑才算重入 */
    if (s_running) {
        ESP_LOGW(TAG, "页面状态残留(引擎已停) ⇒ 复位并重新启动");
        s_running = false;
        s_loading = false;
    }

    /* ★★★ 2026-10-25 linit「二次打开经常提示资源未下载」:
     *   原来只用 stat(目录) 判一次, 而 FATFS 在 SD 监视线程扫描、或上一轮刚退出时
     *   会**瞬时**失败 ⇒ 误判成"资源未安装"。现在:
     *     ① 用 opendir 判目录(比 stat 稳) + 用 fopen 打开 SSS.MKF 交叉验证;
     *     ② 重试 15 次 × 100ms = 1.5s, 覆盖 SD 忙的时间窗;
     *     ③ 失败时把 errno 打出来, 便于区分"真没装"和"文件系统忙"。 */
    {
        char probe[160];
        snprintf(probe, sizeof(probe), "%s/SSS.MKF", pal_entry_datadir());
        bool ok = false;
        for (int i = 0; i < 15; i++) {
            DIR *d = opendir(pal_entry_datadir());
            if (d) {
                closedir(d);
                FILE *f = fopen(probe, "rb");
                if (f) { fclose(f); ok = true; break; }
                ESP_LOGW(TAG, "目录在但 %s 打不开 (errno=%d)", probe, errno);
            } else {
                ESP_LOGW(TAG, "opendir 失败 %s (errno=%d) 第%d次", pal_entry_datadir(), errno, i + 1);
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        s_res_missing = !ok;
        if (ok) ESP_LOGI(TAG, "资源就绪: %s", pal_entry_datadir());
        else    ESP_LOGE(TAG, "资源确实不可用(重试15次): %s", pal_entry_datadir());
    }
    if (s_res_missing || !pal_launch()) {
        s_running = false;
        if (!s_res_missing) ESP_LOGE(TAG, "启动失败: 内存不足");
        return;
    }
    s_running = true;
    s_key = PAL_KEY_NONE;
    s_hold = 0;
    ctx->fullscreen = true;
    /* ★★★ 2026-10-25 linit: 仙剑要显示虚拟手柄。引擎侧早有 pal_keys_enable()，
     *   但页面从来没调过 ⇒ 屏幕上一个虚拟键都没有（GBC/NES 都有，就仙剑没有）。
     *   开启后由 pal_port_present() 每帧画在游戏帧之上。 */
    pal_keys_enable(false);        /* 手柄等载入完成再显示 */
    s_loading = true;
    s_load_us = esp_timer_get_time();
    s_load_pct = 0; s_last_st = -1; s_load_finish = false;
    /* ★ 片头(软星 logo / 仙剑 logo)是白底浅灰, 1bpp 下整片糊白, 就是"白屏一会"的来源。
     *   载入期间**完全不送屏**, 屏幕整块留给载入页 ⇒ 片头不再漏到屏上。 */
    pal_port_pause_output(true);
    /* ★★★ 2026-10-25 linit「单独限制仙剑奇侠传帧率」: 只限这一路, UI 和其他引擎不动。
     *   20fps = 50ms 一帧; 超时的帧直接丢(不阻塞主循环), 面板本身也只有 16~24Hz。 */
    pal_set_max_fps(20);
    /* 游戏中不自动息屏/深睡: 既避免"玩到一半黑屏", 也减少面板进休眠被锁死的机会 */
    wp_idle_hold(true);
    ESP_LOGI(TAG, "仙剑已启动");
}

static void pal_on_exit(ui_ctx_t *ctx)
{
    (void)ctx;
    s_loading = false;
    s_load_finish = false;          /* 收尾标志也要清, 否则下次进来第一帧就判"已收尾" */
    pal_port_pause_output(false);   /* 恢复送屏, 否则下次进来仍是暂停态 */
    pal_keys_enable(false);     /* 退出必须关, 否则虚拟键会残留在别的页面 */
    wp_idle_hold(false);        /* 退出必须解除占位, 否则整机再也不息屏 */
    /* ★ 2026-10-25: 无条件做全量退出 —— s_running 可能已经是 false(BACK 分支先置过),
     *   但音频任务仍可能活着 ⇒ 必须每次退出都把 sdlpal/sdl_aud 清干净。 */
    pal_shutdown();
    s_running = false;
    s_res_missing = false;
    ESP_LOGI(TAG, "退出仙剑");
}

static void pal_render(ui_ctx_t *ctx)
{
    if (s_running && s_loading) { pal_draw_loading(ctx, s_load_pct); return; }   /* 载入页 */
    if (s_running) return;      /* 运行中画面由 pal_tick 直接刷屏，这里不画 */
    st7305_clear(ctx->lcd, ST7305_COLOR_WHITE);
    if (s_res_missing) {
        ui_text16(ctx->lcd, 20, 60, "\xe4\xbb\x99\xe5\x89\x91\xe8\xb5\x84\xe6\xba\x90\xe6\x9c\xaa\xe5\xae\x89\xe8\xa3\x85", false); /* 仙剑资源未安装 */
        ui_text16(ctx->lcd, 20, 96, "\xe8\xaf\xb7\xe5\x85\x88\xe5\x9c\xa8\xe5\xba\x94\xe7\x94\xa8\xe5\x95\x86\xe5\xba\x97\xe4\xb8\x8b\xe8\xbd\xbd\xe8\xb5\x84\xe6\xba\x90\xe5\x8c\x85", false); /* 请先在应用商店下载资源包 */
        ui_text16(ctx->lcd, 20, 140, "\xe6\x8c\x89 BACK \xe8\xbf\x94\xe5\x9b\x9e", false); /* 按 BACK 返回 */
    } else {
        ui_text16(ctx->lcd, 20, 60, "\xe6\xad\xa3\xe5\x9c\xa8\xe5\x90\xaf\xe5\x8a\xa8\xe2\x80\xa6", false); /* 正在启动… */
        ui_text16(ctx->lcd, 20, 140, "\xe6\x8c\x89 BACK \xe8\xbf\x94\xe5\x9b\x9e", false);
    }
}

static void pal_action(ui_ctx_t *ctx, os_action_t a)
{
    if (a == OS_ACTION_BACK) {
        /* ★ 2026-10-25 linit: 所有应用按返回键先弹"退出程序?"确认.
         *   仙剑是自绘帧的游戏, 走冻结帧直绘版确认框 (与各模拟器引擎同一外观),
         *   确认才真退出; 取消则继续游戏 (pal_tick 下一帧照常推进). */
        if (s_running) {
            /* ★★★ 2026-10-25「长按返回强制退不出去」的真正原因:
             *   确认框是 os_confirm_exit_blocking() 直绘 + st7305_flush() **一次**,
             *   而游戏任务(sdlpal, 另一个核)仍在每帧 pal_tick → pal_port_present → 送屏,
             *   一帧就把框整块盖掉。用户看到的是游戏画面, 按"确定"又被游戏吃掉
             *   ⇒ 表现就是"怎么都退不出来", 只能按复位。
             *   弹框前先停游戏送屏(只置标志, 不干扰游戏任务), 取消则恢复并重画一帧。 */
            pal_port_pause_output(true);
            bool ok = os_confirm_exit_blocking(ctx->lcd);
            if (!ok) {
                pal_port_pause_output(false);
                pal_port_mark_dirty();     /* 恢复后立刻整帧重画, 不留确认框残影 */
                return;
            }
            pal_stop_and_wait(300);    /* 确认退出: 内部=pal_shutdown(停音频+杀任务+清残留) */
        }
        s_running = false;
        os_pop(ctx);
        return;
    }
    pal_key_t k = map_action(a);
    if (k != PAL_KEY_NONE && s_running) {
        pal_input(k, 1);
        s_key  = k;
        s_hold = 6;                  /* 约 6 帧后自动松开 (OS 动作只有"按下"事件) */
    }
}

static bool pal_touch(ui_ctx_t *ctx, int x, int y)
{
    (void)ctx; (void)x; (void)y;
    return s_running;                /* 运行中把点击全部吃掉, 不当空白点击 */
}

static void pal_poll(ui_ctx_t *ctx)
{
    if (!s_running) return;
    /* 载入阶段: 只刷进度条, 不喂键/不画虚拟手柄 (pal_poll_keys 会自愈重开手柄, 必须避开) */
    if (s_loading) {
        /* 输出是暂停的, pal_tick 只采样 8bpp 内容、不写 fb 不送屏 (屏幕归载入页)。
         * 判据 = 画面里已经有真内容(连续 3 帧暗部>=6%), 不是"首帧到了" ——
         * 片头那几帧全是白的, 按首帧切就正好切在白屏上(上一版就是这么白了一屏)。 */
        pal_tick(ctx->lcd);
        int64_t el = (esp_timer_get_time() - s_load_us) / 1000;
        if (pal_port_scene_ready() || el > 60000) {
            /* ★★★ 2026-10-25 修「二次进入游戏进度条不全」:
             *   进度是按**时间锚点**插值的（锚点按第一次约 5.5s 标定），
             *   而第二次进来时 SD/资源都热了，往往 1~2 秒就满足"画面有内容"，
             *   于是进度条只走到 30~60% 就被切走 —— 看起来"不全"。
             *   这里先补一帧 100% 并且让 os_render 真的把它刷出去，下一帧才进游戏。
             *   代价只有一帧(≈16ms)，换来"每次都看到走完"。 */
            if (!s_load_finish) {
                s_load_finish = true;
                s_load_pct    = 100;
                ctx->needs_redraw = true;
                ESP_LOGI(TAG, "载入收尾: 补 100%% (实际 %dms stage=%d)",
                         (int)el, pal_port_load_stage());
                return;                       /* 本帧只画 100%，下一帧再切换 */
            }
            s_loading = false;
            pal_port_pause_output(false);     /* 恢复送屏 */
            pal_port_mark_dirty();
            pal_keys_enable(true);            /* 这时才显示虚拟手柄 */
            ESP_LOGI(TAG, "载入完成 %dms stage=%d 暗部=%d%% → 显示游戏与手柄",
                     (int)el, pal_port_load_stage(), pal_port_scene_dark_pct());
        } else {
            int st = pal_port_load_stage();
            int pct = pal_load_pct(el);
            if (pct > pal_stage_cap(st)) pct = pal_stage_cap(st);   /* 不许超过本阶段上限 */
            if (pct != s_load_pct || st != s_last_st) {   /* 进度/阶段变了才重画, 不空转刷屏 */
                /* 状态日志: 每次跨 10% 或换阶段打一行 —— 不用截图, 看日志就能核对
                 * 进度是否单调、锚点是否贴合(配合 pal_port 的 STAGE n @xxms)。 */
                if (pct / 10 != s_load_pct / 10 || st != s_last_st)
                    ESP_LOGI(TAG, "载入 %d%% stage=%d (+%dms 暗部=%d%%)",
                             pct, st, (int)el, pal_port_scene_dark_pct());
                s_load_pct = pct; s_last_st = st;
                ctx->needs_redraw = true;
            }
            return;
        }
    }

    /* 物理键脉冲松开 */
    if (s_key != PAL_KEY_NONE && --s_hold <= 0) {
        pal_input(s_key, 0);
        s_key = PAL_KEY_NONE;
    }

    if (!pal_is_running()) {         /* 游戏自己退出了 (菜单→退出) */
        s_running = false;
        os_pop(ctx);
        return;
    }
    /* ★★★ 2026-10-25 linit: 长按返回(3s)出的是 OS 的"退出程序?"确认框，
     *   但仙剑每帧 pal_tick() 会把游戏帧整屏怼上去，**把确认框刷掉** ⇒
     *   用户什么都看不到，以为"长按返回强行退出不了"。
     *   有弹窗时停止送帧，把屏幕让给弹窗（游戏逻辑在独立任务里继续跑，不受影响）。 */
    if (os_dialog_depth() > 0) return;
    /* 手柄 / 屏幕虚拟按键 → 仙剑按键 (虚拟键只写 joypad 位, 必须在这里桥接) */
    pal_poll_keys();
    pal_tick(ctx->lcd);
}

static const os_module_t s_mod_pal = {
    .name      = "pal",
    .page_id   = OS_PAGE_PAL,
    .on_enter  = pal_on_enter,
    .on_exit   = pal_on_exit,
    .render    = pal_render,
    .action    = pal_action,
    .touch     = pal_touch,
    .poll      = pal_poll,
    .modal     = false,
    .fullscreen = true,
};

void os_page_pal_register(void)
{
    os_register(&s_mod_pal);
}
