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
#include "input.h"
#include "favorites.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include <dirent.h>
#include <string.h>
#include <stdio.h>

#define TAG "PBOOK"

#define BOOK_ROOT "/sdcard/books"
#define BNS "os_book"

/* 书籍文件过滤: txt/epub/fb2 (损坏/0字节也列出, 打开时再提示损坏) */
static bool is_txt(const char *name) {
    size_t len = strlen(name);
    if (len < 5) return false;
    const char *e = name + len - 4;
    return (strcasecmp(e, ".txt") == 0) || (strcasecmp(e, ".epub") == 0) ||
           (strcasecmp(e, ".fb2") == 0);
}

static EXT_RAM_BSS_ATTR os_pane_t s_bp;   /* 双栏模板状态 (PSRAM, 省 30KB 内部 RAM) */

/* 阅读器模式 (运行时全屏) */
static bool s_reader_open = false;

/* ==== 阅读优化设置 (3.3: 字号/字体/页码/边距/行高/字距/缩进/旋转) ==== */
static int s_bs_fontsize = 1;   /* 0=20 1=24 2=28 3=32 */
static int s_bs_font     = 0;   /* 0=仿宋 1=黑体 2+=TF 字体索引 */
static char s_bs_tfname[64] = {0};   /* 选中的 TF 字库文件名 */
static int s_bs_pagenum  = 1;   /* 0=关 1=开 */
static int s_bs_margin   = 1;   /* 0=窄 1=中 2=宽 */
static int s_bs_lineh    = 1;   /* 0=紧凑 1=标准 2=宽松 */
static int s_bs_gap      = 1;   /* 0=紧凑 1=标准 2=宽松 */
static int s_bs_indent   = 1;   /* 0=关 1=开 (段落首行缩进两全角空格) */
static int s_bs_rot      = 0;   /* 0=上 1=下 2=左 3=右 (阅读优化旋转方向) */

static void bs_load(void) {
    nvs_handle_t h;
    if (nvs_open(BNS, NVS_READONLY, &h) == ESP_OK) {
        int32_t v;
        if (nvs_get_i32(h, "fs", &v) == ESP_OK && v >= 0 && v <= 3) s_bs_fontsize = (int)v;
        if (nvs_get_i32(h, "ft", &v) == ESP_OK && v >= 0 && v <= 1) s_bs_font = (int)v;
        if (nvs_get_i32(h, "pn", &v) == ESP_OK && v >= 0 && v <= 1) s_bs_pagenum = (int)v;
        if (nvs_get_i32(h, "mg", &v) == ESP_OK && v >= 0 && v <= 2) s_bs_margin = (int)v;
        if (nvs_get_i32(h, "lh", &v) == ESP_OK && v >= 0 && v <= 2) s_bs_lineh = (int)v;
        if (nvs_get_i32(h, "gp", &v) == ESP_OK && v >= 0 && v <= 2) s_bs_gap = (int)v;
        if (nvs_get_i32(h, "in", &v) == ESP_OK && v >= 0 && v <= 1) s_bs_indent = (int)v;
        if (nvs_get_i32(h, "rt", &v) == ESP_OK && v >= 0 && v <= 3) s_bs_rot = (int)v;
        nvs_close(h);
    }
}
static void bs_save(void) {
    nvs_handle_t h;
    if (nvs_open(BNS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "fs", s_bs_fontsize);
        nvs_set_i32(h, "ft", s_bs_font);
        nvs_set_i32(h, "pn", s_bs_pagenum);
        nvs_set_i32(h, "mg", s_bs_margin);
        nvs_set_i32(h, "lh", s_bs_lineh);
        nvs_set_i32(h, "gp", s_bs_gap);
        nvs_set_i32(h, "in", s_bs_indent);
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
    book_reader_set_settings(false, 1, false, s_bs_pagenum != 0, s_bs_rot,
                             style, s_bs_fontsize, s_bs_margin, s_bs_lineh, s_bs_gap,
                             s_bs_indent);
}

/* ==== 书架旋转 (自包含) ====
 * 渲染: 竖屏(左2/右3) → st7305 pt 逻辑缓冲 300×400; 180°(下1) → st7305 flip;
 *       上(0) 原样. 触摸由本页 render/touch/drag 内用 book_touch_map 映射,
 *       不触碰全局 input 旋转 (避免与 os_dialog 等其它页面共享的 s_screen_rot 冲突). */
static uint8_t *s_book_pfb  = NULL;   /* 竖屏逻辑缓冲 40×400 */
static uint8_t *s_book_flip = NULL;   /* 180° 翻转缓冲 15000 */

static void book_touch_map(int *x, int *y) {
    int ox = *x, oy = *y;
    switch (s_bs_rot & 3) {
    case 1: *x = ST7305_WIDTH  - 1 - ox; *y = ST7305_HEIGHT - 1 - oy; break;
    case 2: *x = oy;                      *y = ST7305_WIDTH  - 1 - ox; break;
    case 3: *x = ST7305_HEIGHT - 1 - oy; *y = ox;                      break;
    default: break;
    }
}

static void book_rotation_off(ui_ctx_t *ctx) {
    if (ctx && ctx->lcd) st7305_set_rotation(ctx->lcd, 0, NULL);
    ui_set_portrait(false);
    if (ctx) ctx->map_touch = NULL;
}

static void book_rotation_ensure(ui_ctx_t *ctx) {
    int rot = s_bs_rot & 3;
    bool portrait = (rot == 2 || rot == 3);
    ui_set_portrait(portrait);
    if (portrait && !s_book_pfb)
        s_book_pfb = heap_caps_malloc(40 * 400, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (rot == 1 && !s_book_flip)
        s_book_flip = heap_caps_malloc(15000, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ctx && ctx->lcd)
        st7305_set_rotation(ctx->lcd, rot, portrait ? s_book_pfb : s_book_flip);
    s_bp.list_y = portrait ? (24 + 4) : 30;   /* 竖屏: 状态栏(24)下方; 横屏: 状态栏(24)+4 */
    if (ctx) ctx->map_touch = NULL;
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
    switch (s_bs_margin) { case 0: return "\xe6\x94\xb6\xe7\xaa\x84"; /*收窄*/ case 2: return "\xe5\x8a\xa0\xe5\xae\xbd"; /*加宽*/ default: return "\xe4\xb8\xad\xe7\xad\x89"; /*中等*/ }
}
static const char *bs_lh_name(void) {
    switch (s_bs_lineh) { case 0: return "\xe7\xb4\xa7\xe5\x87\x91"; /*紧凑*/ case 2: return "\xe5\xae\xbd\xe6\x9d\xbe"; /*宽松*/ default: return "\xe6\xa0\x87\xe5\x87\x86"; /*标准*/ }
}
static const char *bs_gap_name(void) {
    switch (s_bs_gap) { case 0: return "\xe7\xb4\xa7\xe5\x87\x91"; /*紧凑*/ case 2: return "\xe5\xae\xbd\xe6\x9d\xbe"; /*宽松*/ default: return "\xe6\xa0\x87\xe5\x87\x86"; /*标准*/ }
}

static const char *bs_rot_name(void) {
    switch (s_bs_rot) { case 1: return "\xe4\xb8\x8b"; /*下*/ case 2: return "\xe5\xb7\xa6"; /*左*/ case 3: return "\xe5\x8f\xb3"; /*右*/ default: return "\xe4\xb8\x8a"; /*上*/ }
}

/* 旋转方向: 上(0)->左(2)->下(1)->右(3) (逆时针 90°) */
static int rot_next_cw(int rot) {
    static const int seq[4] = { 0, 2, 1, 3 };   /* 上->左->下->右 (逆时针 90°) */
    int i = 0;
    while (i < 4 && seq[i] != (rot & 3)) i++;
    return seq[(i + 1) & 3];
}
static void p_book_settings_render(ui_ctx_t *ctx, os_pane_t *p, int rx0, int rx1,
                                   int list_y, int list_bottom, int top_row, int off_mod) {
    (void)rx1;
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return;
    char buf[64];
    /* 先求可见行中最长文本宽度, 作为整个设置块宽度, 保持各选项原排布整体居中 */
    int max_w = 0;
    for (int i = 0; i < p->settings_count; i++) {
        int idx = top_row + i;
        if (idx >= p->settings_count) break;
        switch (idx) {
        case 0: book_pad(buf, sizeof(buf), 4, "\xe5\xad\x97\xe4\xbd\x93\xe5\xa4\xa7\xe5\xb0\x8f", bs_fs_name()); break; /*字体大小*/
        case 1: book_pad(buf, sizeof(buf), 4, "\xe5\xad\x97\xe4\xbd\x93", bs_font_name()); break; /*字体*/
        case 2: book_pad(buf, sizeof(buf), 4, "\xe6\x98\xbe\xe7\xa4\xba\xe8\xbf\x9b\xe5\xba\xa6", bs_on_off(s_bs_pagenum)); break; /*显示进度*/
        case 3: book_pad(buf, sizeof(buf), 4, "\xe8\xbe\xb9\xe8\xb7\x9d", bs_mg_name()); break; /*边距*/
        case 4: book_pad(buf, sizeof(buf), 4, "\xe8\xa1\x8c\xe9\xab\x98", bs_lh_name()); break; /*行高*/
        case 5: book_pad(buf, sizeof(buf), 4, "\xe5\xad\x97\xe8\xb7\x9d", bs_gap_name()); break; /*字距*/
        case 6: book_pad(buf, sizeof(buf), 4, "\xe6\xae\xb5\xe8\x90\xbd\xe7\xbc\xa9\xe8\xbf\x9b", bs_on_off(s_bs_indent)); break; /*段落缩进*/
        default: book_pad(buf, sizeof(buf), 4, "\xe6\x97\x8b\xe8\xbd\xac\xe6\x96\xb9\xe5\x90\x91", bs_rot_name()); break; /*旋转方向*/
        }
        int tw = text_width(buf);
        if (tw > max_w) max_w = tw;
    }
    /* 整块水平居中: 所有行同一起始 x, 保持原排布 */
    int block_x = rx0 + ((rx1 - rx0) - max_w) / 2;
    if (block_x < rx0 + 2) block_x = rx0 + 2;
    for (int i = 0; i < p->settings_count; i++) {
        int idx = top_row + i;
        if (idx >= p->settings_count) break;
        int row_y = list_y + i * 32 - off_mod;   /* 平滑跟手滚动 (游戏菜单同款) */
        if (row_y + 32 < list_y || row_y >= list_bottom) continue;
        switch (idx) {
        case 0: book_pad(buf, sizeof(buf), 4, "\xe5\xad\x97\xe4\xbd\x93\xe5\xa4\xa7\xe5\xb0\x8f", bs_fs_name()); break; /*字体大小*/
        case 1: book_pad(buf, sizeof(buf), 4, "\xe5\xad\x97\xe4\xbd\x93", bs_font_name()); break; /*字体*/
        case 2: book_pad(buf, sizeof(buf), 4, "\xe6\x98\xbe\xe7\xa4\xba\xe8\xbf\x9b\xe5\xba\xa6", bs_on_off(s_bs_pagenum)); break; /*显示进度*/
        case 3: book_pad(buf, sizeof(buf), 4, "\xe8\xbe\xb9\xe8\xb7\x9d", bs_mg_name()); break; /*边距*/
        case 4: book_pad(buf, sizeof(buf), 4, "\xe8\xa1\x8c\xe9\xab\x98", bs_lh_name()); break; /*行高*/
        case 5: book_pad(buf, sizeof(buf), 4, "\xe5\xad\x97\xe8\xb7\x9d", bs_gap_name()); break; /*字距*/
        case 6: book_pad(buf, sizeof(buf), 4, "\xe6\xae\xb5\xe8\x90\xbd\xe7\xbc\xa9\xe8\xbf\x9b", bs_on_off(s_bs_indent)); break; /*段落缩进*/
        default: book_pad(buf, sizeof(buf), 4, "\xe6\x97\x8b\xe8\xbd\xac\xe6\x96\xb9\xe5\x90\x91", bs_rot_name()); break; /*旋转方向*/
        }
        bool sel = (p->focus == 1 && idx == p->sel_item);
        if (sel) fill_rect(lcd, block_x, row_y, rx1, row_y + 30, ST7305_COLOR_BLACK);
        draw_text(lcd, block_x, row_y + 4, buf, sel);
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

/* ==== 自适应滚轮弹窗 (字号/字体): 尺寸随内容伸缩, 单列纵向循环滚动 ==== */
#define BS_WHEEL_SIDE  2
#define BS_WHEEL_STEP  30
static const char *s_bs_wheel_opts[24];   /* 选项文字 (UTF-8) */
static int   s_bs_wheel_n = 0;
static int   s_bs_wheel_idx = 0;
static void (*s_bs_wheel_commit)(void);   /* 确认提交回调 */

/* 自适应几何: 宽=最长选项+边距, 高=可见行+按钮区, 均夹在屏幕内 */
static void bs_wheel_geom(int *bx, int *by, int *W, int *H) {
    int mw = 44;
    for (int i = 0; i < s_bs_wheel_n; i++) {
        int tw = text_width(s_bs_wheel_opts[i]);
        if (tw > mw) mw = tw;
    }
    int w = mw + 48;
    if (w < 140) w = 140;
    if (w > ui_screen_w() - 12) w = ui_screen_w() - 12;
    int h = (2 * BS_WHEEL_SIDE + 1) * BS_WHEEL_STEP + 50;
    if (h < 150) h = 150;
    if (h > ui_screen_h() - 12) h = ui_screen_h() - 12;
    *W = w; *H = h;
    *bx = (ui_screen_w() - w) / 2;
    *by = (ui_screen_h() - h) / 2;
}

/* 按像素宽截断 UTF-8 文本 (至少保留 1 个完整字符) */
static void bs_wheel_fit(const char *s, int maxw, char *out, size_t n) {
    size_t w = 0;
    int i = 0;
    while (s[i] && i + 1 < (int)n) {
        unsigned char c = (unsigned char)s[i];
        int cl = (c < 0x80) ? 1 : ((c & 0xF0) == 0xE0) ? 3 : ((c & 0xE0) == 0xC0) ? 2 : 4;
        int cw = (c < 0x80) ? font_book_ascii_w() : font_book_cell_w();
        if (w > 0 && w + (size_t)cw > (size_t)maxw) break;
        for (int k = 0; k < cl && s[i + k] && (int)w + k + 1 < (int)n; k++) out[w++] = s[i + k];
        i += cl;
    }
    out[w] = 0;
}

static bool bs_wheel_render(ui_ctx_t *ctx, os_dlg_stack_t *d, void *ud) {
    (void)d; (void)ud;
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return true;
    int bx, by, W, H; bs_wheel_geom(&bx, &by, &W, &H);
    fill_rect(lcd, bx, by, bx + W - 1, by + H - 1, ST7305_COLOR_WHITE);
    for (int k = 0; k < 2; k++) {
        draw_hline(lcd, bx + k, bx + W - 1 - k, by + k, ST7305_COLOR_BLACK);
        draw_hline(lcd, bx + k, bx + W - 1 - k, by + H - 1 - k, ST7305_COLOR_BLACK);
        draw_vline(lcd, bx + k, by + k, by + H - 1 - k, ST7305_COLOR_BLACK);
        draw_vline(lcd, bx + W - 1 - k, by + k, by + H - 1 - k, ST7305_COLOR_BLACK);
    }
    const int bbt = by + H - 2 - 44;   /* 底部按钮区顶 */
    const int cy = by + 2 + (bbt - (by + 2)) / 2;
    const int y0 = cy - 13;
    int ccx = bx + W / 2;
    int n = s_bs_wheel_n; if (n < 1) n = 1;
    char tmp[40];
    for (int k = -BS_WHEEL_SIDE; k <= BS_WHEEL_SIDE; k++) {
        if (k == 0) continue;
        int idx = (s_bs_wheel_idx + k + n * 4) % n;
        bs_wheel_fit(s_bs_wheel_opts[idx], W - 24, tmp, sizeof(tmp));
        draw_text(lcd, ccx - text_width(tmp) / 2, y0 + k * BS_WHEEL_STEP, tmp, false);
    }
    /* 当前值黑框反色 */
    bs_wheel_fit(s_bs_wheel_opts[s_bs_wheel_idx], W - 24, tmp, sizeof(tmp));
    int vw = text_width(tmp);
    fill_rect(lcd, ccx - vw / 2 - 4, y0 - 3, ccx + vw / 2 + 3, y0 + 25, ST7305_COLOR_BLACK);
    draw_text(lcd, ccx - vw / 2, y0, tmp, true);
    /* 底部 确认/取消 */
    draw_hline(lcd, bx + 2, bx + W - 1 - 2, bbt, ST7305_COLOR_BLACK);
    int mid = bx + W / 2;
    draw_vline(lcd, mid, bbt, by + H - 1 - 2, ST7305_COLOR_BLACK);
    int ty = bbt + 10;
    draw_text(lcd, bx + W / 4 - text_width("\xe7\xa1\xae\xe8\xae\xa4") / 2, ty, "\xe7\xa1\xae\xe8\xae\xa4", false);   /* 确认 */
    draw_text(lcd, bx + W * 3 / 4 - text_width("\xe5\x8f\x96\xe6\xb6\x88") / 2, ty, "\xe5\x8f\x96\xe6\xb6\x88", false);  /* 取消 */
    return true;
}

static bool bs_wheel_key(ui_ctx_t *ctx, os_dlg_stack_t *d, os_action_t a, void *ud) {
    (void)d; (void)ud;
    int n = s_bs_wheel_n; if (n < 1) n = 1;
    switch (a) {
    case OS_ACTION_UP:   s_bs_wheel_idx = (s_bs_wheel_idx + 1) % n; ctx->needs_redraw = true; return true;
    case OS_ACTION_DOWN: s_bs_wheel_idx = (s_bs_wheel_idx + n - 1) % n; ctx->needs_redraw = true; return true;
    case OS_ACTION_CONFIRM: if (s_bs_wheel_commit) s_bs_wheel_commit(); os_dialog_pop(ctx); return true;
    default: return false;   /* BACK → 默认取消 */
    }
}

static void bs_wheel_poll(ui_ctx_t *ctx, os_dlg_stack_t *d, void *ud) {
    (void)d; (void)ud;
    int tx, ty;
    static int s_w_last = -1, s_w_acc = 0;
    if (input_get_touch_pos(&tx, &ty)) {
        if (s_w_last < 0) { s_w_last = ty; s_w_acc = 0; }
        else {
            s_w_acc += ty - s_w_last;
            s_w_last = ty;
            int n = s_bs_wheel_n; if (n < 1) n = 1;
            bool changed = false;
            while (s_w_acc <= -BS_WHEEL_STEP) { s_bs_wheel_idx = (s_bs_wheel_idx + 1) % n; s_w_acc += BS_WHEEL_STEP; changed = true; }
            while (s_w_acc >= +BS_WHEEL_STEP) { s_bs_wheel_idx = (s_bs_wheel_idx + n - 1) % n; s_w_acc -= BS_WHEEL_STEP; changed = true; }
            if (changed) ctx->needs_redraw = true;
        }
    } else {
        s_w_last = -1; s_w_acc = 0;
    }
}

static bool bs_wheel_touch(ui_ctx_t *ctx, os_dlg_stack_t *d, int x, int y, void *ud) {
    (void)d; (void)ud;
    int bx, by, W, H; bs_wheel_geom(&bx, &by, &W, &H);
    if (x < bx || x > bx + W - 1 || y < by || y > by + H - 1) return false;
    const int bbt = by + H - 2 - 44;
    if (y >= bbt) {
        int mid = bx + W / 2;
        if (x < mid && s_bs_wheel_commit) s_bs_wheel_commit();
        os_dialog_pop(ctx);
        return true;
    }
    return true;   /* 弹窗内其他区域吞掉 (拖动调值由 poll 处理) */
}

static void bs_wheel_push(ui_ctx_t *ctx) {
    os_dlg_stack_t dlg;
    memset(&dlg, 0, sizeof(dlg));
    dlg.no_footer = true;
    dlg.on_render = bs_wheel_render;
    dlg.on_key    = bs_wheel_key;
    dlg.on_poll   = bs_wheel_poll;
    dlg.on_touch  = bs_wheel_touch;
    os_dialog_push(ctx, &dlg);
}

/* 字号 (4档) */
static void bs_wheel_commit_size(void) { s_bs_fontsize = s_bs_wheel_idx; bs_save(); bs_apply(); }
static void bs_size_wheel_open(ui_ctx_t *ctx) {
    static const char *opts[] = { "20", "24", "28", "32" };
    for (int i = 0; i < 4; i++) s_bs_wheel_opts[i] = opts[i];
    s_bs_wheel_n = 4;
    s_bs_wheel_idx = s_bs_fontsize;
    s_bs_wheel_commit = bs_wheel_commit_size;
    bs_wheel_push(ctx);
}

/* 字体: 内置仿宋/黑体 + TF 字库 */
static void bs_wheel_commit_font(void) {
    int idx = s_bs_wheel_idx;
    if (idx < 2) {
        s_bs_font = idx;   /* 0=仿宋 1=黑体 */
        bs_save(); bs_apply();
    } else {
        int fi = idx - 2;
        if (fi >= 0 && fi < s_tf_font_n) {
            char path[128];
            snprintf(path, sizeof(path), "/sdcard/fonts/%s", s_tf_fonts[fi]);
            if (font_book_select_file(path)) {
                s_bs_font = idx;
                strncpy(s_bs_tfname, s_tf_fonts[fi], sizeof(s_bs_tfname) - 1);
                s_bs_tfname[sizeof(s_bs_tfname) - 1] = 0;
                bs_save(); bs_apply();
            }
        }
    }
}
static void bs_font_wheel_open(ui_ctx_t *ctx) {
    tf_font_scan();
    int n = 0;
    s_bs_wheel_opts[n++] = "\xe4\xbb\xbf\xe5\xae\x8b";  /* 仿宋 */
    s_bs_wheel_opts[n++] = "\xe9\xbb\x91\xe4\xbd\x93";  /* 黑体 */
    for (int i = 0; i < s_tf_font_n && n < 24; i++)
        s_bs_wheel_opts[n++] = s_tf_fonts[i];
    s_bs_wheel_n = n;
    s_bs_wheel_idx = (s_bs_font < 2) ? s_bs_font : 2;
    s_bs_wheel_commit = bs_wheel_commit_font;
    bs_wheel_push(ctx);
}

static void p_book_settings_select(ui_ctx_t *ctx, os_pane_t *p, int item) {
    (void)p;
    switch (item) {
    case 0: bs_size_wheel_open(ctx); return;    /* 字号 → 滚轮 */
    case 1: bs_font_wheel_open(ctx); return;    /* 字体 → 滚轮 */
    case 2: s_bs_pagenum ^= 1; break;           /* 显示页码 点击切换 */
    case 3: s_bs_margin = (s_bs_margin + 1) % 3; break;   /* 边距 点击循环 */
    case 4: s_bs_lineh = (s_bs_lineh + 1) % 3; break;     /* 行高 点击循环 */
    case 5: s_bs_gap = (s_bs_gap + 1) % 3; break;       /* 字距 紧凑/标准/宽松 */
    case 6: s_bs_indent ^= 1; break;            /* 段落缩进 点击切换 */
    default: s_bs_rot = rot_next_cw(s_bs_rot); break;   /* 旋转方向 顺时针+90° */
    }
    bs_save(); bs_apply();
    ctx->needs_redraw = true;
}

/* ==== 书架自包含渲染/触摸/拖动 (不再复用 os_pane 模板的渲染与触摸) ====
 * 保留 os_pane 仅做目录扫描与数据填充 (os_pane_reset/build_right), 
 * 渲染走 st7305 逻辑坐标 (横屏 400×300 / 竖屏 300×400, 与阅读器同源),
 * 触摸与拖动由 input 层按旋转映射为逻辑坐标后直接命中 (与渲染坐标系一致),
 * 底部上滑返回 zone 也随 input_set_screen_rotation 自动跟随显示方向. */
#define SL_ROW_H  32
#define SL_LEFT_W 100
#define SL_GAP    6

/* 左栏总项数 (同 os_pane.total) */
static int book_left_total(void) {
    int n = s_bp.folder_count;
    if (s_bp.settings_label) n++;
    if (s_bp.recent_label) n++;
    if (s_bp.fav_enabled) n++;
    if (!s_bp.hide_all) n++;
    return n;
}

/* UTF-8 截断到最多 max_chars 个字符 (防超长目录名溢出) */
static void book_name_short(char *out, size_t n, const char *src, int max_chars) {
    size_t w = 0;
    int cnt = 0;
    while (*src && cnt < max_chars && w + 1 < n) {
        unsigned char c = (unsigned char)*src;
        int cl = (c < 0x80) ? 1 : ((c & 0xF0) == 0xE0) ? 3 : ((c & 0xE0) == 0xC0) ? 2 : 4;
        for (int i = 0; i < cl && src[i] && w + 1 < n; i++) out[w++] = src[i];
        src += cl;
        cnt++;
    }
    out[w] = 0;
}

/* 左栏显示名 */
static void book_left_name(int disp, char *out, size_t n) {
    int idx = disp;
    size_t cap = 4;
    if (s_bp.settings_label && idx-- == 0) { book_name_short(out, n, s_bp.settings_label, (int)cap); return; }
    if (s_bp.recent_label && idx-- == 0)   { book_name_short(out, n, s_bp.recent_label, (int)cap); return; }
    if (s_bp.fav_enabled && idx-- == 0)    { book_name_short(out, n, "\xe6\x94\xb6\xe8\x97\x8f\xe6\xa0\x8f", (int)cap); return; } /* 收藏栏 */
    if (s_bp.hide_all) { book_name_short(out, n, (idx >= 0 && idx < s_bp.folder_count) ? s_bp.folders[idx] : "", (int)cap); return; }
    if (idx == 0) { book_name_short(out, n, (s_bp.all_label ? s_bp.all_label : "\xe5\x85\xa8\xe9\x83\xa8"), (int)cap); return; } /* 全部 */
    book_name_short(out, n, (idx - 1 >= 0 && idx - 1 < s_bp.folder_count) ? s_bp.folders[idx - 1] : "", (int)cap);
}

/* 右栏平滑滚动最大偏移 */
static int book_max_off(void) {
    int list_y = s_bp.list_y > 0 ? s_bp.list_y : 30;
    int total = s_bp.settings_mode ? s_bp.settings_count : s_bp.item_count;
    int mo = total * SL_ROW_H - (ui_screen_h() - 4 - list_y);
    return mo < 0 ? 0 : mo;
}

/* 渲染双栏 (逻辑坐标; st7305 旋转在 flush 时按当前 target 完成) */
static void book_shelf_render(ui_ctx_t *ctx) {
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return;
    const int list_y = s_bp.list_y > 0 ? s_bp.list_y : 30;
    const int list_bot = ui_screen_h() - 4;
    const int sw = ui_screen_w();
    int lx0 = 2, lx1 = lx0 + SL_LEFT_W - 4;
    int rx0 = lx1 + SL_GAP + 4;
    const int right_edge = sw - 4;

    /* 左栏: 统一 os_pane 模板 (文字 + 水管分割线 + 选中框), 与游戏/密码机一致 */
    os_pane_draw_left(lcd, &s_bp);

    /* 右栏: 设置模式走自定义渲染 */
    if (s_bp.settings_mode) {
        int off2 = s_bp.item_off; if (off2 < 0) off2 = 0;
        int top2 = off2 / SL_ROW_H, mod2 = off2 % SL_ROW_H;
        if (s_bp.settings_render)
            s_bp.settings_render(ctx, &s_bp, rx0, right_edge, list_y, list_bot, top2, mod2);
        return;
    }
    /* 右栏: 内容, 选中整行反色, 平滑滚动 */
    int view_h = list_bot - list_y;
    int max_vis = view_h / SL_ROW_H;
    if (max_vis < 1) max_vis = 1;
    int off = s_bp.item_off; if (off < 0) off = 0;
    int top_row = off / SL_ROW_H, off_mod = off % SL_ROW_H;
    int n_vis = (s_bp.item_count - top_row) > (max_vis + 1) ? (max_vis + 1) : (s_bp.item_count - top_row);
    for (int i = 0; i < n_vis; i++) {
        int idx = top_row + i;
        bool sel = (s_bp.focus == 1 && idx == s_bp.sel_item);
        int py = list_y + i * SL_ROW_H - off_mod;
        if (sel && py + SL_ROW_H - 2 >= list_y && py < list_bot) {
            int fy = py < list_y ? list_y : py;
            int ty = py + SL_ROW_H - 2; if (ty > list_bot - 1) ty = list_bot - 1;
            fill_rect(lcd, rx0, fy, right_edge, ty, ST7305_COLOR_BLACK);
        }
        if (py + SL_ROW_H < list_y || py >= list_bot) continue;
        int tw = text_width(s_bp.items[idx]);
        int tx = rx0 + ((right_edge - rx0) - tw) / 2;
        if (tx < rx0) tx = rx0;
        draw_text(lcd, tx, py + 4, s_bp.items[idx], sel);
    }
    /* 收藏栏/最近阅读为空提示: 右栏内居中 */
    if (s_bp.fav_mode && s_bp.item_count == 0) {
        const char *tip = s_bp.fav_empty_tip ? s_bp.fav_empty_tip :
            "\xe9\x95\xbf\xe6\x8c\x89\xe7\xa1\xae\xe8\xae\xa4\xe6\xb8\xb8\xe6\x88\x8f\xe5\x8a\xa0\xe5\x85\xa5\xe6\x94\xb6\xe8\x97\x8f";
        int tyy = list_y + view_h / 2 - 12;
        int tw = text_width(tip);
        int tx = rx0 + ((right_edge - rx0) - tw) / 2; if (tx < rx0) tx = rx0;
        draw_text(lcd, tx, tyy, tip, false);
    }
    if (s_bp.recent_mode && s_bp.item_count == 0) {
        const char *tip = "\xe6\x9c\x80\xe8\xbf\x91\xe6\x97\xa0\xe9\x98\x85\xe8\xaf\xbb\xe8\xae\xb0\xe5\xbd\x95";
        int tyy = list_y + view_h / 2 - 12;
        int tw = text_width(tip);
        int tx = rx0 + ((right_edge - rx0) - tw) / 2; if (tx < rx0) tx = rx0;
        draw_text(lcd, tx, tyy, tip, false);
    }
}

/* 触摸命中 (物理坐标 → book_touch_map 映射为逻辑坐标再命中, 与渲染坐标系一致) */
static bool book_shelf_touch(ui_ctx_t *ctx, int x, int y) {
    book_touch_map(&x, &y);   /* 物理 → 逻辑 (自包含, 页内映射) */
    const int list_y = s_bp.list_y > 0 ? s_bp.list_y : 30;
    if (y < list_y || y >= ui_screen_h() - 4) return false;
    if (x < SL_LEFT_W) {
        int idx = s_bp.folder_scroll + (y - list_y) / SL_ROW_H;
        if (idx < book_left_total()) {
            if (s_bp.sel_folder != idx) {
                s_bp.sel_folder = idx;
                s_bp.focus = 0;
                os_pane_build_right(&s_bp);
            }
            ctx->needs_redraw = true;
            return true;
        }
        return false;
    }
    if (s_bp.settings_mode) {
        int idx = (s_bp.item_off + (y - list_y)) / SL_ROW_H;
        if (idx >= s_bp.settings_count) return false;
        s_bp.sel_item = idx;
        s_bp.focus = 1;
        ctx->needs_redraw = true;
        if (s_bp.on_settings_select) s_bp.on_settings_select(ctx, &s_bp, idx);
        return true;
    }
    int idx = (s_bp.item_off + (y - list_y)) / SL_ROW_H;
    if (idx >= s_bp.item_count) return false;
    s_bp.sel_item = idx;
    s_bp.focus = 1;
    ctx->needs_redraw = true;
    if (s_bp.on_select) s_bp.on_select(ctx, &s_bp, s_bp.item_paths[idx], s_bp.items[idx]);
    return true;
}

/* 每帧: 右栏触摸拖动滚动 (跟手 1:1; 坐标同样页内映射到逻辑坐标) */
static void book_shelf_drag(ui_ctx_t *ctx) {
    int tx, ty;
    static int s_last_y = -1, s_start_off = 0;
    static bool s_started_right = false;
    const int list_y = s_bp.list_y > 0 ? s_bp.list_y : 30;
    bool down = input_get_touch_pos(&tx, &ty);
    if (down) {
        book_touch_map(&tx, &ty);   /* 物理 → 逻辑 (页内自包含映射) */
        if (!s_bp.settings_mode && tx >= SL_LEFT_W && ty >= list_y && ty < ui_screen_h() - 4) {
            int hidx = (s_bp.item_off + (ty - list_y)) / SL_ROW_H;
            if (hidx >= 0 && hidx < s_bp.item_count)
                if (s_bp.sel_item != hidx || s_bp.focus != 1) {
                    s_bp.sel_item = hidx;
                    s_bp.focus = 1;
                    if (ctx) ctx->needs_redraw = true;
                }
        }
        if (s_last_y < 0) {
            bool in_right = (tx >= SL_LEFT_W && ty >= list_y && ty < ui_screen_h() - 4);
            s_started_right = in_right;
            s_last_y = ty;
            s_start_off = s_bp.item_off;
            return;
        }
        if (!s_started_right) return;
        int max_off = book_max_off();
        if (max_off < 0) max_off = 0;
        int new_off = s_start_off - (ty - s_last_y);   /* 1:1 跟手 */
        if (new_off < 0) new_off = 0;
        if (new_off > max_off) new_off = max_off;
        if (new_off != s_bp.item_off) {
            s_bp.item_off = new_off;
            if (ctx) ctx->needs_redraw = true;
        }
    } else {
        s_last_y = -1;
        s_start_off = 0;
        s_started_right = false;
    }
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
    } else {
        os_dialog_toast(ctx, "\xe6\x96\x87\xe4\xbb\xb6\xe6\x8d\x9f\xe5\x9d\x8f");   /* 文件损坏 */
    }
}

/* 最近阅读入口: 从 NVS 读最近打开的书 (TF 卡损坏/换卡后仍可回到最近一本) */
static void book_recent_load(void) {
    s_bp.recent_path[0] = 0;
    nvs_handle_t h;
    if (nvs_open(BNS, NVS_READONLY, &h) == ESP_OK) {
        size_t sz = sizeof(s_bp.recent_path);
        if (nvs_get_str(h, "last_path", s_bp.recent_path, &sz) != ESP_OK)
            s_bp.recent_path[0] = 0;
        nvs_close(h);
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
        /* 侧栏「最近阅读」: 设备级兜底, 右栏显示最近打开的书 */
        s_bp.recent_label = "\xe6\x9c\x80\xe8\xbf\x91\xe9\x98\x85\xe8\xaf\xbb"; /* 最近阅读 */
        /* 侧栏「收藏栏」: 长按书加入收藏, 与游戏收藏同机制 (TF 卡持久化) */
        s_bp.fav_enabled = true;
        s_bp.fav_engine = FAV_ENGINE_BOOK;
        s_bp.fav_empty_tip = "\xe9\x95\xbf\xe6\x8c\x89\xe7\xa1\xae\xe8\xae\xa4\xe4\xb9\xa6\xe5\x8a\xa0\xe5\x85\xa5\xe6\x94\xb6\xe8\x97\x8f"; /* 长按确认书加入收藏 */
        s_bp.list_y = 30;   /* 状态栏(24)下方 */
        bs_load();
        os_pane_reset(&s_bp);
    }
    book_recent_load();   /* 每次进入刷新最近阅读 (NVS 可能已被阅读器更新) */
    input_set_screen_rotation(0);   /* 书架触摸用物理坐标, 页内自映射 (防前一页残留旋转) */
    book_rotation_ensure(ctx);   /* 进入书架即按旋转方向启用竖屏 */
    os_pane_build_right(&s_bp);
}

static void p_book_exit(ui_ctx_t *ctx) {
    /* 从阅读器直接 HOME/退出离开: 先收尾阅读器 (释放内存+复位), 再复位触摸旋转 */
    if (s_reader_open) {
        book_reader_close();
        s_reader_open = false;
        ctx->fullscreen = false;
    }
    book_rotation_off(ctx);   /* 卸载书架旋转 (含 map_touch), 阅读器自管全局旋转 */
    input_set_screen_rotation(0);
}

static void p_book_render(ui_ctx_t *ctx) {
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return;
    if (s_reader_open && book_reader_is_open()) {
        /* 阅读器自己管理旋转 (自带 pfb/portrait_rotate), 书架竖屏模式让位 */
        book_rotation_off(ctx);   /* 阅读器接管: 卸载书架 st7305 旋转与映射 */
        book_reader_render(lcd);
        ctx->fullscreen = true;
        return;
    }
    if (s_reader_open) {
        /* 阅读器已通过内部菜单关闭 (如"返回书库") → 收尾并刷新最近阅读 */
        s_reader_open = false;
        ctx->fullscreen = false;
        bs_load();   /* 同步阅读器内旋转方向 (book_rotate_next 已写 NVS) */
        book_recent_load();
        s_bp.built_folder = -1;
        os_pane_build_right(&s_bp);
    }
    book_rotation_ensure(ctx);   /* 书架按旋转方向渲染 (竖屏画进 pfb, flush 时旋转) */
    ctx->fullscreen = false;
    st7305_clear(lcd, ST7305_COLOR_WHITE);
    book_shelf_render(ctx);   /* 自包含渲染 (逻辑坐标, 与阅读器同一坐标系) */
}

static void p_book_action(ui_ctx_t *ctx, os_action_t a) {
    if (s_reader_open) {
        if (a == OS_ACTION_BACK) {
            book_reader_close();
            s_reader_open = false;
            ctx->fullscreen = false;
            /* 刚读完的书 → 刷新「最近阅读」右栏 + 同步阅读器内旋转方向 */
            bs_load();
            book_recent_load();
            s_bp.built_folder = -1;   /* 强制重建 (recent_path 可能已变) */
            os_pane_build_right(&s_bp);
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
    return book_shelf_touch(ctx, x, y);   /* 自包含书架触摸 (页内映射逻辑坐标) */
}

static void p_book_poll(ui_ctx_t *ctx) {
    if (s_reader_open) {
        if (book_reader_poll()) ctx->needs_redraw = true;   /* 阅读器内: 目录/书签菜单拖动滚动 */
        return;
    }
    if (!s_reader_open) book_shelf_drag(ctx);   /* 自包含右栏拖动滚动 (页内映射) */
}

static const os_module_t s_mod_book = {
    .name       = "book",
    .page_id    = OS_PAGE_BOOK,
    .on_enter   = p_book_enter,
    .on_exit    = p_book_exit,
    .render     = p_book_render,
    .action     = p_book_action,
    .touch      = p_book_touch,
    .poll       = p_book_poll,
    .fullscreen = false,
};

void os_page_book_register(void) { os_register(&s_mod_book); }
