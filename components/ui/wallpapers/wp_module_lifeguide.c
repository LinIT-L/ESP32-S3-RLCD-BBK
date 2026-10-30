/* wp_module_lifeguide.c — 人生指南壁纸模块
 *
 * 把 Lua 应用《人生指南》的 676 条建议当作**屏保壁纸**显示：每分钟随机换一条。
 *
 * 数据来源：/store/apps/app/lifeguide.lua 里的
 *   local RAW = [[  id\x1f章节\x1f证据\x1f性价比\x1f标题\x1f白话\x1f花费\x1f文献数 \n ... ]]
 * 首次渲染时整份读进 PSRAM（约 370KB），并按行建索引；之后切条目只查索引，
 * 不再碰 flash（屏保期间尽量少碰 flash）。
 *
 * 与星空不同，这是**静态壁纸**：没到 60 秒就返回 0（不重绘、不 flush），
 * 避免每帧刷 15000B 白耗电。
 */
#include "wallpapers.h"
#include "ui_common.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define LG_TAG        "WPLG"
#define LG_PATH       "/store/apps/app/lifeguide.lua"
#define LG_SEP        '\x1f'
#define LG_MAX_ITEMS  1200
#define LG_SWAP_MS    60000u        /* 每条停留 1 分钟 */

#define LG_W  ST7305_WIDTH
#define LG_H  ST7305_HEIGHT
#define LG_PAD 18
#define LG_LH  22

static char     *s_data;            /* 数据段（PSRAM，就地按 \n 切） */
static uint32_t *s_off;             /* 每条起始偏移 */
static int       s_n;
static int       s_cur = -1;
static uint32_t  s_last_ms;
static bool      s_tried;

/* ---------------- UTF-8 宽度 ---------------- */

static int u8len(unsigned char c)
{
    if (c >= 0xF0) return 4;
    if (c >= 0xE0) return 3;
    if (c >= 0xC0) return 2;
    return 1;
}
static int chpx(const char *p) { return (u8len((unsigned char)*p) >= 3) ? 16 : 8; }

static int strw(const char *s)
{
    int w = 0;
    while (*s) { w += chpx(s); s += u8len((unsigned char)*s); }
    return w;
}

static void txt(st7305_handle_t *lcd, int x, int y, const char *s)
{
    if (!s || !*s) return;
    if (ui_text16_ready()) ui_text16(lcd, x, y, s, false);
    else                   draw_text(lcd, x, y, s, false);
}

/* 点阵渐隐线（视觉铁律：中段实线，两端隔点） */
static void hline_fade(st7305_handle_t *lcd, int x0, int x1, int y)
{
    int span = x1 - x0;
    int a = x0 + span / 4, b = x1 - span / 4;
    for (int x = a; x <= b; x++) st7305_draw_pixel(lcd, x, y, ST7305_COLOR_BLACK);
    for (int x = x0; x < a; x += 4) {
        st7305_draw_pixel(lcd, x, y, ST7305_COLOR_BLACK);
        st7305_draw_pixel(lcd, x + 1, y, ST7305_COLOR_BLACK);
    }
    for (int x = b; x <= x1; x += 4) {
        st7305_draw_pixel(lcd, x, y, ST7305_COLOR_BLACK);
        if (x + 1 <= x1) st7305_draw_pixel(lcd, x + 1, y, ST7305_COLOR_BLACK);
    }
}

/* 按像素宽把一行画到 (x,y)，返回用掉的显示宽度；超宽即止（不折行） */
static int __attribute__((unused)) draw_clip(st7305_handle_t *lcd, int x, int y, const char *s, int maxw)
{
    char buf[64];
    int n = 0, w = 0;
    while (*s) {
        int l = u8len((unsigned char)*s);
        int cw = chpx(s);
        if (w + cw > maxw) break;
        if (n + l >= (int)sizeof(buf)) break;
        memcpy(buf + n, s, (size_t)l);
        n += l;
        w += cw;
        s += l;
    }
    buf[n] = 0;
    txt(lcd, x, y, buf);
    return w;
}

/* 折行并逐行画，返回画了几行 */
static int draw_wrap(st7305_handle_t *lcd, int x, int y, const char *s, int maxw, int maxrows)
{
    int rows = 0;
    while (*s && rows < maxrows) {
        char buf[80];
        int n = 0, w = 0;
        while (*s) {
            int l = u8len((unsigned char)*s);
            int cw = chpx(s);
            if (w + cw > maxw) break;
            if (n + l >= (int)sizeof(buf)) break;
            memcpy(buf + n, s, (size_t)l);
            n += l; w += cw; s += l;
        }
        if (n == 0) break;
        buf[n] = 0;
        txt(lcd, x, y, buf);
        y += LG_LH; rows++;
    }
    return rows;
}

/* ---------------- 数据加载 ---------------- */

static bool lg_load(void)
{
    if (s_tried) return s_n > 0;
    s_tried = true;

    FILE *f = fopen(LG_PATH, "rb");
    if (!f) { ESP_LOGW(LG_TAG, "打不开 %s", LG_PATH); return false; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 4 * 1024 * 1024) { fclose(f); return false; }

    char *buf = heap_caps_malloc((size_t)sz + 1, MALLOC_CAP_SPIRAM);
    if (!buf) { fclose(f); ESP_LOGW(LG_TAG, "PSRAM 分配失败"); return false; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = 0;

    char *p = strstr(buf, "local RAW = [[");
    if (!p) { ESP_LOGW(LG_TAG, "数据段标记未找到"); heap_caps_free(buf); return false; }
    p += strlen("local RAW = [[");
    char *e = strstr(p, "]]");
    if (e) *e = 0;

    s_data = p;
    s_off  = heap_caps_malloc(sizeof(uint32_t) * LG_MAX_ITEMS, MALLOC_CAP_SPIRAM);
    if (!s_off) { heap_caps_free(buf); return false; }

    s_n = 0;
    s_off[s_n++] = 0;
    for (uint32_t i = 0; p[i]; i++)
        if (p[i] == '\n' && p[i + 1] && s_n < LG_MAX_ITEMS) s_off[s_n++] = i + 1;

    ESP_LOGI(LG_TAG, "人生指南壁纸: %d 条已载入", s_n);
    return s_n > 0;
}

/* ---------------- 绘制一条 ---------------- */

static void draw_item(st7305_handle_t *lcd, int idx)
{
    if (idx < 0 || idx >= s_n) return;
    char *line = s_data + s_off[idx];

    /* 就地切字段: id | 章节 | 证据 | 性价比 | 标题 | 白话 | 花费 | 文献 */
    char *fld[8] = { line, NULL };
    int fn = 1;
    for (char *q = line; *q && fn < 8; q++) {
        if (*q == LG_SEP) { *q = 0; fld[fn++] = q + 1; }
    }
    /* 行尾换行去掉（最后一段） */
    if (fn >= 1) {
        char *last = fld[fn - 1];
        size_t ll = strlen(last);
        while (ll && (last[ll - 1] == '\n' || last[ll - 1] == '\r')) last[--ll] = 0;
    }
    if (fn < 6) return;

    const char *sec   = fld[1];
    const char *grade = fld[2];
    const char *ratio = fld[3];
    const char *title = fld[4];
    const char *body  = fld[5];

    st7305_clear(lcd, ST7305_COLOR_WHITE);

    int inner = LG_W - LG_PAD * 2;
    int trows = (strw(title) + inner - 1) / inner;
    if (trows > 3) trows = 3;
    int brows = (strw(body) + inner - 1) / inner;
    if (brows > 6) brows = 6;

    int total = LG_LH + 8 + trows * LG_LH + 14 + brows * LG_LH + 26;
    int y = (LG_H - total) / 2;
    if (y < 14) y = 14;

    /* 分类名（居中） */
    txt(lcd, (LG_W - strw(sec)) / 2, y, sec);
    y += LG_LH + 8;

    /* 标题（居中，最多 3 行） */
    {
        const char *s = title;
        int rows = 0;
        while (*s && rows < 3) {
            char buf[80];
            int n = 0, w = 0;
            while (*s) {
                int l = u8len((unsigned char)*s);
                int cw = chpx(s);
                if (w + cw > inner) break;
                if (n + l >= (int)sizeof(buf)) break;
                memcpy(buf + n, s, (size_t)l);
                n += l; w += cw; s += l;
            }
            if (n == 0) break;
            buf[n] = 0;
            txt(lcd, (LG_W - w) / 2, y, buf);
            y += LG_LH; rows++;
        }
    }
    y += 6;

    hline_fade(lcd, LG_W / 2 - 40, LG_W / 2 + 40, y);
    y += 12;

    /* 白话正文 */
    int maxrows = (LG_H - 34 - y) / LG_LH;
    if (maxrows < 1) maxrows = 1;
    draw_wrap(lcd, LG_PAD, y, body, inner, maxrows < 3 ? maxrows : 3);

    /* 底部：证据 / 性价比 */
    char meta[64];
    snprintf(meta, sizeof(meta), "证据%s  性价比%s", grade ? grade : "", ratio ? ratio : "");
    int mw = strw(meta);
    if (mw > inner) { meta[16] = 0; mw = strw(meta); }
    txt(lcd, (LG_W - mw) / 2, LG_H - 24, meta);
}

/* ---------------- 模块入口 ---------------- */

static int render_lifeguide(st7305_handle_t *lcd, uint32_t now_ms)
{
    if (s_n == 0 && !lg_load()) {
        /* 数据文件不在(应用被卸载) ⇒ 退回星空, 不能让屏保停在白屏 */
        return wp_program_render(lcd, WP_PROG_STARS, now_ms);
    }

    /* 未到 60 秒：什么都不画（不重绘、不 flush，省电） */
    if (s_cur >= 0 && (uint32_t)(now_ms - s_last_ms) < LG_SWAP_MS) return 0;

    s_last_ms = now_ms;
    int i = rand() % s_n;
    if (s_n > 1 && i == s_cur) i = (i + 1) % s_n;
    s_cur = i;

    draw_item(lcd, i);
    return 1;
}

/* 退出屏保/换壁纸时清掉"当前条"，下次进入重新随机 */
static void lg_reset(void) { s_cur = -1; }

static bool s_reg;
void wp_lifeguide_register_module(void)
{
    if (s_reg) return;
    s_reg = true;
    static const wp_module_t mod = {
        .id     = WP_PROG_LIFEGUIDE,
        .name   = "\xe4\xba\xba\xe7\x94\x9f\xe6\x8c\x87\xe5\x8d\x97",   /* 人生指南 */
        .render = render_lifeguide,
    };
    wp_register_module(&mod);
    lg_reset();
}

/* 应用是否已安装 (壁纸列表据此决定是否显示这一项) */
bool wp_lifeguide_installed(void)
{
    FILE *f = fopen(LG_PATH, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}
