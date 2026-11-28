/**
 * page_storage.c — 存储管理 页面模块 (3.3 版 list_dialog 复刻).
 *
 * 主菜单"存储"打开 → 存储管理列表弹窗:
 *   浏览文件 / 挂载到电脑 / 格式化TF卡 / 存储信息 / 返回
 * 与 3.3 一致用 list_dialog 弹窗样式 (固定 350×250, 底部固定返回行).
 * 私有 state 全部 static 留在本文件.
 */
#include "os.h"
#include "ui_common.h"   /* UI_SCREEN_W/H, fill_rect, draw_hline/vline, draw_text_centered */
#include "input.h"
#include "sd_scan.h"
#include "usbh_msc_sdspi.h"
#include "web_files.h"     /* ★ 2026-11-01 第三回合: TF 卡网页共享 (/files/) */
#include "wifi_manager.h"  /* 开共享时可能需要开热点; 状态栏 NET 图标 */
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>

#define TAG "PSTO"

static void open_sd_dlg(ui_ctx_t *ctx);
static void open_sd_share(ui_ctx_t *ctx);   /* ★ 第三回合: 网络访问存储 (定义在下面) */
static void open_sd_format(ui_ctx_t *ctx);  /* ★ 2026-11-01: 入口移到「存储信息」页顶部, 需前置声明 */

/* 返回/取消 → 关弹窗 + 直接退出存储页 (回主菜单).
 * 用户规范 2026-09-28: **存储只做弹窗**, 不要独立的整页界面 (不要"按返回键退出"那种全屏页)。
 * 实现: 先 os_dialog_pop 关掉弹窗(s_cb_handled 置位 → dlg_finish_top 不会再补弹一次),
 * 再显式 os_pop 退出存储页。两步都写在这里, 意图明确, 不依赖"双重弹栈"的巧合。 */
static void sd_close(ui_ctx_t *ctx, int result, void *ud) {
    (void)result; (void)ud;
    os_dialog_pop(ctx);   /* 关存储主弹窗 */
    os_pop(ctx);          /* 退出存储页本体 → 回主菜单 */
}

/* ---- 存储信息弹窗 (3.3: 只读多行 + 返回) ---- */
/* ★ 2026-11-01: 标记"本次格式化是从存储信息页进来的" —— 格式化完成后要连那一层一起关，
 *   否则会停在一个显示**格式化前**容量/剩余的旧快照页上（数字全是过期的）。 */
static bool s_fmt_from_info = false;

static void sd_info_cb(ui_ctx_t *ctx, int result, void *ud) {
    (void)ud;
    /* ★ 2026-11-01: 顶部第一项 = 「格式化TF卡」(原来在主菜单里, 现挪到本页顶部)。
     *   其余几行是只读信息, 点了不该关窗 → 用 mark_handled 阻止自动 pop。
     *   （open_sd_format 内部的 os_dialog_push 本身也会置 handled, 父层不受影响。） */
    if (result == 0) { s_fmt_from_info = true; open_sd_format(ctx); return; }
    if (result >= 0) os_dialog_mark_handled(ctx);
}

static void open_sd_info(ui_ctx_t *ctx) {
    EXT_RAM_BSS_ATTR static char items[6][40];
    static const char *ptrs[6];
    int n = 0;
    uint64_t total = 0, free = 0;
    /* ★ 2026-11-01 linit: "格式化TF卡" 放到本页**顶部**，作为可点项；主菜单里已去掉。 */
    snprintf(items[n++], sizeof(items[0]), "%s",
             "\xe6\xa0\xbc\xe5\xbc\x8f\xe5\x8c\x96TF\xe5\x8d\xa1");   /* 格式化TF卡 */
    /* ★ 2026-10-18: 第一行原来是 CID 型号名("SD16G"), 用户会当成容量 —— 那是出厂型号, 不是容量。
     *   现在显示 **品牌 + 标称容量**(按字节换十进制 GB 四舍五入: 3834MB→4G / 14.8GiB→16G)。
     * ★ 2026-11-01 linit: 原来第二行「名称: xxx」(出厂 CID 名) 不再显示。 */
    uint64_t cap_bytes = sd_get_size_bytes();
    if (cap_bytes > 0) {
        uint32_t nominal_g = (uint32_t)((cap_bytes + 500000000ULL) / 1000000000ULL);
        if (nominal_g == 0) nominal_g = 1;
        snprintf(items[n++], sizeof(items[0]), "\xe5\x8d\xa1: %s %uG",      /* 卡: 品牌 XG */
                 sd_get_vendor_str(), (unsigned)nominal_g);
    } else {
        snprintf(items[n++], sizeof(items[0]), "\xe5\x8d\xa1: %s", sd_get_cid_name());   /* 卡: xxx */
    }
    if (sd_get_info(&total, &free) == 0) {
        uint32_t total_mb = (uint32_t)(total / (1024ULL * 1024ULL));
        uint32_t free_mb  = (uint32_t)(free  / (1024ULL * 1024ULL));
        uint32_t used_mb  = total_mb > free_mb ? (total_mb - free_mb) : 0;
        snprintf(items[n++], sizeof(items[0]), "\xe5\xae\xb9\xe9\x87\x8f: %luMB", (unsigned long)total_mb); /* 总容量 */
        snprintf(items[n++], sizeof(items[0]), "\xe5\xb7\xb2\xe7\x94\xa8:  %luMB", (unsigned long)used_mb);            /* 已用 */
        snprintf(items[n++], sizeof(items[0]), "\xe5\x89\xa9\xe4\xbd\x99:  %luMB", (unsigned long)free_mb);            /* 剩余 */
    } else {
        /* 未挂载: 显示**具体原因** (卡无响应 / exFAT 不支持 / 无文件系统), 而不是笼统一句"未挂载" */
        snprintf(items[n++], sizeof(items[0]), "\xe7\x8a\xb6\xe6\x80\x81: %s", sd_get_mnt_state_str()); /* 状态: ... */
    }
    for (int i = 0; i < n; i++) ptrs[i] = items[i];
    os_dialog_list(ctx, "\xe5\xad\x98\xe5\x82\xa8\xe4\xbf\xa1\xe6\x81\xaf", ptrs, n, 0, sd_info_cb, NULL); /* 存储信息 */
}

/* ---- 格式化TF卡: 三次重复确认防误触, 第 3 次确认才真正执行 ---- */
static int s_fmt_stage = 0;   /* 1..3 当前确认到第几步; 取消/完成归零 */

static void sd_format_stage(ui_ctx_t *ctx);   /* 按 s_fmt_stage 弹出当前步的确认框 */

/* ---- 格式化进度弹窗 (2026-10-01) ----
 * 之前格式化在 UI 回调里同步跑 → 全卡擦除数十秒把界面冻住 (用户实测"卡死、无进度条")。
 * 现在: 任务在后台跑, 这里用弹窗的 on_render 钩子自己画进度条 + 阶段文字。 */
#define FMT_DLG_W 300
#define FMT_DLG_H 126
static bool fmt_progress_render(ui_ctx_t *ctx, os_dlg_stack_t *d, void *ud);
static void fmt_progress_poll(ui_ctx_t *ctx, os_dlg_stack_t *d, void *ud);
static bool fmt_progress_key(ui_ctx_t *ctx, os_dlg_stack_t *d, os_action_t a, void *ud);

static bool fmt_progress_render(ui_ctx_t *ctx, os_dlg_stack_t *d, void *ud)
{
    (void)ud;
    st7305_handle_t *lcd = ctx->lcd;
    int w = FMT_DLG_W, h = FMT_DLG_H;
    int x = (UI_SCREEN_W - w) / 2, y = (UI_SCREEN_H - h) / 2;

    /* 弹窗不清屏 → 自己把本框范围刷白 (与系统弹窗同族: 2px 边框) */
    fill_rect(lcd, x, y, x + w - 1, y + h - 1, ST7305_COLOR_WHITE);
    draw_dialog_frame(lcd, x, y, w, h, 2);

    ui_text16c(lcd, x + w / 2, y + 10, d->title, false);

    int pct = sd_format_pct();
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    int bx = x + 24, by = y + 40, bw = w - 48, bh = 18;
    draw_rect_outline(lcd, bx, by, bx + bw - 1, by + bh - 1, ST7305_COLOR_BLACK);
    int inner = bw - 4;
    int fillw = inner * pct / 100;
    if (fillw > 0)
        fill_rect(lcd, bx + 2, by + 2, bx + 2 + fillw - 1, by + bh - 3, ST7305_COLOR_BLACK);

    char buf[32];
    snprintf(buf, sizeof(buf), "%d%%", pct);
    ui_text16c(lcd, x + w / 2, by + bh + 6, buf, false);
    /* 阶段文字 (驱动给的 UTF-8, 如 "擦除中") */
    const char *st = sd_format_stage_str();
    if (st && st[0]) ui_text16c(lcd, x + w / 2, y + h - 26, st, false);
    return true;    /* 已消费: 不再走默认列表渲染 */
}

static void fmt_progress_poll(ui_ctx_t *ctx, os_dlg_stack_t *d, void *ud)
{
    (void)d; (void)ud;
    if (sd_format_is_running()) { ctx->needs_redraw = true; return; }   /* 进度在动 → 持续重绘 */
    int rc = sd_format_result();
    os_dialog_pop(ctx);                 /* 关进度窗 */
    if (s_fmt_from_info) {              /* 从存储信息页进来的: 连带关掉那一层(数据已过期), 回主菜单 */
        s_fmt_from_info = false;
        os_dialog_pop(ctx);
    }
    os_dialog_toast(ctx, (rc == 0)
                    ? "\xe6\xa0\xbc\xe5\xbc\x8f\xe5\x8c\x96\xe5\xae\x8c\xe6\x88\x90"        /* 格式化完成 */
                    : "\xe6\xa0\xbc\xe5\xbc\x8f\xe5\x8c\x96\xe5\xa4\xb1\xe8\xb4\xa5");     /* 格式化失败 */
    ctx->needs_redraw = true;
}

static bool fmt_progress_key(ui_ctx_t *ctx, os_dlg_stack_t *d, os_action_t a, void *ud)
{
    (void)ctx; (void)d; (void)a; (void)ud;
    return true;    /* 格式化进行中吞掉按键 (完成由 on_poll 收尾, 避免中途退出留悬空状态) */
}

static void sd_format_done(ui_ctx_t *ctx, int result, void *ud) {
    (void)ud;
    if (result != 0) {           /* 取消 / 返回: 直接关确认框, 归零 */
        s_fmt_stage = 0;
        s_fmt_from_info = false; /* ★ 取消时标记也要清, 否则下次完成会多关一层 */
        return;
    }
    os_dialog_pop(ctx);          /* 确认: 先关当前确认框 */
    if (s_fmt_stage < 3) {
        s_fmt_stage++;
        sd_format_stage(ctx);    /* 还有未走完的确认 → 出下一步 */
    } else {
        s_fmt_stage = 0;
        if (!sd_format_start_async()) {
            os_dialog_toast(ctx, "\xe6\xa0\xbc\xe5\xbc\x8f\xe5\x8c\x96\xe5\x90\xaf\xe5\x8a\xa8\xe5\xa4\xb1\xe8\xb4\xa5"); /* 格式化启动失败 */
            return;
        }
        /* 进度弹窗: on_render 自绘进度条, on_poll 完成后收尾 (不阻塞 UI) */
        static os_dlg_stack_t d;
        memset(&d, 0, sizeof(d));
        snprintf(d.title, sizeof(d.title), "%s", "\xe6\xa0\xbc\xe5\xbc\x8f\xe5\x8c\x96\xe4\xb8\xad"); /* 格式化中 */
        d.no_footer  = true;
        d.on_render  = fmt_progress_render;
        d.on_poll    = fmt_progress_poll;
        d.on_key     = fmt_progress_key;
        os_dialog_push(ctx, &d);
    }
}

static void sd_format_stage(ui_ctx_t *ctx) {
    const char *msg;
    switch (s_fmt_stage) {
    case 1: msg = "\xe7\xa1\xae\xe5\xae\x9a\xe6\xa0\xbc\xe5\xbc\x8f\xe5\x8c\x96TF\xe5\x8d\xa1?"; /* 确定格式化TF卡? */
        break;
    case 2: msg = "\xe5\xb0\x86\xe6\xb8\x85\xe7\xa9\xba\xe5\x8d\xa1\xe5\x86\x85\xe6\x89\x80\xe6\x9c\x89\xe6\x95\xb0\xe6\x8d\xae!"; /* 将清空卡内所有数据! */
        break;
    default: msg = "\xe6\x9c\x80\xe5\x90\x8e\xe7\xa1\xae\xe8\xae\xa4\xe6\xa0\xbc\xe5\xbc\x8f\xe5\x8c\x96?"; /* 最后确认格式化? */
        break;
    }
    os_dialog_confirm_ex(ctx, msg, 0, 0, sd_format_done, NULL);
}

static void open_sd_format(ui_ctx_t *ctx) {
    /* ★ 2026-11-22/2026-11-25: 卡交出去期间禁止格式化 ——
     *   否则会在 PC 正在读写这张卡的时候抢占 SDMMC 并整卡擦除 (PC 侧直接损坏)。
     *   用 card_busy() 而不是 state==USB: PREPARING/STOPPING/RESTORING 这三个过渡态
     *   同样不能碰卡。驱动层 sd_format_start_async() 仍有同一道闸, 这里先给可读提示。 */
    if (usbh_msc_card_busy() || sd_usb_owned()) {
        os_dialog_toast(ctx, "请先退出电脑挂载");
        return;
    }
    s_fmt_stage = 1;
    sd_format_stage(ctx);
}

/* =====================================================================
 * 挂载/卸载 —— 一个弹窗按状态原地变形, 真实流程全在后台 (UI 不卡顿)。
 *
 * 历史: 最初在 UI 回调里同步执行 (Serial/JTAG 卸载 + tinyusb 安装 + PHY 切 OTG
 * 十几秒, 白屏卡死) → 后来改成两个后台任务 + 一堆界面标志。
 * 2026-11-25 收敛成: 界面只发请求 + 按 usbh_msc 状态机渲染 (见下面的大段说明)。
 * ===================================================================== */
#include "mount_icon.inc"
#include "web_icon.inc"     /* ★ 2026-11-01: 「网络访问存储」用的浏览器图标 64x64 */

/* ═══════════════════════════════════════════════════════════════════════════
 * USB 访问存储: 界面只做两件事 —— 发请求 + 按状态渲染
 *
 * 2026-11-25 重构: 这里原来自己维护 7 个状态变量 (s_mount_result / s_unmounting /
 *   s_vfs_restoring / s_mount_t0 / s_unmount_t0 / s_vfs_t0 / s_mount_cancel)、
 *   两个后台任务 (mount_worker / unmount_worker) 和三处超时, 与 usbh_msc、sd_scan
 *   各自记的"卡归谁"互相打架, 于是出现:
 *     · "电脑上已卸载, 机器上弹窗不关" —— 关窗条件其实挂在本地 VFS 重挂上;
 *     · "提示存储恢复中, 只能重启" —— 守卫没有超时, 后台卡在挂载锁上就永久拒绝;
 *     · "超时后放行入口" —— 状态被界面改掉, 后台却还在跑。
 *   现在生命周期全部归 usbh_msc 的会话状态机 (见 usbh_msc_sdspi.h), 本文件:
 *     · 发请求: usbh_msc_request_start / stop / cancel
 *     · 读快照: usbh_msc_status()  (state + outcome + seq)
 *     · 渲染:   状态变一次就整体重画一次 (seq 就是"有没有新事件"的判据)
 *   ⛔ 不要在这里重新加"正在挂载/正在卸载"之类的本地标志或超时。
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 弹窗打开那一刻的状态 seq —— 只用于"自打开以来有没有新事件",
 * 是**弹窗作用域**的基准值, 不是生命周期状态。 */
static uint32_t s_dlg_seq0 = 0;

static void mount_dlg_render(os_dlg_stack_t *d, usbh_msc_status_t st);
static void mount_dlg_poll(ui_ctx_t *ctx, os_dlg_stack_t *d, void *ud);
static void mount_btn_cb(ui_ctx_t *ctx, int result, void *ud);

/* 按状态整体改写弹窗内容。同一份代码既用于"首次打开"也用于"状态变了重绘",
 * 因此不存在"某个转换忘了改标题/按钮"这类问题。
 * ⚠ 只填字段, **不做 memset** —— 它也会被用于改写活弹窗(os_dialog_top_mut)。 */
static void mount_dlg_render(os_dlg_stack_t *d, usbh_msc_status_t st)
{
    const int64_t nowm = (int64_t)(esp_timer_get_time() / 1000);
    const int64_t in_state_ms = st.since_ms ? (nowm - (int64_t)st.since_ms) : 0;

    d->title[0]  = '\0';
    d->count     = 0;
    d->sel       = 0;
    d->icon      = NULL;
    d->icon_w    = 0;
    d->icon_h    = 0;
    d->no_footer = true;
    d->small     = true;
    d->cb        = mount_btn_cb;
    d->on_poll   = mount_dlg_poll;
    /* 宽度按"确认框"定死: 挂载中/已挂载都是同一个框原位变形, 宽度跳变很显眼 */
    d->fix_w     = text_width("确定挂载到电脑?") + 24;

    switch (st.state) {
    case USBH_MSC_IDLE:
    case USBH_MSC_FAULT:
        snprintf(d->title, sizeof(d->title), "%s",
                 (st.state == USBH_MSC_FAULT) ? "重新挂载到电脑?" : "确定挂载到电脑?");
        d->count = 2;
        snprintf(d->items[0], sizeof(d->items[0]), "确定");
        snprintf(d->items[1], sizeof(d->items[1]), "取消");
        break;

    case USBH_MSC_PREPARING:
        /* 图标 + 单个「取消」: 后台在每个阶段边界都查取消闩锁, 点了就一定会收手
         * (没交卡就直接退; 已交卡就立刻收回), 不会留下半挂状态。 */
        if (in_state_ms > 20000) snprintf(d->title, sizeof(d->title), "挂载较慢…");
        d->icon   = mount_icon_tf_64;
        d->icon_w = MOUNT_ICON_W;
        d->icon_h = MOUNT_ICON_H;
        d->count  = 1;
        snprintf(d->items[0], sizeof(d->items[0]), "取消");
        break;

    case USBH_MSC_USB:
        d->icon   = mount_icon_tf_64;
        d->icon_w = MOUNT_ICON_W;
        d->icon_h = MOUNT_ICON_H;
        d->count  = 2;
        snprintf(d->items[0], sizeof(d->items[0]), "卸载");
        snprintf(d->items[1], sizeof(d->items[1]), "取消");
        break;

    case USBH_MSC_STOPPING:
        /* USB 侧还在退(等主机完成未结的 SCSI 命令)。只报告, 不假装完成。 */
        if (in_state_ms > 15000) snprintf(d->title, sizeof(d->title), "卸载较慢…");
        d->icon   = mount_icon_tf_64;
        d->icon_w = MOUNT_ICON_W;
        d->icon_h = MOUNT_ICON_H;
        d->count  = 1;
        snprintf(d->items[0], sizeof(d->items[0]), "卸载中");
        break;

    default:   /* RESTORING: 正常情况下弹窗已关(poll 关的), 这里只是兜底 */
        d->count = 1;
        snprintf(d->items[0], sizeof(d->items[0]), "存储恢复中");
        break;
    }
}

static void mount_dlg_poll(ui_ctx_t *ctx, os_dlg_stack_t *d, void *ud)
{
    (void)ud;
    usbh_msc_status_t st = usbh_msc_status();

    if (st.seq == s_dlg_seq0) {
        /* 没有新事件: 只在"卡在过渡态很久"时补一次文字提示 ——
         * 纯提示, **不改状态、不放行任何操作**(放行只能等状态机自己迁移)。 */
        const int64_t nowm = (int64_t)(esp_timer_get_time() / 1000);
        const int64_t el   = st.since_ms ? (nowm - (int64_t)st.since_ms) : 0;
        if (d->title[0] == '\0' &&
            ((st.state == USBH_MSC_PREPARING && el > 20000) ||
             (st.state == USBH_MSC_STOPPING  && el > 15000))) {
            mount_dlg_render(d, st);
            ctx->needs_redraw = true;
        }
        return;
    }
    s_dlg_seq0 = st.seq;

    if (st.state == USBH_MSC_RESTORING) {
        /* USB 侧确实已经退出 = 用户视角的"卸载完成"。此刻就关窗,
         * 后面本地 VFS 恢复再慢也不影响界面 —— 这正是原 bug 的判据。 */
        os_dialog_pop(ctx);
        os_dialog_toast(ctx, "已卸载");
        return;
    }
    if (st.state == USBH_MSC_IDLE || st.state == USBH_MSC_FAULT) {
        os_dialog_pop(ctx);
        switch (st.outcome) {
        case USBH_MSC_OP_START_FAIL_VFS: os_dialog_toast(ctx, "卸载存储失败"); break;
        case USBH_MSC_OP_START_FAIL_USB: os_dialog_toast(ctx, "挂载失败"); break;
        case USBH_MSC_OP_START_BUSY:     os_dialog_toast(ctx, "正在传输文件，请稍后再试"); break;
        case USBH_MSC_OP_CANCELLED:      os_dialog_toast(ctx, "已取消挂载"); break;
        case USBH_MSC_OP_STOP_OK:        os_dialog_toast(ctx, "已卸载"); break;
        case USBH_MSC_OP_STOP_DEGRADED:  os_dialog_toast(ctx, "已卸载，但存储未恢复"); break;
        case USBH_MSC_OP_STOP_USB_STUCK: os_dialog_toast(ctx, "USB 未退干净，请重启"); break;
        case USBH_MSC_OP_STOP_DEFERRED:  os_dialog_toast(ctx, "电脑仍在写入，请在电脑上弹出后重试"); break;
        default: break;      /* 状态被别处改回终态: 不提示 */
        }
        return;
    }
    mount_dlg_render(d, st);          /* PREPARING / USB / STOPPING: 原位变形 */
    ctx->needs_redraw = true;
}

/* 弹窗按钮: items[0] 永远是主操作(确定/取消/卸载), items[1] 是「取消」。 */
static void mount_btn_cb(ui_ctx_t *ctx, int result, void *ud)
{
    (void)ud;
    usbh_msc_state_t st = usbh_msc_status().state;

    if (result != 0) {                        /* 取消 / 返回键 */
        if (st == USBH_MSC_PREPARING) {       /* 交卡中: 请求撤销, 保持弹窗看结果 */
            usbh_msc_request_cancel();
            os_dialog_mark_handled(ctx);
        } else if (st == USBH_MSC_STOPPING) { /* 卸载中: 保持弹窗(后台正在改卡状态) */
            os_dialog_mark_handled(ctx);
        }
        /* USB: 关窗即可(卡仍归电脑, 需要时再从菜单进来卸载);
         * IDLE / FAULT: 交回框架正常出栈。两者都不 mark_handled。 */
        return;
    }

    switch (st) {
    case USBH_MSC_IDLE:
    case USBH_MSC_FAULT:
        if (!usbh_msc_request_start()) os_dialog_toast(ctx, "正在忙，请稍后");
        os_dialog_mark_handled(ctx);          /* 保持弹窗, poll 会改成"挂载中" */
        break;
    case USBH_MSC_PREPARING:                  /* 此时按钮是「取消」 */
        usbh_msc_request_cancel();
        os_dialog_mark_handled(ctx);
        break;
    case USBH_MSC_USB:                        /* 此时按钮是「卸载」 */
        usbh_msc_request_stop();
        os_dialog_mark_handled(ctx);
        break;
    default:                                  /* STOPPING / RESTORING: 忽略重复点击 */
        os_dialog_mark_handled(ctx);
        break;
    }
}

/* 挂载/卸载两个后台任务、三处超时、旧的状态消费逻辑已在 2026-11-25 全部删除:
 * 它们和 usbh_msc 的会话状态机重复, 是"弹窗不关 / 存储恢复中卡死"的根源。
 * 现在真实流程在 usbh_msc_sdspi.c 的 msc_do_start / msc_do_stop 里, 单线程、单所有者。 */

/* ── 旧的 unmount_worker_task (两阶段上报) 已删除 ──────────────────────────
 * 它把"USB 已退出"和"本地 VFS 已恢复"分成两次上报, 但两者仍然挤在同一个任务里,
 * 且状态靠两个 bool 传递; 现在由状态机直接表达:
 *   STOPPING(USB 还在退) → RESTORING(USB 已退, 本地恢复中) → IDLE/FAULT。
 * UI 只需在见到 RESTORING 时关窗 —— 与本地恢复耗时彻底解耦。 */

/* 旧的 mount_state_poll 已删除 —— 状态消费与超时兜底统一由 mount_dlg_poll +
 * usbh_msc 状态机负责, 界面不再"改状态来关弹窗"。 */

/* 旧的 mount_btn_cb 已删除 —— 按钮语义统一按"当前状态"决定 (见上面的 mount_btn_cb):
 * 每个状态下 items[0] 的含义固定, 重复点击在当前状态下自然被忽略, 不再需要
 * s_mount_cancel / s_unmounting 这类跨任务标志。 */

/* 旧的 sd_mount_confirm 已删除: 确认框与"挂载中/已挂载"是**同一份渲染**(mount_dlg_render),
 * 确认动作只是 usbh_msc_request_start(), 弹窗由 poll 随状态原地变形。 */

static void open_sd_mount(ui_ctx_t *ctx) {
    usbh_msc_status_t st = usbh_msc_status();

    /* 过渡态不弹新窗: 直接按**当前状态**给一句提示。
     * 这里没有"守卫"也没有超时 —— 状态机自己会离开这些态(RESTORING 最多
     * 8s+4s+1s 的有界等待后必进 IDLE 或 FAULT), 所以入口不可能被永久拒绝。 */
    if (st.state == USBH_MSC_STOPPING) {
        os_dialog_toast(ctx, "正在卸载");
        return;
    }
    if (st.state == USBH_MSC_RESTORING) {
        os_dialog_toast(ctx, "存储恢复中");
        return;
    }
    /* ★ FAULT: 上一次卸载后本机 VFS 没恢复。弹窗已在 RESTORING 时关掉(USB 侧卸载
     *   是既成事实, 不能为等 VFS 而吊住界面), 所以这里按状态补一句提示 ——
     *   不新增"待播报"标志, 状态机本身就是这则提示的唯一真相源。 */
    if (st.state == USBH_MSC_FAULT) {
        os_dialog_toast(ctx, "本机存储未恢复，可重试挂载");
    }
    /* IDLE / FAULT / PREPARING / USB: 都可以开这个弹窗 ——
     * PREPARING 打开就是"挂载中(可取消)", USB 打开就是"已挂载(可卸载)"。 */

    os_dlg_stack_t dlg;
    memset(&dlg, 0, sizeof(dlg));          /* 只清零**栈上副本**, 绝不 memset 活弹窗 */
    s_dlg_seq0 = st.seq;
    mount_dlg_render(&dlg, st);
    os_dialog_push(ctx, &dlg);
}

/* ==== 浏览文件 (融合弹窗) 状态与入口, 前置供 sd_main_key 使用 ==== */
#define FB_MAX 60
#define FB_BX 25      /* 弹窗左 */
#define FB_BY 25      /* 弹窗顶 */
#define FB_BW 350     /* 弹窗宽 (与存储菜单一致) */
#define FB_BH 250     /* 弹窗高 */
#define FB_RH 24      /* 行高 (16px 字体) */
/* ★ 2026-11-01 linit 改版：
 *   顶部路径栏横跨两栏 → 左侧快捷位置 100px → 右侧列表(16px 图标 + 名称 + 右对齐信息)
 *   → 底部左「确认」右「返回」。
 *   ⚠ 行区/行高改动必须同步 fb_render / fb_touch / fb_follow / fb_poll，四处同源。 */
#define FB_PATH_H 26                          /* 顶部路径栏高 */
#define FB_SIDE_W 100                         /* 左栏快捷位置宽 */
#define FB_ROW_TOP (FB_BY + FB_PATH_H + 2)    /* 列表顶 (路径栏之下) */
#define FB_ROW_BOT (FB_BY + FB_BH - 6)        /* 列表底 (无底部按钮栏，直接用到底) */

/* ★ 左栏入口：根目录 / 应用 / 游戏 / 音乐 / 返回（22px、居中、可点击）
 *   路径名取自设备实测：/sdcard 下有 apps、roms、media、fonts、books、Rec、data。 */
/* ★ 左栏入口：5 个目录项从上往下排，「返回」**固定在左栏最底部**（不跟列表走） */
#define FB_SIDE_DIRS 5                        /* 目录项个数 */
#define FB_SIDE_BACK 5                        /* 「返回」在数组里的下标（固定底部） */
static const char *const FB_SIDE_NAME[FB_SIDE_BACK + 1] = {
    "根目录", "应用", "游戏", "音乐", "书籍", "返回"
};
static const char *const FB_SIDE_PATH[FB_SIDE_BACK + 1] = {
    "", "apps", "roms", "media", "books", NULL
};
static int s_fb_side_cur = 0;                 /* 当前所在快捷位置 */

static EXT_RAM_BSS_ATTR char  s_fb_path[160] = "";          /* 相对根目录, ""= /sdcard */
static EXT_RAM_BSS_ATTR char  s_fb_names[FB_MAX][30];       /* 条目名 (不含分隔符) */
static EXT_RAM_BSS_ATTR bool  s_fb_isdir[FB_MAX];
static EXT_RAM_BSS_ATTR int   s_fb_count = 0;
static EXT_RAM_BSS_ATTR char  s_fb_delpath[180];            /* 待删除文件完整路径 */
static void open_fb_dlg(ui_ctx_t *ctx);

/* ===== NVS 备份 / 恢复 (整区备份到 TF) ===== */
#define NVS_BAK_DIR "/sdcard/backup"
static EXT_RAM_BSS_ATTR char s_bak_paths[8][140];
static EXT_RAM_BSS_ATTR char s_bak_pend[140];   /* 待覆盖/删除的备份路径 */
static void nvs_del_cb(ui_ctx_t*ctx,int result,void*ud);
static void nvs_restore_do(ui_ctx_t*ctx,int result,void*ud);
static void nvs_restore_list(ui_ctx_t*ctx);

static void nvs_backup_to_tf(ui_ctx_t *ctx){
    const esp_partition_t *p=esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, "nvs");
    if(!p){ os_dialog_toast(ctx,"\xe6\x97\xa0NVS\xe5\x88\x86\xe5\x8c\xba"); return; }
    if(!sd_is_mounted()){ os_dialog_toast(ctx,"TF\xe6\x9c\xaa\xe6\x8c\x82\xe8\xbd\xbd"); return; }
    mkdir(NVS_BAK_DIR,0755);
    uint8_t *buf=heap_caps_malloc(p->size, MALLOC_CAP_SPIRAM);
    if(!buf){ os_dialog_toast(ctx,"\xe5\x86\x85\xe5\xad\x98\xe4\xb8\x8d\xe8\xb6\xb3"); return; }
    char path[140]; time_t t=time(NULL);
    snprintf(path,sizeof(path),"%s/nvs_%ld.bin",NVS_BAK_DIR,(long)t);
    if(esp_partition_read(p,0,buf,p->size)==ESP_OK){
        FILE *f=fopen(path,"wb");
        if(f){ size_t w=fwrite(buf,1,p->size,f); fclose(f);
               os_dialog_toast(ctx, w==p->size?"\xe5\xb7\xb2\xe5\xa4\x87\xe4\xbb\xbd":"\xe5\x86\x99\xe5\x85\xa5\xe5\xa4\xb1\xe8\xb4\xa5"); }
        else os_dialog_toast(ctx,"\xe6\x89\x93\xe5\xbc\x80\xe6\x96\x87\xe4\xbb\xb6\xe5\xa4\xb1\xe8\xb4\xa5");
    } else os_dialog_toast(ctx,"\xe8\xaf\xbbNVS\xe5\xa4\xb1\xe8\xb4\xa5");
    heap_caps_free(buf);
}
static void nvs_restore_do(ui_ctx_t*ctx,int result,void*ud){
    (void)ud; if(result!=0) return;   /* 取消 → 不动 */
    const esp_partition_t *p=esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, "nvs");
    if(!p){ os_dialog_toast(ctx,"\xe6\x97\xa0NVS\xe5\x88\x86\xe5\x8c\xba"); return; }
    /* ★ 2026-11-22 修「截断的备份也能把 NVS 清空」: 原来不校验就 erase 整区再写,
     *   文件只有一半(备份中途断电/卡写失败/选错文件)时, 设置与密钥全部丢失且不可恢复。
     *   现在: 文件大小必须**严格等于**分区大小, 读写/擦除任一步失败都中止且不重启。 */
    struct stat st;
    if (stat(s_bak_pend,&st)!=0 || (size_t)st.st_size != p->size) {
        ESP_LOGW("STOR","备份大小不符: %ld != %u ⇒ 取消恢复",
                 (long)((stat(s_bak_pend,&st)==0)?st.st_size:-1), (unsigned)p->size);
        os_dialog_toast(ctx,"\xe5\xa4\x87\xe4\xbb\xbd\xe6\x96\x87\xe4\xbb\xb6\xe4\xb8\x8d\xe5\xae\x8c\xe6\x95\xb4,\xe5\xb7\xb2\xe5\x8f\x96\xe6\xb6\x88"); /* 备份文件不完整,已取消 */
        return;
    }
    FILE *f=fopen(s_bak_pend,"rb"); if(!f){ os_dialog_toast(ctx,"\xe6\x89\x93\xe5\xbc\x80\xe5\xa4\xb1\xe8\xb4\xa5"); return; }
    uint8_t *buf=heap_caps_malloc(p->size, MALLOC_CAP_SPIRAM); if(!buf){ fclose(f); os_dialog_toast(ctx,"\xe5\x86\x85\xe5\xad\x98\xe4\xb8\x8d\xe8\xb6\xb3"); return; }
    size_t rd=fread(buf,1,p->size,f); fclose(f);
    if (rd != p->size) { heap_caps_free(buf); os_dialog_toast(ctx,"\xe8\xaf\xbb\xe5\xa4\x87\xe4\xbb\xbd\xe5\xa4\xb1\xe8\xb4\xa5"); return; }
    if (esp_partition_erase_range(p,0,p->size)!=ESP_OK) {
        heap_caps_free(buf); os_dialog_toast(ctx,"\xe6\x93\xa6\xe9\x99\xa4NVS\xe5\xa4\xb1\xe8\xb4\xa5"); return; }  /* 擦除NVS失败 */
    if (esp_partition_write(p,0,buf,p->size)!=ESP_OK) {
        heap_caps_free(buf); os_dialog_toast(ctx,"\xe5\x86\x99\xe5\x85\xa5NVS\xe5\xa4\xb1\xe8\xb4\xa5"); return; } /* 写入NVS失败 */
    heap_caps_free(buf);
    os_dialog_toast_ms(ctx,"\xe5\xb7\xb2\xe6\x81\xa2\xe5\xa4\x8d,\xe9\x87\x8d\xe5\x90\xaf\xe7\x94\x9f\xe6\x95\x88",1500); /* 已恢复,重启生效 */
    esp_restart();
}
static void nvs_restore_confirm2(ui_ctx_t*ctx,int result,void*ud);
static void nvs_restore_sel(ui_ctx_t*ctx,int result,void*ud){
    (void)ud; if(result<0) return;
    snprintf(s_bak_pend,sizeof(s_bak_pend),"%s",s_bak_paths[result]);
    os_dialog_confirm_ex(ctx,"\xe7\xa1\xae\xe5\xae\x9a\xe6\x81\xa2\xe5\xa4\x8d\xe8\xaf\xa5\xe5\xa4\x87\xe4\xbb\xbd?",0,-1,nvs_restore_confirm2,NULL); /* ①确认还原该备份? */
}
static void nvs_restore_confirm2(ui_ctx_t*ctx,int result,void*ud){
    (void)ud; if(result!=0) return;
    os_dialog_confirm_ex(ctx,"\xe6\x9c\x80\xe5\x90\x8e\xe7\xa1\xae\xe8\xae\xa4\xe8\xa6\x86\xe7\x9b\x96NVS\xe5\xb9\xb6\xe9\x87\x8d\xe5\x90\xaf?",0,-1,nvs_restore_do,NULL); /* ②最后确认覆盖NVS并重启? */
}
static void nvs_del_cb(ui_ctx_t*ctx,int result,void*ud){
    (void)ud; if(result!=0) return;
    remove(s_bak_pend);
    os_dialog_toast(ctx,"\xe5\xb7\xb2\xe5\x88\xa0\xe9\x99\xa4");  /* 已删除 */
    os_dialog_pop(ctx);
    nvs_restore_list(ctx);   /* 刷新列表 */
}
static bool nvs_restore_key(ui_ctx_t*ctx,os_dlg_stack_t*d,os_action_t a,void*ud){
    (void)ud;
    if(a==OS_ACTION_LONG_PRESS){   /* 长按备份项 → 确认删除 */
        if(d->sel<0 || d->sel>=d->count) return false;
        snprintf(s_bak_pend,sizeof(s_bak_pend),"%s",s_bak_paths[d->sel]);
        os_dialog_confirm_ex(ctx,"\xe5\x88\xa0\xe9\x99\xa4\xe8\xaf\xa5\xe5\xa4\x87\xe4\xbb\xbd?",0,-1,nvs_del_cb,NULL); /* 删除该备份? */
        return true;
    }
    return false;
}
static void nvs_restore_list(ui_ctx_t*ctx){
    if(!sd_is_mounted()){ os_dialog_toast(ctx,"TF\xe6\x9c\xaa\xe6\x8c\x82\xe8\xbd\xbd"); return; }
    os_dlg_stack_t dlg; memset(&dlg,0,sizeof(dlg));
    DIR *d=opendir(NVS_BAK_DIR); int n=0;
    if(d){ struct dirent *e;
        while((e=readdir(d)) && n<8){
            if(!strstr(e->d_name,".bin")) continue;
            /* 放不下就跳过: 不用被截断的备份路径 */
            if (strlen(NVS_BAK_DIR) + 1 + strlen(e->d_name) >= sizeof(s_bak_paths[0])) continue;
            snprintf(s_bak_paths[n],sizeof(s_bak_paths[0]),"%.*s/%.*s",
                     (int)sizeof(s_bak_paths[0])/2-1,NVS_BAK_DIR,(int)sizeof(s_bak_paths[0])/2-1,e->d_name);
                snprintf(dlg.items[n],sizeof(dlg.items[0]),"%.*s",(int)sizeof(dlg.items[0])-1,e->d_name); n++;
        }
        closedir(d);
    }
    if(!n){ os_dialog_toast(ctx,"\xe6\x97\xa0\xe5\xa4\x87\xe4\xbb\xbd\xe6\x96\x87\xe4\xbb\xb6"); return; }
    dlg.count=n; dlg.sel=0; dlg.cb=nvs_restore_sel; dlg.on_key=nvs_restore_key;
    os_dialog_push(ctx,&dlg);
}

static void bak_restore_cb(ui_ctx_t*ctx,int result,void*ud){
    (void)ud;
    if(result==0) nvs_backup_to_tf(ctx);       /* ① 备份NVS到TF */
    else if(result==1) nvs_restore_list(ctx);  /* ② 还原备份文件 */
}
/* 备份还原 二级菜单 */
static void open_sd_bak_restore(ui_ctx_t*ctx){
    static const char *ptrs[2]={ "\xe5\xa4\x87\xe4\xbb\xbdNVS\xe5\x88\xb0TF",   /* 备份NVS到TF */
                                 "\xe8\xbf\x98\xe5\x8e\x9f\xe5\xa4\x87\xe4\xbb\xbd\xe6\x96\x87\xe4\xbb\xb6" }; /* 还原备份文件 */
    os_dialog_list(ctx,"",ptrs,2,0,bak_restore_cb,NULL);
}

static bool sd_main_key(ui_ctx_t *ctx, os_dlg_stack_t *d, os_action_t a, void *ud) {
    (void)ud;
    if (a != OS_ACTION_CONFIRM) return false;   /* UP/DOWN/BACK 走默认 */
    if (d->sel >= d->count) return false;       /* 底部返回行 → 默认关闭 */
    switch (d->sel) {
    case 0:  /* 浏览文件 → 融合进同一弹窗: 快扫/目录进退/长按删除 */
        s_fb_path[0] = '\0';   /* 每次进入从根开始 */
        open_fb_dlg(ctx);
        break;
    case 1:  /* 存储信息 (★ 「格式化TF卡」在这个页面的顶部) */
        open_sd_info(ctx);
        break;
    case 2:  /* 备份还原 */
        open_sd_bak_restore(ctx);
        break;
    case 3:  /* USB访问存储 (原「挂载到电脑 → USB 数据线」, 2026-11-01 提到主菜单) */
        open_sd_mount(ctx);
        break;
    case 4:  /* 网络访问存储 (原「挂载到电脑 → 网络访问存储」, 同上) */
        open_sd_share(ctx);
        break;
    default:
        break;
    }
    return true;   /* 不关闭主弹窗, 子弹窗压栈 */
}

/* ==== 浏览文件 (融合进同一弹窗): 快扫 / 进目录 / 长按或点×删除 ==== */
static bool fb_at_root(void) { return s_fb_path[0] == '\0'; }
static const char *fb_dir_path(void) {
    if (fb_at_root()) return "/sdcard";
    static EXT_RAM_BSS_ATTR char buf[160];
    snprintf(buf, sizeof(buf), "/sdcard/%.*s", (int)sizeof(buf) - 9, s_fb_path);
    return buf;
}
static void fb_fullpath(char *out, int cap, const char *name) {
    snprintf(out, cap, "%s/%s", fb_dir_path(), name);
}
/* 标题截断: title[32] 很小, 按 UTF-8 边界截短当前路径放标题栏 */
/* 文件类型 (用于行首 【xxx】 标签) */
typedef enum { FBT_DIR, FBT_IMG, FBT_MP3, FBT_VID, FBT_GAME, FBT_TXT, FBT_OTHER } fb_type_t;
static EXT_RAM_BSS_ATTR fb_type_t s_fb_type[FB_MAX];
static EXT_RAM_BSS_ATTR uint32_t  s_fb_size[FB_MAX];   /* ★ 文件字节数(目录=0)，右侧信息列用 */
static EXT_RAM_BSS_ATTR int  s_fb_sel = 0;
static EXT_RAM_BSS_ATTR int  s_fb_off = 0;         /* 滚动像素偏移 (跟手拖动) */
static EXT_RAM_BSS_ATTR int  s_fb_drag = -1, s_fb_drag_off0 = 0;

/* 隐藏文件 / Windows 系统垃圾 (不显示) */
static bool fb_junk(const char *n) {
    if (n[0] == '.') return true;                       /* 点文件含 .DS_Store/.Trashes/._* 等 */
    int l = (int)strlen(n);
    if (l > 4 && !strcasecmp(n + l - 4, ".tmp")) return true;
    if (l > 5 && !strcasecmp(n + l - 5, ".temp")) return true;
    if (n[0] == '~' && n[1] == '$') return true;        /* office/word 临时文件 */
    if (!strcasecmp(n, "Thumbs.db")) return true;
    if (!strcasecmp(n, "desktop.ini")) return true;
    if (!strcasecmp(n, "System Volume Information")) return true;
    if (!strcasecmp(n, "$RECYCLE.BIN")) return true;
    if (!strcasecmp(n, "RECYCLER")) return true;
    return false;
}
static fb_type_t fb_type_of(const char *n, bool dir) {
    if (dir) return FBT_DIR;
    const char *e = strrchr(n, '.');
    if (!e || !e[1]) return FBT_OTHER;
    e++;
    if (!strcasecmp(e,"jpg")||!strcasecmp(e,"jpeg")||!strcasecmp(e,"png")||
        !strcasecmp(e,"bmp")||!strcasecmp(e,"gif")||!strcasecmp(e,"webp")) return FBT_IMG;
    if (!strcasecmp(e,"mp3")||!strcasecmp(e,"wav")||!strcasecmp(e,"ogg")||
        !strcasecmp(e,"flac")||!strcasecmp(e,"aac")||!strcasecmp(e,"m4a")) return FBT_MP3;
    if (!strcasecmp(e,"mp4")||!strcasecmp(e,"avi")||!strcasecmp(e,"mkv")||
        !strcasecmp(e,"mov")||!strcasecmp(e,"flv")) return FBT_VID;
    if (!strcasecmp(e,"gb")||!strcasecmp(e,"gbc")||!strcasecmp(e,"nes")||
        !strcasecmp(e,"fds")||!strcasecmp(e,"swc")||!strcasecmp(e,"ab")||
        !strcasecmp(e,"lav")) return FBT_GAME;
    if (!strcasecmp(e,"txt")||!strcasecmp(e,"md")||!strcasecmp(e,"log")||
        !strcasecmp(e,"ini")||!strcasecmp(e,"csv")) return FBT_TXT;
    return FBT_OTHER;
}
/* 快扫 + 隐藏 + 类型: 用 d_type 判断目录 (FAT 通常直接给), 未识别才 stat */
static void fb_scan(void) {
    /* ★ 2026-10-24: 挂载是异步的, 早进来 opendir 会拿到空列表(linit: "文件全空")。
     *   过门闸: 已挂载立刻返回; 还在挂载就等它收尾, 没了就补挂一次。 */
    sd_wait_ready(1200, true);
    s_fb_count = 0;
    DIR *d = opendir(fb_dir_path());
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && s_fb_count < FB_MAX) {
        if (fb_junk(e->d_name)) continue;
        bool isdir = (e->d_type == DT_DIR);
        uint32_t fsz = 0;
        {   /* 统一 stat 一次：既拿准确类型，也拿文件大小（右侧信息列要用） */
            char p[180];
            fb_fullpath(p, sizeof(p), e->d_name);
            struct stat st;
            if (stat(p, &st) == 0) {
                isdir = S_ISDIR(st.st_mode);
                if (!isdir) fsz = (uint32_t)st.st_size;
            }
        }
        memcpy(s_fb_names[s_fb_count], e->d_name, 29);
        s_fb_names[s_fb_count][29] = 0;
        s_fb_isdir[s_fb_count] = isdir;
        s_fb_size[s_fb_count]  = fsz;
        s_fb_type[s_fb_count] = fb_type_of(e->d_name, isdir);
        s_fb_count++;
    }
    closedir(d);
    /* 目录在前, 各自字母序 (同步类型) */
    for (int i = 1; i < s_fb_count; i++)
        for (int j = i; j > 0; j--) {
            int c = (s_fb_isdir[j - 1] == s_fb_isdir[j])
                        ? strcmp(s_fb_names[j], s_fb_names[j - 1])
                        : (s_fb_isdir[j - 1] ? -1 : 1);
            if (c < 0) {
                char tn[30]; bool tf; fb_type_t tt; uint32_t tsz;
                strcpy(tn, s_fb_names[j - 1]); tf = s_fb_isdir[j - 1]; tt = s_fb_type[j - 1]; tsz = s_fb_size[j - 1];
                strcpy(s_fb_names[j - 1], s_fb_names[j]); s_fb_isdir[j - 1] = s_fb_isdir[j]; s_fb_type[j - 1] = s_fb_type[j]; s_fb_size[j - 1] = s_fb_size[j];
                strcpy(s_fb_names[j], tn); s_fb_isdir[j] = tf; s_fb_type[j] = tt; s_fb_size[j] = tsz;
            } else break;
        }
}
static void fb_enter_dir(int ei) {
    if (fb_at_root()) strncpy(s_fb_path, s_fb_names[ei], sizeof(s_fb_path) - 1);
    else if (strlen(s_fb_path) + 1 + strlen(s_fb_names[ei]) < sizeof(s_fb_path))
        snprintf(s_fb_path + strlen(s_fb_path), sizeof(s_fb_path) - strlen(s_fb_path),
                 "/%s", s_fb_names[ei]);
}
static void fb_go_up(void) {
    char *sl = strrchr(s_fb_path, '/');
    if (sl) *sl = '\0';
    else s_fb_path[0] = '\0';
}
/* 左栏点击：目录项跳转；「返回」项回到上级（根目录则退出） */
static void fb_side_goto(ui_ctx_t *ctx, int i) {
    if (i < 0 || i > FB_SIDE_BACK) return;
    if (i == FB_SIDE_BACK) {
        if (fb_at_root()) { os_dialog_pop(ctx); return; }
        fb_go_up();
        s_fb_side_cur = 0;
        s_fb_sel = 0; s_fb_off = 0;
        fb_scan();
        return;
    }
    const char *p = FB_SIDE_PATH[i];
    if (!p) return;
    if (p[0]) {
        char full[180];
        snprintf(full, sizeof(full), "/sdcard/%s", p);
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode)) return;
    }
    snprintf(s_fb_path, sizeof(s_fb_path), "%s", p);
    s_fb_side_cur = i;
    s_fb_sel = 0;
    s_fb_off = 0;
    fb_scan();
}
static void fb_delete_cb(ui_ctx_t *ctx, int result, void *ud) {
    (void)ud;
    if (result != 0) return;   /* 取消: 只关删除确认, 回浏览弹窗 */
    if (s_fb_delpath[0]) remove(s_fb_delpath);
    os_dialog_pop(ctx);        /* 关删除确认 */
    os_dialog_pop(ctx);        /* 关当前浏览层 */
    open_fb_dlg(ctx);
    os_dialog_toast(ctx, "\xe5\xb7\xb2\xe5\x88\xa0\xe9\x99\xa4"); /* 已删除 */
}
/* 让选中行跟随滚动: 框在可视窗内移动, 越界才滚动 */
static void fb_follow(void) {
    const int rtop = FB_ROW_TOP, rh = FB_RH, rbot = FB_ROW_BOT;
    int maxvis = (rbot - rtop) / rh; if (maxvis < 1) maxvis = 1;
    int first = s_fb_off / rh;
    if (s_fb_sel < first) first = s_fb_sel;
    else if (s_fb_sel >= first + maxvis) first = s_fb_sel - maxvis + 1;
    if (first < 0) first = 0;
    s_fb_off = first * rh;
    int maxoff = s_fb_count * rh - (rbot - rtop); if (maxoff < 0) maxoff = 0;
    if (s_fb_off > maxoff) s_fb_off = maxoff;
}

/* 按 16px 实际像素宽截断 —— 路径/文件名可能很长，必须保证不越过右边框 */
static const char *fb_fit16(const char *s, int maxw)
{
    static char buf[64];
    snprintf(buf, sizeof(buf), "%s", s);
    int len = (int)strlen(buf);
    while (len > 0 && ui_text16w(buf) > maxw) buf[--len] = 0;
    return buf;
}

/* 16px 类型图标（12×14 轮廓 + 内部标识）—— 全部用绘图原语，不占额外内存 */
static void fb_draw_icon(st7305_handle_t *lcd, int x, int y, fb_type_t t, bool inv)
{
    uint8_t c = inv ? ST7305_COLOR_WHITE : ST7305_COLOR_BLACK;
    draw_hline(lcd, x, x + 11, y, c);
    draw_hline(lcd, x, x + 11, y + 13, c);
    draw_vline(lcd, x, y, y + 13, c);
    draw_vline(lcd, x + 11, y, y + 13, c);
    switch (t) {
    case FBT_DIR:                                   /* 文件夹：顶部折角 */
        draw_hline(lcd, x + 1, x + 5, y + 1, c);
        draw_vline(lcd, x + 5, y + 1, y + 3, c);
        break;
    case FBT_GAME:                                  /* 游戏：实心块 */
        fill_rect(lcd, x + 3, y + 5, x + 8, y + 9, c);
        break;
    case FBT_TXT:
    case FBT_OTHER:                                 /* 文本：三条横线 */
        draw_hline(lcd, x + 3, x + 8, y + 4, c);
        draw_hline(lcd, x + 3, x + 8, y + 7, c);
        draw_hline(lcd, x + 3, x + 8, y + 10, c);
        break;
    default:                                        /* 图片/音频/视频：方框 */
        draw_hline(lcd, x + 3, x + 8, y + 4, c);
        draw_vline(lcd, x + 3, y + 4, y + 9, c);
        draw_vline(lcd, x + 8, y + 4, y + 9, c);
        draw_hline(lcd, x + 3, x + 8, y + 9, c);
        break;
    }
}

/* 自定义整层渲染：顶部路径栏(横跨) + 左栏快捷位置 + 右栏列表(图标/名称/右对齐信息) + 底部按钮 */
static bool fb_render(ui_ctx_t *ctx, os_dlg_stack_t *d, void *ud) {
    (void)d; (void)ud;
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return true;
    int bx = FB_BX, by = FB_BY, bw = FB_BW, bh = FB_BH;
    fill_rect(lcd, bx, by, bx + bw - 1, by + bh - 1, ST7305_COLOR_WHITE);
    for (int k = 0; k < 2; k++) {
        draw_hline(lcd, bx + k, bx + bw - 1 - k, by + k, ST7305_COLOR_BLACK);
        draw_hline(lcd, bx + k, bx + bw - 1 - k, by + bh - 1 - k, ST7305_COLOR_BLACK);
        draw_vline(lcd, bx + k, by + k, by + bh - 1 - k, ST7305_COLOR_BLACK);
        draw_vline(lcd, bx + bw - 1 - k, by + k, by + bh - 1 - k, ST7305_COLOR_BLACK);
    }

    /* ---- ① 顶部路径栏：横跨两栏，右端为「返回上级」图标 ---- */
    /* 路径显示：根目录 "< /sdcard"，子目录 "< /sdcard/xxx"（★ 别漏斜杠，否则会拼成 /sdcardapps）
     * 注意 s_fb_path 存的是**相对**路径（不含开头的 /），显示时要自己补。 */
    char pathbuf[48];
    const char *rp = s_fb_path[0] ? s_fb_path : "";
    if (rp[0]) snprintf(pathbuf, sizeof(pathbuf), "< /sdcard/%s", rp);
    else       snprintf(pathbuf, sizeof(pathbuf), "< /sdcard");
    ui_text16(lcd, bx + 6, by + 5, fb_fit16(pathbuf, bw - 12 - 34), false);   /* 留出右侧图标宽度，绝不越界 */
    /* 右端返回图标：方框 + 左箭头（纯几何绘制，不依赖字形） */
    {
        int ix = bx + bw - 24, iy = by + 6;
        draw_hline(lcd, ix, ix + 15, iy, ST7305_COLOR_BLACK);
        draw_hline(lcd, ix, ix + 15, iy + 13, ST7305_COLOR_BLACK);
        draw_vline(lcd, ix, iy, iy + 13, ST7305_COLOR_BLACK);
        draw_vline(lcd, ix + 15, iy, iy + 13, ST7305_COLOR_BLACK);
        draw_hline(lcd, ix + 4, ix + 11, iy + 6, ST7305_COLOR_BLACK);
        draw_hline(lcd, ix + 4, ix + 6, iy + 4, ST7305_COLOR_BLACK);
        draw_hline(lcd, ix + 4, ix + 6, iy + 8, ST7305_COLOR_BLACK);
    }
    draw_hline(lcd, bx + 2, bx + bw - 3, by + FB_PATH_H - 2, ST7305_COLOR_BLACK);

    /* ---- ② 左栏：目录项(22px 居中) + 底部固定「返回」 ---- */
    int sy = by + FB_PATH_H + 8;
    for (int i = 0; i < FB_SIDE_DIRS; i++) {
        bool cur = (i == s_fb_side_cur);
        if (cur) fill_rect(lcd, bx + 3, sy - 3, bx + FB_SIDE_W - 4, sy + 21, ST7305_COLOR_BLACK);
        draw_text_centered_in(lcd, bx, bx + FB_SIDE_W, sy, FB_SIDE_NAME[i], cur);
        sy += 30;                    /* ★ 项间距：22px 字 + 8px 缝隙，不要挤在一起 */
    }
    /* 「返回」固定在左栏最底部，带一条分割线与上面目录项区分 */
    draw_hline(lcd, bx + 4, bx + FB_SIDE_W - 5, by + bh - 40, ST7305_COLOR_BLACK);
    draw_text_centered_in(lcd, bx, bx + FB_SIDE_W, by + bh - 34, FB_SIDE_NAME[FB_SIDE_BACK], false);
    draw_vline(lcd, bx + FB_SIDE_W, by + FB_PATH_H, by + bh - 4, ST7305_COLOR_BLACK);

    /* ---- ③ 右栏：文件列表 ---- */
    const int lx = bx + FB_SIDE_W + 5;       /* 图标左 */
    const int tx = lx + 18;                  /* 名称左 */
    const int rx = bx + bw - 8;              /* 右侧信息右边界 */
    int rtop = FB_ROW_TOP, rbot = FB_ROW_BOT, rh = FB_RH;
    int view = rbot - rtop;
    int off = s_fb_off; if (off < 0) off = 0;
    int first = off / rh, mod = off % rh;
    int maxvis = view / rh; if (maxvis < 1) maxvis = 1;

    if (s_fb_count == 0) {
        ui_text16(lcd, tx, rtop + 4, "空目录", false);
    }
    for (int i = 0; i <= maxvis; i++) {
        int idx = first + i;
        if (idx >= s_fb_count) break;
        int y = rtop + i * rh - mod;
        if (y < rtop) continue;
        /* ★ 2026-11-01 修复"拖动时底部越界变黑"：
         *   滚动时最后一行是半行，原来只判了上边界，黑条/图标会画到 FB_ROW_BOT 以下
         *   （弹窗底 275 → 实际画到 291），越出边框的部分留在屏幕上越积越黑。
         *   这里按列表下边界裁剪行高，并且整行完全在界外就不再画。 */
        if (y >= rbot) break;
        int rh_draw = (y + rh - 2 > rbot) ? (rbot - y) : (rh - 2);
        if (rh_draw < 4) break;
        /* ★ 只有**完整行**才画图标和文字：半行只画黑条。
         *   否则文字垂直居中后会超出 rbot（16px 字放在 y+4，若 y 已接近底边 ⇒ 直接出框）。 */
        bool full_row = (y + rh <= rbot);
        /* ★ 2026-11-22: 触屏方案(有屏且没连手柄)不画默认选中黑条 —— 一进文件列表就整行反白
         *   会被误读成"已经选中了这一项"。判据统一用 input_scheme_touch()。 */
        bool sel = (!input_scheme_touch() && idx == s_fb_sel);
        if (sel) fill_rect(lcd, lx - 3, y, rx + 2, y + rh_draw, ST7305_COLOR_BLACK);
        if (!full_row) continue;

        fb_draw_icon(lcd, lx, y + (rh - 14) / 2, s_fb_type[idx], sel);

        /* 右侧信息：目录显示"目录"，文件显示大小 */
        char info[16];
        if (s_fb_isdir[idx]) snprintf(info, sizeof(info), "目录");
        else {
            uint32_t s = s_fb_size[idx];
            if (s < 1024)            snprintf(info, sizeof(info), "%uB", (unsigned)s);
            else if (s < 1024u * 1024u) snprintf(info, sizeof(info), "%uK", (unsigned)(s / 1024u));
            else                     snprintf(info, sizeof(info), "%u.%uM",
                                              (unsigned)(s / (1024u * 1024u)),
                                              (unsigned)((s % (1024u * 1024u)) / 102400u));
        }
        int iw = ui_text16w(info);
        ui_text16(lcd, rx - iw, y + (rh - 16) / 2, info, sel);

        /* 名称：按 16px 实际像素宽截断，保证不压到右侧信息列 */
        int maxw = (rx - iw - 6) - tx;
        if (maxw < 16) maxw = 16;
        ui_text16(lcd, tx, y + (rh - 16) / 2, fb_fit16(s_fb_names[idx], maxw), sel);
    }

    /* 滚动条 */
    if (s_fb_count > maxvis) {
        int bxr = bx + bw - 4;
        int bar_y0 = rtop, bar_y1 = rtop + maxvis * rh - 1;
        draw_vline(lcd, bxr, bar_y0, bar_y1, ST7305_COLOR_BLACK);
        int track = bar_y1 - bar_y0;
        int th = track * maxvis / s_fb_count; if (th < 4) th = 4;
        int maxoff = s_fb_count * rh - (rbot - rtop); if (maxoff < 0) maxoff = 0;
        int py = maxoff > 0 ? (int)((long)s_fb_off * (track - th) / maxoff) : 0;
        if (py < 0) py = 0;
        fill_rect(lcd, bxr, bar_y0 + py, bxr, bar_y0 + py + th - 1, ST7305_COLOR_BLACK);
    }

    /* ---- ④ 底部：已移除按钮栏，列表直到底边 ---- */
    return true;
}
/* 按键: 上下移动 / 确认进目录 / 长按删除 / 返回上级 */
static bool fb_key(ui_ctx_t *ctx, os_dlg_stack_t *d, os_action_t a, void *ud) {
    (void)d; (void)ud;
    if (a == OS_ACTION_UP) {
        if (s_fb_sel > 0) { s_fb_sel--; fb_follow(); }
        ctx->needs_redraw = true; return true;
    }
    if (a == OS_ACTION_DOWN) {
        if (s_fb_sel + 1 < s_fb_count) { s_fb_sel++; fb_follow(); }
        ctx->needs_redraw = true; return true;
    }
    if (a == OS_ACTION_CONFIRM) {
        int ei = s_fb_sel;
        if (ei >= 0 && ei < s_fb_count && s_fb_isdir[ei]) {
            fb_enter_dir(ei);
            os_dialog_pop(ctx); open_fb_dlg(ctx);
        }
        return true;   /* 文件项不打开 */
    }
    if (a == OS_ACTION_LONG_PRESS) {    /* 长按确认键 → 弹删除确认 */
        int ei = s_fb_sel;
        if (ei >= 0 && ei < s_fb_count && !s_fb_isdir[ei]) {
            fb_fullpath(s_fb_delpath, sizeof(s_fb_delpath), s_fb_names[ei]);
            os_dialog_confirm_ex(ctx,
                "\xe5\x88\xa0\xe9\x99\xa4\xe8\xbf\x99\xe4\xb8\xaa\xe6\x96\x87\xe4\xbb\xb6?", /* 删除这个文件? */
                0, 0, fb_delete_cb, NULL);
        }
        return true;
    }
    if (a == OS_ACTION_BACK) {          /* 返回 = 上级 (根=退出) */
        if (fb_at_root()) { os_dialog_pop(ctx); return true; }
        fb_go_up();
        os_dialog_pop(ctx); open_fb_dlg(ctx);
        return true;
    }
    return false;
}
/* 触摸: 点行→选中(文件夹即进入), 点底部行→返回上级 */
static bool fb_touch(ui_ctx_t *ctx, os_dlg_stack_t *d, int x, int y, void *ud) {
    (void)d; (void)ud;
    int bx = FB_BX, by = FB_BY, bw = FB_BW, bh = FB_BH;
    if (x < bx || x > bx + bw - 1 || y < by || y > by + bh - 1) return false;
    /* 顶部栏：右端返回图标 = 返回上级 / 根目录退出 */
    if (y < by + FB_PATH_H) {
        if (x >= bx + bw - 32) {
            if (fb_at_root()) os_dialog_pop(ctx);
            else { fb_go_up(); s_fb_side_cur = 0; os_dialog_pop(ctx); open_fb_dlg(ctx); }
        }
        return true;
    }
    /* 左栏：底部「返回」固定区，其余为目录项 */
    if (x < bx + FB_SIDE_W) {
        if (y >= by + bh - 44) {          /* 底部固定「返回」 */
            fb_side_goto(ctx, FB_SIDE_BACK);
            ctx->needs_redraw = true;
            return true;
        }
        int sy = by + FB_PATH_H + 8;
        for (int i = 0; i < FB_SIDE_DIRS; i++) {
            if (y >= sy - 3 && y <= sy + 21) {
                fb_side_goto(ctx, i);
                ctx->needs_redraw = true;
                return true;
            }
            sy += 30;
        }
        return true;
    }
    int idx = (s_fb_off + (y - FB_ROW_TOP)) / FB_RH;
    if (idx < 0 || idx >= s_fb_count) return true;
    s_fb_sel = idx;
    ctx->needs_redraw = true;
    if (s_fb_isdir[idx]) { fb_enter_dir(idx); os_dialog_pop(ctx); open_fb_dlg(ctx); }
    return true;
}
/* 每帧: 触屏拖动滚动 (跟手) */
static void fb_poll(ui_ctx_t *ctx, os_dlg_stack_t *d, void *ud) {
    (void)d; (void)ud;
    int tx, ty;
    bool down = input_get_touch_pos(&tx, &ty);
    int rtop = FB_ROW_TOP, rbot = FB_ROW_BOT;
    int maxoff = s_fb_count * FB_RH - (rbot - rtop); if (maxoff < 0) maxoff = 0;
    if (down) {
        if (tx < FB_BX || ty < rtop || ty >= rbot) { if (s_fb_drag == -1) s_fb_drag = -999; return; }
        if (s_fb_drag == -1) { s_fb_drag = ty; s_fb_drag_off0 = s_fb_off; return; }
        if (s_fb_drag == -999) return;
        int no = s_fb_drag_off0 - (ty - s_fb_drag);
        if (no < 0) no = 0;
        if (no > maxoff) no = maxoff;
        if (no != s_fb_off) { s_fb_off = no; if (ctx) ctx->needs_redraw = true; }
    } else {
        s_fb_drag = -1;
    }
}
static void open_fb_dlg(ui_ctx_t *ctx) {
    fb_scan();
    if (s_fb_sel >= s_fb_count) s_fb_sel = 0;
    s_fb_off = 0;
    os_dlg_stack_t dlg;
    memset(&dlg, 0, sizeof(dlg));
    dlg.count = 0;               /* 自定义整层渲染, 全自管理 */
    dlg.on_render = fb_render;
    dlg.on_key = fb_key;
    dlg.on_touch = fb_touch;
    dlg.on_poll = fb_poll;
    dlg.no_footer = true;
    os_dialog_push(ctx, &dlg);
}

/* ==== 局域网共享 (第三回合): 浏览器访问 http://<设备IP>/files/ 管理 TF 卡 ====
 * 服务本体在 services/web_files（走统一 web_core，不自己起 HTTP Server）。
 * 这里只做"开关 + 告诉用户去哪儿访问"，不复制任何网络代码。
 * ★ 2026-11-01 linit: 交互改成和「USB访问存储」同款 —— 先确认框，确定后同框换成
 *   **浏览器图标** + 「关闭共享/取消」，不再是一进来就一个纯文字列表。 */

/* 图标窗按钮: 0=关闭共享, 1=取消(走默认 pop, 共享继续运行) */
static void wf_icon_btn_cb(ui_ctx_t *ctx, int result, void *ud) {
    (void)ud;
    if (result != 0) return;
    web_files_stop();
    if (!wifi_manager_ap_is_on()) os_ap_set(false);
    os_dialog_pop(ctx);
    os_dialog_toast(ctx, "已关闭网络访问存储");
}

/* 把栈顶弹窗原地改写成「浏览器图标 + 关闭共享/取消」(不关不弹栈) */
static void wf_morph_to_icon(ui_ctx_t *ctx, const char *ip) {
    os_dlg_stack_t *d = os_dialog_top_mut(ctx);
    if (!d) return;
    os_dialog_mark_handled(ctx);          /* 保持同一弹窗, 不自动关闭 */
    d->title[0] = '\0';
    d->count = 2;
    snprintf(d->items[0], sizeof(d->items[0]), "\xe5\x85\xb3\xe9\x97\xad\xe5\x85\xb1\xe4\xba\xab"); /* 关闭共享 */
    snprintf(d->items[1], sizeof(d->items[1]), "\xe5\x8f\x96\xe6\xb6\x88");                        /* 取消 */
    d->sel = 0;
    d->icon   = web_icon_64;
    d->icon_w = WEB_ICON_W;
    d->icon_h = WEB_ICON_H;
    /* ★ 2026-11-08 linit: IP 直接印在图标下方, 不再另弹 toast 打扰 */
    d->note[0] = '\0';
    if (ip && ip[0] && strcmp(ip, "-") != 0)
        snprintf(d->note, sizeof(d->note), "IP\xef\xbc\x9a%s", ip);   /* IP：x.x.x.x */
    d->cb     = wf_icon_btn_cb;
}

static void wf_start_cb(ui_ctx_t *ctx, int result, void *ud) {
    (void)ud;
    if (result != 0) return;              /* 取消 */
    if (!wifi_manager_is_connected()) {   /* ★ 局域网共享要求先连上 WiFi, 不开热点 */
        os_dialog_pop(ctx);
        os_dialog_toast(ctx, "请先连接 WiFi");
        return;
    }
    if (web_files_start() != ESP_OK) {
        os_dialog_pop(ctx);
        os_dialog_toast(ctx, "网络访问存储启动失败");
        return;
    }
    os_ap_set(true);
    char ip[20] = "-";
    web_files_get_addr(ip, sizeof(ip));
    wf_morph_to_icon(ctx, ip);            /* 同框变图标窗: 图标 + IP + 按钮 */
}

static void open_sd_share(ui_ctx_t *ctx) {
    os_dlg_stack_t dlg;
    memset(&dlg, 0, sizeof(dlg));
    dlg.count = 2;
    dlg.sel = 0;
    dlg.no_footer = true;
    dlg.small = true;

    if (!web_files_is_running()) {
        /* 与「USB访问存储」同款确认框: 确定后同框换图标 */
        snprintf(dlg.title, sizeof(dlg.title), "\xe5\xbc\x80\xe5\x90\xaf\xe7\xbd\x91\xe7\xbb\x9c\xe8\xae\xbf\xe9\x97\xae\xe5\xad\x98\xe5\x82\xa8?"); /* 开启网络访问存储? */
        snprintf(dlg.items[0], sizeof(dlg.items[0]), "\xe7\xa1\xae\xe5\xae\x9a");   /* 确定 */
        snprintf(dlg.items[1], sizeof(dlg.items[1]), "\xe5\x8f\x96\xe6\xb6\x88");   /* 取消 */
        dlg.fix_w = text_width(dlg.title) + 24;
        dlg.cb = wf_start_cb;
        os_dialog_push(ctx, &dlg);
        return;
    }
    /* 已在运行: 直接给图标窗, 点「关闭共享」才停 */
    dlg.title[0] = '\0';
    snprintf(dlg.items[0], sizeof(dlg.items[0]), "\xe5\x85\xb3\xe9\x97\xad\xe5\x85\xb1\xe4\xba\xab");
    snprintf(dlg.items[1], sizeof(dlg.items[1]), "\xe5\x8f\x96\xe6\xb6\x88");
    dlg.icon   = web_icon_64;
    dlg.icon_w = WEB_ICON_W;
    dlg.icon_h = WEB_ICON_H;
    {   /* ★ 2026-11-08: 重开时同样在图标下显示访问 IP */
        char ip[20] = "-";
        web_files_get_addr(ip, sizeof(ip));
        if (ip[0] && strcmp(ip, "-") != 0)
            snprintf(dlg.note, sizeof(dlg.note), "IP\xef\xbc\x9a%s", ip);
    }
    dlg.cb = wf_icon_btn_cb;
    os_dialog_push(ctx, &dlg);
}

/* ★ 2026-11-01 linit: 「挂载到电脑」二级菜单取消 —— 两个去向(USB访问存储 / 网络访问存储)
 *   已直接提到存储主菜单最下面两行, 这里原有两个函数(mount_way_cb / open_mount_way)删除。 */

static void open_sd_dlg(ui_ctx_t *ctx) {
    /* ★ 2026-11-01 linit 再调整:
     *   ① 「格式化TF卡」→ 已移到「存储信息」页顶部
     *   ② 「挂载到电脑」二级菜单取消, 两个去向直接摆到最下面两行:
     *        倒数第二行 USB访问存储 (原 USB 数据线挂载)
     *        最后一行   网络访问存储
     *   ③ 「重新挂载TF卡」删除 (换卡后 sd_remount 由 SD watcher 自动补挂)
     *   最终 5 项。 */
    static const char *const items[5] = {
        "\xe6\xb5\x8f\xe8\xa7\x88\xe6\x96\x87\xe4\xbb\xb6",                    /* 0 浏览文件 */
        "\xe5\xad\x98\xe5\x82\xa8\xe4\xbf\xa1\xe6\x81\xaf",                     /* 1 存储信息 (顶部含格式化) */
        "\xe5\xa4\x87\xe4\xbb\xbd\xe8\xbf\x98\xe5\x8e\x9f",                     /* 2 备份还原 */
        "USB\xe8\xae\xbf\xe9\x97\xae\xe5\xad\x98\xe5\x82\xa8",                  /* 3 USB访问存储 */
        "\xe7\xbd\x91\xe7\xbb\x9c\xe8\xae\xbf\xe9\x97\xae\xe5\xad\x98\xe5\x82\xa8", /* 4 网络访问存储 */
    };
    os_dlg_stack_t dlg;
    memset(&dlg, 0, sizeof(dlg));
    dlg.count = 5;
    for (int i = 0; i < 5; i++)
        snprintf(dlg.items[i], sizeof(dlg.items[i]), "%s", items[i]);
    dlg.sel = 0;
    dlg.on_key = sd_main_key;
    dlg.cb = sd_close;   /* 返回关闭主弹窗 → 退出存储页 */
    os_dialog_push(ctx, &dlg);
}

static void p_sto_enter(ui_ctx_t *ctx) {
    /* ★ 2026-11-21: 存储页是"管卡"的唯一入口 ⇒ 这里解除"已确认无卡"锁存,
     *   并让后台补探一次 —— 开机后插卡不用重启也能认到, 且不阻塞弹窗打开。
     *   (其它页面 (游戏/音乐/阅读器) 只在锁存为"有卡"时才扫目录, 无卡时秒开不探测) */
    if (!sd_is_mounted()) {
        sd_probe_invalidate();
        sd_probe_async();
    }
    open_sd_dlg(ctx);
}

/* 退出存储页: 只"表达意图", 不在这里做任何卡操作。
 *
 * ★ 2026-11-25 重构: 原来这里要等后台卸载收尾、再 stop 一次、再重挂 VFS、再补
 *   resume/watcher —— 那些正是"双所有者"造成的: 页面和后台任务都能改同一套状态,
 *   于是必须靠 s_unmounting 等标志互相让路(还可能双 free)。
 *   现在卡的生命周期只有 usbh_msc 会话任务一个所有者:
 *     · 正在交卡(M_MOUNTING) → request_cancel(): 后台在阶段边界收手;
 *     · 已挂载(USB)          → request_stop()  : 后台自己停 USB + 收回本地存储;
 *     · 其它过渡态           → 什么都不做, 会话任务本来就会跑完并落终态。
 *   页面退出不影响后台完成; 下次进存储页读到的就是状态机的当前值。 */
static void p_sto_exit(ui_ctx_t *ctx) {
    (void)ctx;
    usbh_msc_state_t st = usbh_msc_status().state;
    if (st == USBH_MSC_PREPARING) {
        ESP_LOGI(TAG, "退出存储页: 挂载尚未完成 ⇒ 请求取消");
        usbh_msc_request_cancel();
    } else if (st == USBH_MSC_USB) {
        ESP_LOGI(TAG, "退出存储页: 停止 USB MSC 并收回本地存储 (后台执行, 不重启)");
        usbh_msc_request_stop();
    }
}

/* 页面本体**不渲染** —— 存储只做弹窗界面 (用户规范 2026-09-28)。
 * 这里曾加过一个"标题 + TF 卡状态 + 按返回键退出"的整页兜底画面,
 * 用户明确不要: 返回时应该直接回主菜单, 不要多出一层全屏页。
 * 弹窗关闭与页面退出在 sd_close 里一次做完, 所以不会出现空白中间态。 */

static const os_module_t s_mod_storage = {
    .name       = "storage",
    .page_id    = OS_PAGE_STORAGE,
    .on_enter   = p_sto_enter,
    .on_exit    = p_sto_exit,
    .fullscreen = false,
    .exit_confirm = true,   /* ★ 2026-10-25 linit: 应用页按返回键先弹"退出程序?" */
};

void os_page_storage_register(void) { os_register(&s_mod_storage); }
