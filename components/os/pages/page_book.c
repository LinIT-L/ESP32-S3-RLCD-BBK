/**
 * page_book.c — 电子书 页面模块.
 *
 * 书架双栏 (左分类 / 右书籍) 使用统一模板 os_pane + 阅读器模式 (book_reader 组件, 运行时全屏).
 * 私有 state 一律 static 留在本文件, 不放进 ui_ctx.
 */
#include "os.h"
#include "os_pane.h"
#include "ui_common.h"
#include "book_reader.h"
#include "font_book.h"
#include "sd_scan.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_attr.h"
#include <dirent.h>
#include <string.h>
#include <stdio.h>

#define TAG "PBOOK"

#define BOOK_ROOT "/sdcard/books"
#define BNS "os_book"

static bool is_txt(const char *name) {
    size_t len = strlen(name);
    return (len > 4) && (strcasecmp(name + len - 4, ".txt") == 0);
}

static EXT_RAM_BSS_ATTR os_pane_t s_bp;   /* 双栏模板状态 (PSRAM, 省 30KB 内部 RAM) */

/* 阅读器模式 (运行时全屏) */
static bool s_reader_open = false;

/* ==== 阅读优化设置 (3.3: 字号/字体/夜间/页码/边距/行高/字距/旋转) ==== */
static int s_bs_fontsize = 1;   /* 0=20 1=24 2=28 3=32 */
static int s_bs_font     = 0;   /* 0=仿宋 1=黑体 2+=TF 字体索引 */
static char s_bs_tfname[64] = {0};   /* 选中的 TF 字库文件名 */
static int s_bs_night    = 0;   /* 0=关 1=开 */
static int s_bs_pagenum  = 1;   /* 0=关 1=开 */
static int s_bs_margin   = 1;   /* 0=窄 1=中 2=宽 */
static int s_bs_lineh    = 1;   /* 0=紧凑 1=标准 2=宽松 */
static int s_bs_gap      = 0;   /* 0=标准 1=宽松 */
static int s_bs_rot      = 0;   /* 0=上 1=下 2=左 3=右 (阅读优化旋转方向) */

static void bs_load(void) {
    nvs_handle_t h;
    if (nvs_open(BNS, NVS_READONLY, &h) == ESP_OK) {
        int32_t v;
        if (nvs_get_i32(h, "fs", &v) == ESP_OK && v >= 0 && v <= 3) s_bs_fontsize = (int)v;
        if (nvs_get_i32(h, "ft", &v) == ESP_OK && v >= 0 && v <= 1) s_bs_font = (int)v;
        if (nvs_get_i32(h, "nt", &v) == ESP_OK && v >= 0 && v <= 1) s_bs_night = (int)v;
        if (nvs_get_i32(h, "pn", &v) == ESP_OK && v >= 0 && v <= 1) s_bs_pagenum = (int)v;
        if (nvs_get_i32(h, "mg", &v) == ESP_OK && v >= 0 && v <= 2) s_bs_margin = (int)v;
        if (nvs_get_i32(h, "lh", &v) == ESP_OK && v >= 0 && v <= 2) s_bs_lineh = (int)v;
        if (nvs_get_i32(h, "gp", &v) == ESP_OK && v >= 0 && v <= 1) s_bs_gap = (int)v;
        if (nvs_get_i32(h, "rt", &v) == ESP_OK && v >= 0 && v <= 3) s_bs_rot = (int)v;
        nvs_close(h);
    }
}
static void bs_save(void) {
    nvs_handle_t h;
    if (nvs_open(BNS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "fs", s_bs_fontsize);
        nvs_set_i32(h, "ft", s_bs_font);
        nvs_set_i32(h, "nt", s_bs_night);
        nvs_set_i32(h, "pn", s_bs_pagenum);
        nvs_set_i32(h, "mg", s_bs_margin);
        nvs_set_i32(h, "lh", s_bs_lineh);
        nvs_set_i32(h, "gp", s_bs_gap);
        nvs_set_i32(h, "rt", s_bs_rot);
        nvs_commit(h);
        nvs_close(h);
    }
}
static void bs_apply(void) {
    int style = (s_bs_font >= 2) ? 0 : s_bs_font;   /* TF 字库时不传内置 style, 防越界 */
    if (s_bs_font >= 2 && s_bs_tfname[0]) {
        char p[128]; snprintf(p, sizeof(p), "/sdcard/fonts/%s", s_bs_tfname);
        font_book_select_file(p);
    }
    book_reader_set_settings(false, 1, s_bs_night != 0, s_bs_pagenum != 0, s_bs_rot,
                             style, s_bs_fontsize, s_bs_margin, s_bs_lineh, s_bs_gap);
}

/* 阅读优化右栏渲染 (3.3: 全角空格对齐冒号, 选中反色) */
#define BS_FULL "\xe3\x80\x80"
static void book_pad(char *buf, size_t len, int target, const char *name, const char *val) {
    int n = 0;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++)
        if ((*p & 0xC0) != 0x80) n++;
    int pad = (n < target) ? target - n : 0, pos = 0;
    for (int i = 0; i < pad && pos < (int)len; i++)
        pos += snprintf(buf + pos, len - (size_t)pos, "%s", BS_FULL);
    snprintf(buf + pos, len - (size_t)pos, "%s: %s", name, val);
}
static const char *bs_fs_name(void) {
    switch (s_bs_fontsize) { case 0: return "20"; case 2: return "28"; case 3: return "32"; default: return "24"; }
}
static const char *bs_font_name(void) { return s_bs_font ? "\xe9\xbb\x91\xe4\xbd\x93" : "\xe4\xbb\xbf\xe5\xae\x8b"; } /* 黑体/仿宋 */
static const char *bs_on_off(int v) { return v ? "\xe5\xbc\x80" : "\xe5\x85\xb3"; } /* 开/关 */
static const char *bs_mg_name(void) {
    switch (s_bs_margin) { case 0: return "\xe7\xaa\x84"; /*窄*/ case 2: return "\xe5\xae\xbd"; /*宽*/ default: return "\xe4\xb8\xad"; /*中*/ }
}
static const char *bs_lh_name(void) {
    switch (s_bs_lineh) { case 0: return "\xe7\xb4\xa7\xe5\x87\x91"; /*紧凑*/ case 2: return "\xe5\xae\xbd\xe6\x9d\xbe"; /*宽松*/ default: return "\xe6\xa0\x87\xe5\x87\x86"; /*标准*/ }
}
static const char *bs_gap_name(void) { return s_bs_gap ? "\xe5\xae\xbd\xe6\x9d\xbe" : "\xe6\xa0\x87\xe5\x87\x86"; } /* 宽松/标准 */

static const char *bs_rot_name(void) {
    switch (s_bs_rot) { case 1: return "\xe4\xb8\x8b"; /*下*/ case 2: return "\xe5\xb7\xa6"; /*左*/ case 3: return "\xe5\x8f\xb3"; /*右*/ default: return "\xe4\xb8\x8a"; /*上*/ }
}
static void p_book_settings_render(ui_ctx_t *ctx, os_pane_t *p, int rx0, int rx1, int y) {
    (void)rx1;
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return;
    char buf[64];
    for (int i = 0; i < p->settings_count; i++) {
        switch (i) {
        case 0: book_pad(buf, sizeof(buf), 5, "\xe5\xad\x97\xe4\xbd\x93\xe5\xa4\xa7\xe5\xb0\x8f", bs_fs_name()); break; /*字体大小*/
        case 1: book_pad(buf, sizeof(buf), 5, "\xe5\xad\x97\xe4\xbd\x93", bs_font_name()); break; /*字体*/
        case 2: book_pad(buf, sizeof(buf), 5, "\xe5\xa4\x9c\xe9\x97\xb4\xe6\xa8\xa1\xe5\xbc\x8f", bs_on_off(s_bs_night)); break; /*夜间模式*/
        case 3: book_pad(buf, sizeof(buf), 5, "\xe6\x98\xbe\xe7\xa4\xba\xe9\xa1\xb5\xe7\xa0\x81", bs_on_off(s_bs_pagenum)); break; /*显示页码*/
        case 4: book_pad(buf, sizeof(buf), 5, "\xe8\xbe\xb9\xe8\xb7\x9d", bs_mg_name()); break; /*边距*/
        case 5: book_pad(buf, sizeof(buf), 5, "\xe8\xa1\x8c\xe9\xab\x98", bs_lh_name()); break; /*行高*/
        case 6: book_pad(buf, sizeof(buf), 5, "\xe5\xad\x97\xe8\xb7\x9d", bs_gap_name()); break; /*字距*/
        default: book_pad(buf, sizeof(buf), 5, "\xe6\x97\x8b\xe8\xbd\xac\xe6\x96\xb9\xe5\x90\x91", bs_rot_name()); break; /*旋转方向*/
        }
        int row_y = y + i * 32;
        bool sel = (p->focus == 1 && i == p->sel_item);
        if (sel) fill_rect(lcd, rx0, row_y, rx1, row_y + 30, ST7305_COLOR_BLACK);
        draw_text(lcd, rx0 + 8, row_y + 4, buf, sel);
    }
}

/* TF 卡字体文件夹扫描: 返回字体文件数 (存 s_tf_fonts) */
EXT_RAM_BSS_ATTR static char s_tf_fonts[16][64];
static int  s_tf_font_n = 0;
static void tf_font_scan(void) {
    s_tf_font_n = 0;
    DIR *d = opendir("/sdcard/fonts");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && s_tf_font_n < 16) {
        if (e->d_name[0] == '.') continue;
        const char *ext = strrchr(e->d_name, '.');
        if (ext && (strcasecmp(ext, ".fnt") == 0 || strcasecmp(ext, ".bin") == 0))
            snprintf(s_tf_fonts[s_tf_font_n++], 64, "%.63s", e->d_name);
    }
    closedir(d);
}

/* 字体选择列表弹窗 */
static void bs_font_cb(ui_ctx_t *ctx, int result, void *ud) {
    (void)ud;
    if (result < 0) return;   /* 返回 */
    int builtin = 2;   /* 内置字体数: 仿宋/黑体 */
    if (result < 2) {
        s_bs_font = result;   /* 0=仿宋 1=黑体 */
        bs_save(); bs_apply();
        os_dialog_toast(ctx, result ? "\xe5\xad\x97\xe4\xbd\x93\xe5\xb7\xb2\xe5\x88\x87\xe6\x8d\xa2: \xe9\xbb\x91\xe4\xbd\x93" /*字体已切换: 黑体*/
                                  : "\xe5\xad\x97\xe4\xbd\x93\xe5\xb7\xb2\xe5\x88\x87\xe6\x8d\xa2: \xe4\xbb\xbf\xe5\xae\x8b"); /*字体已切换: 仿宋*/
    } else {
        int b = 2;   /* 内置字体数: 仿宋/黑体 */
        int idx = result - b;
        if (idx >= 0 && idx < s_tf_font_n) {
            char path[128];
            snprintf(path, sizeof(path), "/sdcard/fonts/%s", s_tf_fonts[idx]);
            if (font_book_select_file(path)) {
                s_bs_font = result;
                strncpy(s_bs_tfname, s_tf_fonts[idx], sizeof(s_bs_tfname) - 1);
                s_bs_tfname[sizeof(s_bs_tfname) - 1] = 0;
                bs_save(); bs_apply();
                char msg[64];
                snprintf(msg, sizeof(msg), "\xe5\xad\x97\xe4\xbd\x93\xe5\xb7\xb2\xe5\x88\x87\xe6\x8d\xa2: %s", /* 字体已切换: */
                         s_tf_fonts[idx]);
                os_dialog_toast(ctx, msg);
            } else {
                os_dialog_toast(ctx, "\xe5\xad\x97\xe4\xbd\x93\xe5\x8a\xa0\xe8\xbd\xbd\xe5\xa4\xb1\xe8\xb4\xa5"); /* 字体加载失败 */
            }
        }
    }
}
static void bs_font_dlg(ui_ctx_t *ctx) {
    tf_font_scan();
    EXT_RAM_BSS_ATTR static char items[20][40];
    static const char *ptrs[20];
    int n = 0;
    snprintf(items[n++], 40, "%s%s", (s_bs_font == 0) ? "* " : "", "\xe4\xbb\xbf\xe5\xae\x8b"); /* 仿宋 */
    snprintf(items[n++], 40, "%s%s", (s_bs_font == 1) ? "* " : "", "\xe9\xbb\x91\xe4\xbd\x93"); /* 黑体 */
    for (int i = 0; i < s_tf_font_n && n < 20; i++)
        snprintf(items[n++], 40, "%s%s", "", s_tf_fonts[i]);
    for (int i = 0; i < n; i++) ptrs[i] = items[i];
    os_dialog_list(ctx, "", ptrs, n, (s_bs_font < 2) ? s_bs_font : 2, bs_font_cb, NULL);
}

static void p_book_settings_select(ui_ctx_t *ctx, os_pane_t *p, int item) {
    (void)p;
    switch (item) {
    case 0: s_bs_fontsize = (s_bs_fontsize + 1) % 4; break;
    case 1: bs_font_dlg(ctx); return;   /* 字体 → 列表弹窗 */
    case 2: s_bs_night ^= 1; break;
    case 3: s_bs_pagenum ^= 1; break;
    case 4: s_bs_margin = (s_bs_margin + 1) % 3; break;
    case 5: s_bs_lineh = (s_bs_lineh + 1) % 3; break;
    case 6: s_bs_gap ^= 1; break;
    default: s_bs_rot = (s_bs_rot + 1) % 4; break;   /* 旋转方向 上→下→左→右 */
    }
    bs_save(); bs_apply();
    ctx->needs_redraw = true;
}

/* 右栏确认: 打开书籍 */
static void on_book_select(ui_ctx_t *ctx, os_pane_t *p,
                           const char *path, const char *name) {
    (void)p; (void)name;
    bs_apply();
    if (book_reader_open(path)) {
        s_reader_open = true;
        ctx->fullscreen = true;
        ESP_LOGI(TAG, "打开: %s", path);
    }
}

static void p_book_enter(ui_ctx_t *ctx) {
    if (!s_bp.loaded) {
        s_bp.root = BOOK_ROOT;
        s_bp.is_item = is_txt;
        s_bp.on_select = on_book_select;
        s_bp.all_label = NULL;
        /* 侧栏首项「阅读优化」(3.3): 选中后右栏内联显示阅读设置 */
        s_bp.settings_label = "\xe9\x98\x85\xe8\xaf\xbb\xe4\xbc\x98\xe5\x8c\x96"; /* 阅读优化 */
        s_bp.settings_count = 8;
        s_bp.settings_render = p_book_settings_render;
        s_bp.on_settings_select = p_book_settings_select;
        s_bp.list_y = 30;   /* 状态栏(24)下方 */
        bs_load();
        os_pane_reset(&s_bp);
    }
    os_pane_build_right(&s_bp);
}

static void p_book_render(ui_ctx_t *ctx) {
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return;
    if (s_reader_open && book_reader_is_open()) {
        book_reader_render(lcd);
        ctx->fullscreen = true;
        return;
    }
    ctx->fullscreen = false;
    st7305_clear(lcd, ST7305_COLOR_WHITE);
    os_pane_render(ctx, &s_bp);
}

static void p_book_action(ui_ctx_t *ctx, os_action_t a) {
    if (s_reader_open) {
        if (a == OS_ACTION_BACK) {
            book_reader_close();
            s_reader_open = false;
            ctx->fullscreen = false;
            ctx->needs_redraw = true;
            return;
        }
        if (book_reader_handle_action((int)a)) ctx->needs_redraw = true;
        return;
    }
    if (os_pane_action(ctx, &s_bp, a)) return;
    if (a == OS_ACTION_BACK) os_pop(ctx);
}

static bool p_book_touch(ui_ctx_t *ctx, int x, int y) {
    if (s_reader_open) {
        if (book_reader_handle_touch(x, y)) ctx->needs_redraw = true;
        return true;   /* 阅读器内全部消费 */
    }
    return os_pane_touch(ctx, &s_bp, x, y);
}

static void p_book_poll(ui_ctx_t *ctx) {
    if (!s_reader_open) os_pane_poll(ctx, &s_bp);   /* 书架右栏触摸拖动滚动 */
}

static const os_module_t s_mod_book = {
    .name       = "book",
    .page_id    = OS_PAGE_BOOK,
    .on_enter   = p_book_enter,
    .render     = p_book_render,
    .action     = p_book_action,
    .touch      = p_book_touch,
    .poll       = p_book_poll,
    .fullscreen = false,
};

void os_page_book_register(void) { os_register(&s_mod_book); }
