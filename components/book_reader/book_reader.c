/* 电子书阅读器 V3 (移植 libunibreak 排版内核)
 * - 排版内核: libunibreak 7.0 (Unicode UAX#14 断行, KOReader 同款), 中文避头尾
 * - 解码: UTF-8 / GBK / UTF-16 LE/BE (book_flow 内核, 固件与主机测试共用)
 * - 不再整本载入: 流式索引页偏移表 + 按页窗口渲染, 后台渐进索引, sidecar 秒开
 * - 进度/书签 (.pos): 断点续读 + 手动书签; 目录: 章节识别 (第X章/Chapter/Vol)
 * - 按键: 右/下/确认 = 下一页, 左/上 = 上一页, BACK/HOME = 退出,
 *         KEY 长按 (LONG_LEFT) = 阅读菜单 (目录/添加书签/书签列表/返回阅读)
 */
#include "book_reader.h"

/* V1.0.68: 输入层屏幕旋转 (触摸跟随旋转) */
extern void input_set_screen_rotation(int rot);

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "audio_player.h"
#include "font_book.h"
#include "book_flow.h"
#include "fb2_convert.h"
#include "epub_convert.h"

static const char *TAG = "BOOK";

/* 与 menu_system.h 的 menu_action_t 保持一致 (避免组件循环依赖) */
enum {
    BOOK_ACTION_NONE = 0,
    BOOK_ACTION_UP = 1,
    BOOK_ACTION_DOWN = 2,
    BOOK_ACTION_LEFT = 3,
    BOOK_ACTION_RIGHT = 4,
    BOOK_ACTION_CONFIRM = 5,
    BOOK_ACTION_BACK = 6,
    BOOK_ACTION_HOME = 7,
    BOOK_ACTION_LONG_LEFT = 8,
};

/* 上限 (防病态文件) */
#define BOOK_MAX_FILE       (512u * 1024 * 1024)   /* 单本上限 512MB */
#define BOOK_MAX_PAGES      (262144u)              /* 页表上限 1MB PSRAM */
#define BOOK_MAX_CHAPTERS   3000u
#define BOOK_MAX_BOOKMARKS  8u
#define BOOK_WIN_SIZE       (64 * 1024)            /* 渲染窗口 */
#define BOOK_SCAN_CHUNK     (32 * 1024)            /* 索引读缓冲 */
#define BOOK_SCAN_STEP      (256u * 1024)          /* 后台任务每步扫描量 */
#define BOOK_FIRST_CHUNK    (48u * 1024)           /* 打开时同步索引量 (提速进入) */

#define BOOK_CONTENT_X0   8
#define BOOK_CONTENT_Y0   2                    /* 全屏: 无标题栏 */
/* 竖屏布局 (旋转 90/270): 逻辑 300 宽 x 400 高, 17 列 x 22 行 */
#define BOOK_PORTRAIT_W      300
#define BOOK_PORTRAIT_H      400

#define FLOW_SCRATCH_SIZE   (BF_MAX_WIN * 8 + 8)

/* === 阅读器状态 === */
static bool         s_open = false;
static FILE        *s_fp = NULL;          /* 渲染句柄 (整个阅读期间保持) */
static char         s_path[256] = {0};
static char         s_src_path[320] = {0}; /* 实际阅读源 (FB2 时为转换缓存) */
static uint32_t     s_file_size = 0;
static uint32_t     s_src_size = 0;         /* 实际阅读源字节数 (FB2 为缓存) */
static uint32_t     s_mtime = 0;
static uint32_t     s_file_start = 0;     /* BOM 跳过后的起始偏移 */
static uint8_t      s_enc = BF_ENC_UTF8;

static uint32_t    *s_page_off = NULL;    /* PSRAM 页偏移表 */
static uint32_t     s_page_cap = 0;
static uint32_t     s_page_count = 0;     /* 已索引页数 (锁保护) */
static uint32_t     s_page = 0;
static uint32_t     s_indexed_bytes = 0;  /* 索引已扫描字节 (进度显示) */
static volatile bool s_index_done = false;
static volatile bool s_index_error = false;
static uint32_t     s_resume_off = 0;     /* 侧边索引断点续扫位置 */

static uint8_t     *s_win = NULL;         /* 渲染窗口 PSRAM */
static uint32_t     s_win_base = 0;
static uint32_t     s_win_len = 0;

/* 索引任务工作区 (与渲染工作区分离, 避免并发冲突) */
static uint8_t     *s_scan_chunk = NULL;
static bf_ch_t     *s_scan_win = NULL;
static uint8_t     *s_scan_brk = NULL;
static uint8_t     *s_scan_scratch = NULL;
static uint32_t    *s_scan_off = NULL;
/* 渲染工作区 */
static bf_ch_t     *s_rwin = NULL;
static uint8_t     *s_rbrk = NULL;
static uint8_t     *s_rscratch = NULL;

static char         s_title[64] = {0};

/* === 章节表 === */
typedef struct {
    uint32_t off;
    char     title[32];      /* 按文件原编码 (UTF-8 或 GBK) */
} book_chapter_t;
static book_chapter_t *s_chapters = NULL;
static uint32_t s_chapter_cap = 0;
static uint32_t s_chapter_count = 0;

/* === 书签 === */
typedef struct {
    uint32_t off;
    uint32_t page;
    char     title[32];
} book_bm_t;
static book_bm_t   s_bms[BOOK_MAX_BOOKMARKS];
static uint32_t    s_bm_count = 0;
static uint32_t    s_loaded_page = 0;     /* 断点续读页 */
static uint32_t    s_last_saved_page = 0; /* 上次落盘页 (每 5 页保存) */

/* === 索引任务同步 === */
static SemaphoreHandle_t s_idx_mutex = NULL;
static TaskHandle_t s_idx_task = NULL;
static volatile bool s_idx_stop = false;

/* === 阅读菜单 === */
enum { BM_READ = 0, BM_MENU = 1, BM_TOC = 2, BM_BMKS = 3 };
static uint8_t      s_menu = BM_READ;
static uint32_t     s_menu_sel = 0;
static uint32_t     s_menu_scroll = 0;
static char         s_menu_msg[40] = {0};
static bool         s_exit_confirm = false;   /* 返回菜单键退出确认 */

/* === 旋转/夜间 === */
static uint8_t      s_rot = 0;            /* 0上 1下 2左 3右 */
static bool         s_night = false;
static bool         s_pagenum = false;    /* 默认不显示右下角页码 */
static uint8_t      s_fontstyle = 0;       /* 0=仿宋 1=黑体(菜单字体) */
static uint8_t      s_fontsize = 1;       /* 字号 0=16 1=24 2=32 (默认 24) */
static uint8_t      s_margin_id = 1;      /* 0=窄 1=中 2=宽 */
static uint8_t      s_lineh_id = 1;       /* 0=紧凑 1=标准 2=宽松 */
static uint8_t      s_gap_id = 0;         /* 0=标准 1=宽松 */
static bool         s_inverted = false;
static st7305_handle_t *s_lcd = NULL;
static uint8_t     *s_rot_buf = NULL;     /* 180° 旋转输出备用缓冲 */
static uint8_t     *s_pfb = NULL;         /* 竖屏逻辑缓冲 (1bpp 行序) */

/* === 布局度量 (随旋转方向变化) === */
static inline bool book_is_portrait(void) {
    return (s_rot == 2 || s_rot == 3);
}
/* 边距 (字离边框距离) */
static inline int book_margin(void) {
    static const int m[3] = { 4, 8, 14 };
    return m[s_margin_id > 2 ? 1 : s_margin_id];
}
/* 行高 = 字高 + 行距 */
static inline int book_line_h(void) {
    static const int e[3] = { 0, 4, 8 };
    return font_book_cell_h() + e[s_lineh_id > 2 ? 1 : s_lineh_id];
}
/* 字间距 */
static inline int book_gap(void) {
    return s_gap_id ? 2 : 0;
}
static inline int book_line_max(void) {
    if (book_is_portrait()) {
        int cw = font_book_cell_w();
        if (cw < 8) cw = 16;
        return (BOOK_PORTRAIT_W - 2 * book_margin()) / cw * cw;
    }
    return ST7305_WIDTH - 2 * book_margin();
}
static inline int book_rows(void) {
    int lh = book_line_h();
    if (lh < 12) lh = 16;
    if (book_is_portrait()) return (BOOK_PORTRAIT_H - 2 * book_margin()) / lh;
    return (ST7305_HEIGHT - 2 * book_margin()) / lh;
}

/* === 竖屏逻辑缓冲 (1bpp, 位=1 黑字, 行序, 行宽 40 字节) === */
#define PFB_ROW_BYTES 40
static void pfb_clear(uint8_t *fb) {
    memset(fb, 0, PFB_ROW_BYTES * BOOK_PORTRAIT_H);
}
static void pfb_px(uint8_t *fb, int x, int y, int v) {
    if (x < 0 || x >= BOOK_PORTRAIT_W || y < 0 || y >= BOOK_PORTRAIT_H) return;
    uint8_t *p = &fb[(size_t)y * PFB_ROW_BYTES + (size_t)(x >> 3)];
    uint8_t m = (uint8_t)(1u << (7 - (x & 7)));
    if (v) *p |= m; else *p &= (uint8_t)~m;
}
static int pfb_get(const uint8_t *fb, int x, int y) {
    return (int)((fb[(size_t)y * PFB_ROW_BYTES + (size_t)(x >> 3)] >> (7 - (x & 7))) & 1u);
}
static void pfb_blit(uint8_t *fb, int x, int y, const book_glyph_t *g) {
    if (!g || !g->bitmap) return;
    int row_bytes = (g->w + 7) / 8;
    for (int row = 0; row < g->h; row++) {
        const uint8_t *src = g->bitmap + row * row_bytes;
        for (int col = 0; col < g->w; col++) {
            if (src[col >> 3] & (1u << (7 - (col & 7)))) {
                pfb_px(fb, x + col, y + row, 1);
            }
        }
    }
}

/* 写 ST7305 横屏帧缓冲像素 (与 st7305_draw_pixel 同格式, bit=1 白) */
static inline void fb_set_px_landscape(uint8_t *fb, int x, int y, int black) {
    if (x < 0 || x >= ST7305_WIDTH || y < 0 || y >= ST7305_HEIGHT) return;
    int inv_y = ST7305_HEIGHT - 1 - y;
    uint32_t idx = (uint32_t)(x >> 1) * (ST7305_HEIGHT >> 2) + (uint32_t)(inv_y >> 2);
    uint8_t bit = 7u - (uint8_t)(((inv_y & 3) << 1) | (x & 1));
    if (black) fb[idx] &= (uint8_t)~(1u << bit);
    else       fb[idx] |= (uint8_t)(1u << bit);
}

/* ============ 绘图工具 ============ */

static void draw_missing_glyph(st7305_handle_t *lcd, int x, int y) {
    int cw = font_book_cell_w(), chh = book_line_h();
    for (int i = 0; i < cw; i++) {
        st7305_draw_pixel(lcd, x + i, y, ST7305_COLOR_BLACK);
        st7305_draw_pixel(lcd, x + i, y + chh - 1, ST7305_COLOR_BLACK);
    }
    for (int i = 0; i < chh; i++) {
        st7305_draw_pixel(lcd, x, y + i, ST7305_COLOR_BLACK);
        st7305_draw_pixel(lcd, x + cw - 1, y + i, ST7305_COLOR_BLACK);
    }
}

static inline int ch_width(const bf_ch_t *ch) {
    return ((ch->kind == BF_CH_ASCII) ? font_book_ascii_w() : font_book_cell_w()) + book_gap();
}

/* 空白/不可见/格式控制码点: 无条件跳过, 绝不画方框
 * (字库里 U+FEFF/U+200B/U+202F 等字形在部分字体里是"空心方块",
 *  只靠"字形缺失才跳过"不够, 必须无条件不画) */
static bool is_ws_cp(uint32_t cp) {
    if (cp == 0x0020 || cp == 0x00A0 || cp == 0x1680 || cp == 0x202F ||
        cp == 0x205F || cp == 0x3000 || cp == 0xFEFF || cp == 0xFFFD ||
        cp == 0xFFFE || cp == 0xFFFF || cp == 0x00AD || cp == 0x180E ||
        cp == 0x200B || cp == 0x2028 || cp == 0x2029) return true;
    if (cp >= 0x2000 && cp <= 0x200F) return true;   /* 各宽度空格 + 零宽 + LRM/RLM */
    if (cp >= 0x202A && cp <= 0x202E) return true;   /* 双向文本控制 */
    if (cp >= 0x2060 && cp <= 0x206F) return true;   /* 词连字符/不可见操作符 */
    if (cp >= 0xFE00 && cp <= 0xFE0F) return true;   /* 变体选择符 */
    if (cp == 0xE0001) return true;
    if (cp >= 0xE0020 && cp <= 0xE007F) return true; /* 标签字符 */
    return false;
}

/* 绘制一个解码后的字符 (utf8/utf16 模式用 cp; gbk 模式用 hi/lo), 返回像素宽 */
static int draw_decoded_char(st7305_handle_t *lcd, int x, int y,
                             const bf_ch_t *ch) {
    int w = ch_width(ch);
    /* 换行/控制符: 不占像素 */
    if (ch->kind == BF_CH_NEWLINE || ch->kind == BF_CH_SKIP) return 0;
    /* 空白/不可见字符: 无条件留白 (utf8/utf16 模式) */
    if (s_enc != BF_ENC_GBK && is_ws_cp(ch->cp)) return w;
    /* GBK 全角空格 A1A1: 即使字库缺失也留白 */
    if (s_enc == BF_ENC_GBK && ch->kind == BF_CH_CJK &&
        ch->hi == 0xA1 && ch->lo == 0xA1) return w;
    book_glyph_t g;
    bool ok = false;
    if (s_enc == BF_ENC_GBK) {
        if (ch->kind == BF_CH_ASCII) {
            ok = font_book_glyph_ascii((uint8_t)ch->cp, &g);
        } else {
            ok = font_book_glyph_gb(ch->hi, ch->lo, &g);
        }
    } else if (ch->cp < 0x80) {
        ok = font_book_glyph_ascii((uint8_t)ch->cp, &g);
    } else {
        ok = font_book_glyph_unicode(ch->cp, &g);
    }
    if (ok && g.bitmap) {
        st7305_blit_1bit(lcd, x, y, g.w, g.h, g.bitmap);
    } else {
        draw_missing_glyph(lcd, x, y);
    }
    return w;
}

/* ============ 索引表 ============ */

static int idx_pages_append(uint32_t off) {
    if (s_idx_mutex) xSemaphoreTake(s_idx_mutex, portMAX_DELAY);
    if (s_page_count >= BOOK_MAX_PAGES) { if (s_idx_mutex) xSemaphoreGive(s_idx_mutex); return -1; }
    if (s_page_count == s_page_cap) {
        uint32_t nc = s_page_cap ? s_page_cap * 2 : 4096;
        if (nc > BOOK_MAX_PAGES) nc = BOOK_MAX_PAGES;
        uint32_t *np = heap_caps_realloc(s_page_off, (size_t)nc * sizeof(uint32_t),
                                         MALLOC_CAP_SPIRAM);
        if (!np) { if (s_idx_mutex) xSemaphoreGive(s_idx_mutex); return -1; }
        s_page_off = np;
        s_page_cap = nc;
    }
    s_page_off[s_page_count++] = off;
    if (s_idx_mutex) xSemaphoreGive(s_idx_mutex);
    return 0;
}

static int idx_chapters_append(uint32_t off, const uint8_t *title, uint8_t tlen) {
    if (s_idx_mutex) xSemaphoreTake(s_idx_mutex, portMAX_DELAY);
    if (s_chapter_count >= BOOK_MAX_CHAPTERS) { if (s_idx_mutex) xSemaphoreGive(s_idx_mutex); return 0; }
    /* 截断回退: 避免标题末尾是半个 UTF-8/GBK 字符 */
    if (s_enc == BF_ENC_GBK) {
        if (tlen > 0 && title[tlen - 1] >= 0x81) tlen--;
    } else {
        while (tlen > 0 && (title[tlen - 1] & 0xC0) == 0x80) tlen--;
        if (tlen > 0 && (title[tlen - 1] & 0xE0) == 0xC0) tlen--;
        else if (tlen > 1 && (title[tlen - 1] & 0xF0) == 0xE0) tlen--;
        else if (tlen > 2 && (title[tlen - 1] & 0xF8) == 0xF0) tlen--;
    }
    if (tlen > 31) tlen = 31;
    if (s_chapter_count == s_chapter_cap) {
        uint32_t nc = s_chapter_cap ? s_chapter_cap * 2 : 128;
        if (nc > BOOK_MAX_CHAPTERS) nc = BOOK_MAX_CHAPTERS;
        book_chapter_t *np = heap_caps_realloc(s_chapters, (size_t)nc * sizeof(book_chapter_t),
                                               MALLOC_CAP_SPIRAM);
        if (!np) { if (s_idx_mutex) xSemaphoreGive(s_idx_mutex); return -1; }
        s_chapters = np;
        s_chapter_cap = nc;
    }
    book_chapter_t *c = &s_chapters[s_chapter_count++];
    c->off = off;
    memcpy(c->title, title, tlen);
    c->title[tlen] = 0;
    if (s_idx_mutex) xSemaphoreGive(s_idx_mutex);
    return 0;
}

/* ---- 章节标题匹配 ---- */

static bool utf8_num_word(const uint8_t *p) {
    static const uint16_t words[] = {
        0x96B6, 0x4E00, 0x4E8C, 0x4E09, 0x56DB, 0x4E94, 0x516D, 0x4E03,
        0x516B, 0x4E5D, 0x5341, 0x767E, 0x5343, 0x4E07, 0x4E24
    };
    uint32_t cp = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        if (cp == words[i]) return true;
    }
    return false;
}

static bool gbk_num_word(const uint8_t *p) {
    static const uint16_t words[] = {
        0xC1E3, 0xD2BB, 0xB6FE, 0xC8FD, 0xCBC4, 0xCEE5, 0xC1F9, 0xC6DF,
        0xB0CB, 0xBEC5, 0xCAAE, 0xB0D9, 0xC7A7, 0xCDF2, 0xC1BD
    };
    uint16_t w = (uint16_t)((p[0] << 8) | p[1]);
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        if (w == words[i]) return true;
    }
    return false;
}

static bool utf8_suffix(const uint8_t *p) {
    static const uint32_t suf[] = {
        0xE7ABA0, 0xE59B9E, 0xE88A82, 0xE58DB7, 0xE983A8, 0xE8AF9D, 0xE99B86
    };
    uint32_t cp = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
    for (size_t i = 0; i < sizeof(suf) / sizeof(suf[0]); i++) {
        if (cp == suf[i]) return true;
    }
    return false;
}

static bool gbk_suffix(const uint8_t *p) {
    static const uint16_t suf[] = {
        0xB5C2, 0xBBD8, 0xBDDA, 0xBEED, 0xB2BF, 0xBBB0, 0xBCAF
    };
    uint16_t w = (uint16_t)((p[0] << 8) | p[1]);
    for (size_t i = 0; i < sizeof(suf) / sizeof(suf[0]); i++) {
        if (w == suf[i]) return true;
    }
    return false;
}

static bool ascii_prefix_cmp(const uint8_t *p, int n, const char *word) {
    int wl = (int)strlen(word);
    if (n < wl) return false;
    for (int k = 0; k < wl; k++) {
        char a = (char)p[k];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (a != word[k]) return false;
    }
    return true;
}

/* 判断 line[0..len) 是否为章节标题行 (按原编码匹配) */
static bool toc_match(const uint8_t *s, int len, bool gbk) {
    int i = 0;
    while (i < len) {
        if (s[i] == ' ' || s[i] == '\t' || s[i] == '\r') { i++; continue; }
        if (!gbk && i + 2 < len && s[i] == 0xEF && s[i + 1] == 0xBC && s[i + 2] == 0x80) {
            i += 3; continue;
        }
        break;
    }
    if (i >= len) return false;
    const uint8_t *p = s + i;
    int n = len - i;

    if (gbk) {
        if (n >= 2 && p[0] == 0xB5 && p[1] == 0xDA) {
            int j = 2, nd = 0;
            while (j < n) {
                if (p[j] >= '0' && p[j] <= '9') { j++; nd++; }
                else if (j + 1 < n && gbk_num_word(p + j)) { j += 2; nd++; }
                else break;
            }
            if (nd > 0 && j + 1 < n && gbk_suffix(p + j)) return true;
        }
    } else {
        if (n >= 3 && p[0] == 0xE7 && p[1] == 0xAC && p[2] == 0xAC) {
            int j = 3, nd = 0;
            while (j < n) {
                if (p[j] >= '0' && p[j] <= '9') { j++; nd++; }
                else if (j + 2 < n && utf8_num_word(p + j)) { j += 3; nd++; }
                else break;
            }
            if (nd > 0 && j + 2 < n && utf8_suffix(p + j)) return true;
        }
    }
    const char *prefixes[] = { "chapter", "chap", "volume", "vol", "section", "sec" };
    for (size_t pi = 0; pi < sizeof(prefixes) / sizeof(prefixes[0]); pi++) {
        if (!ascii_prefix_cmp(p, n, prefixes[pi])) continue;
        int j = (int)strlen(prefixes[pi]);
        while (j < n && (p[j] == ' ' || p[j] == '\t' || p[j] == ':' || p[j] == '-' ||
                         p[j] == '.')) j++;
        if (j < n && p[j] >= '0' && p[j] <= '9') return true;
        break;
    }
    if (!gbk) {
        /* 俄语章节: Глава / Часть + 数字或罗马数字 (参考 Porfiry 项目) */
        static const uint8_t glava[] = { 0xD0,0x93,0xD0,0xBB,0xD0,0xB0,0xD0,0xB2,0xD0,0xB0 };
        static const uint8_t chast[] = { 0xD0,0xA7,0xD0,0xB0,0xD1,0x81,0xD1,0x82,0xD1,0x8C };
        const uint8_t *words[] = { glava, chast };
        const int wlens[] = { 10, 8 };
        for (int wi = 0; wi < 2; wi++) {
            if (n >= wlens[wi] && memcmp(p, words[wi], wlens[wi]) == 0) {
                int j = wlens[wi];
                while (j < n && (p[j] == ' ' || p[j] == '\t')) j++;
                if (j < n &&
                    ((p[j] >= '0' && p[j] <= '9') || strchr("IVXLCDM", p[j]))) {
                    return true;
                }
            }
        }
    }
    return false;
}

/* ---- 扫描上下文 ---- */

typedef struct {
    FILE   *fp;
    uint32_t fsz;
    uint32_t cur;
    bool    resume;         /* 续扫: 起始页已记录, 不再重复 */
    bool    at_line_start;  /* 章节行跟踪 */
    uint32_t line_off;
    uint8_t line[40];
    uint8_t line_len;
} idx_ctx_t;

static idx_ctx_t s_idx;

static size_t scan_read_at(void *ud, uint32_t pos, uint8_t *buf, size_t want) {
    idx_ctx_t *c = (idx_ctx_t *)ud;
    if (!c->fp || pos >= c->fsz) return 0;
    /* V1.0.68 fix: 退出阅读器时立即中断索引扫描.
     * 返回 0 (=EOF) 让 bf_paginate 提前结束当前步, 索引任务随即退出,
     * book_reader_close 不再干等 1-2 秒 (旧: 256KB/步扫完才停). */
    if (s_idx_stop) return 0;
    uint32_t w = (uint32_t)want;
    if (w > c->fsz - pos) w = c->fsz - pos;
    if (fseek(c->fp, (long)pos, SEEK_SET) != 0) return 0;
    return fread(buf, 1, w, c->fp);
}

static int scan_add_page(void *ud, uint32_t off) {
    (void)ud;
    return idx_pages_append(off);
}

static void toc_track(idx_ctx_t *c, const bf_ch_t *ch, uint32_t pos) {
    if (c->at_line_start) {
        c->line_off = pos;
        c->line_len = 0;
        c->at_line_start = false;
    }
    if (ch->kind == BF_CH_NEWLINE) {
        if (c->line_len > 0) {
            uint8_t *s = c->line;
            int n = c->line_len;
            while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) n--;
            if (n > 0 && toc_match(s, n, s_enc == BF_ENC_GBK)) {
                idx_chapters_append(c->line_off, s, (uint8_t)n);
            }
        }
        c->at_line_start = true;
        return;
    }
    if (ch->kind == BF_CH_SKIP) return;
    if (c->line_len >= sizeof(c->line)) return;
    if (s_enc == BF_ENC_GBK) {
        if (ch->kind == BF_CH_CJK) {
            c->line[c->line_len++] = ch->hi;
            c->line[c->line_len++] = ch->lo;
        } else {
            c->line[c->line_len++] = (uint8_t)ch->cp;
        }
    } else {
        uint8_t tmp[4];
        int kn = bf_cp_to_utf8(ch->cp, tmp);
        for (int k = 0; k < kn && c->line_len < sizeof(c->line); k++) {
            c->line[c->line_len++] = tmp[k];
        }
    }
}

static void scan_on_char(void *ud, const bf_ch_t *ch, uint32_t pos) {
    toc_track((idx_ctx_t *)ud, ch, pos);
}

/* 从 c->cur 扫到 limit (阶段性上限), 更新 cur/page_open */
static int book_scan_run(idx_ctx_t *c, uint32_t limit) {
    if (c->cur >= limit || c->cur >= c->fsz) return 0;
    if (!s_scan_chunk || !s_scan_win || !s_scan_brk || !s_scan_scratch || !s_scan_off) return -1;
    bf_src_t src = { scan_read_at, c };
    uint32_t next;
    int r = bf_paginate(src, c->fsz, s_enc, c->cur,
                        book_rows(), book_line_max(),
                        font_book_ascii_w() + book_gap(), font_book_cell_w() + book_gap(),
                        scan_add_page, NULL, scan_on_char, c,
                        s_scan_chunk, BOOK_SCAN_CHUNK,
                        s_scan_win, s_scan_brk, s_scan_scratch, FLOW_SCRATCH_SIZE,
                        s_scan_off, limit, c->resume, &next);
    if (r != 0) return -1;
    c->cur = next;
    c->resume = false;
    return 0;
}

/* ============ 侧边索引 / 进度持久化 ============ */

typedef struct __attribute__((packed)) {
    char     magic[8];
    uint32_t fsz;
    uint32_t mtime;
    uint8_t  enc;
    uint8_t  layout;
    uint8_t  font_w;
    uint8_t  font_h;
    uint8_t  margin;
    uint8_t  lineh;
    uint8_t  gap;
    uint32_t indexed_bytes;
    uint32_t pages;
    uint32_t chapters;
} sidx_hdr_t;

typedef struct __attribute__((packed)) {
    char     magic[8];
    uint32_t fsz;
    uint32_t mtime;
    uint8_t  layout;
    uint8_t  bm_count;
    uint8_t  font_w;
    uint8_t  font_h;
    uint8_t  margin;
    uint8_t  lineh;
    uint8_t  gap;
    uint8_t  fontsize;
    uint32_t last_page;
    uint32_t last_off;   /* V1.0.68 fix: 当前页字节偏移 (布局/字体变化时按偏移恢复) */
} prog_hdr_t;

typedef struct __attribute__((packed)) {
    uint32_t off;
    uint32_t page;
    uint16_t tlen;
    char     title[32];
} prog_bm_t;

static uint32_t fnv1a(const char *s) {
    uint32_t h = 2166136261u;
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h;
}

static uint8_t book_layout_id(void) {
    return book_is_portrait() ? 1 : 0;
}

static void book_save_sidecar(void) {
    if (!s_path[0] || s_page_count == 0) return;
    char path[300];
    snprintf(path, sizeof(path), "%s.idx", s_path);
    FILE *f = fopen(path, "wb");
    if (!f) { ESP_LOGW(TAG, "侧边索引写入失败"); return; }
    if (s_idx_mutex) xSemaphoreTake(s_idx_mutex, portMAX_DELAY);
    sidx_hdr_t h;
    memset(&h, 0, sizeof(h));
    memcpy(h.magic, "BBKIDX05", 8);
    h.fsz = s_file_size;
    h.mtime = s_mtime;
    h.enc = s_enc;
    h.layout = book_layout_id();
    h.font_w = (uint8_t)font_book_cell_w();
    h.font_h = (uint8_t)font_book_cell_h();
    h.margin = s_margin_id;
    h.lineh = s_lineh_id;
    h.gap = s_gap_id;
    h.indexed_bytes = s_indexed_bytes;
    h.pages = s_page_count;
    h.chapters = s_chapter_count;
    fwrite(&h, 1, sizeof(h), f);
    if (s_page_count) fwrite(s_page_off, 4, s_page_count, f);
    for (uint32_t i = 0; i < s_chapter_count; i++) {
        uint32_t off = s_chapters[i].off;
        uint16_t tl = (uint16_t)strlen(s_chapters[i].title);
        fwrite(&off, 4, 1, f);
        fwrite(&tl, 2, 1, f);
        fwrite(s_chapters[i].title, 1, tl, f);
    }
    if (s_idx_mutex) xSemaphoreGive(s_idx_mutex);
    fclose(f);
    ESP_LOGI(TAG, "侧边索引已保存: %s (%lu 页, %lu 章)", path,
             (unsigned long)s_page_count, (unsigned long)s_chapter_count);
}

/* 返回 true 表示加载成功 (page_off/chapters 已就绪) */
static bool book_load_sidecar(void) {
    if (!s_path[0]) return false;
    char path[300];
    snprintf(path, sizeof(path), "%s.idx", s_path);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    sidx_hdr_t h;
    if (fread(&h, 1, sizeof(h), f) != sizeof(h) ||
        memcmp(h.magic, "BBKIDX05", 8) != 0 ||
        h.fsz != s_file_size || h.mtime != s_mtime || h.enc != s_enc ||
        h.layout != book_layout_id() ||
        h.font_w != (uint8_t)font_book_cell_w() ||
        h.font_h != (uint8_t)font_book_cell_h() ||
        h.margin != s_margin_id || h.lineh != s_lineh_id || h.gap != s_gap_id ||
        h.indexed_bytes > s_src_size ||
        h.pages == 0 ||
        h.pages > BOOK_MAX_PAGES || h.chapters > BOOK_MAX_CHAPTERS) {
        fclose(f);
        return false;
    }
    uint32_t *po = heap_caps_malloc((size_t)h.pages * 4, MALLOC_CAP_SPIRAM);
    if (!po) { fclose(f); return false; }
    if (fread(po, 4, h.pages, f) != h.pages) {
        free(po);
        fclose(f);
        return false;
    }
    book_chapter_t *ch = NULL;
    if (h.chapters) {
        ch = heap_caps_malloc((size_t)h.chapters * sizeof(book_chapter_t), MALLOC_CAP_SPIRAM);
        if (!ch) { free(po); fclose(f); return false; }
        for (uint32_t i = 0; i < h.chapters; i++) {
            uint32_t off;
            uint16_t tl;
            if (fread(&off, 4, 1, f) != 1 || fread(&tl, 2, 1, f) != 1 || tl > 31) {
                free(po); free(ch); fclose(f);
                return false;
            }
            if (fread(ch[i].title, 1, tl, f) != tl) {
                free(po); free(ch); fclose(f);
                return false;
            }
            ch[i].off = off;
            ch[i].title[tl] = 0;
        }
    }
    fclose(f);
    if (s_idx_mutex) xSemaphoreTake(s_idx_mutex, portMAX_DELAY);
    if (s_page_off) free(s_page_off);
    if (s_chapters) free(s_chapters);
    s_page_off = po;
    s_page_cap = h.pages;
    s_page_count = h.pages;
    s_chapters = ch;
    s_chapter_cap = h.chapters;
    s_chapter_count = h.chapters;
    s_indexed_bytes = h.indexed_bytes;
    s_index_done = (h.indexed_bytes >= s_src_size);
    s_resume_off = s_index_done ? 0 : s_indexed_bytes;
    s_index_error = false;
    if (s_idx_mutex) xSemaphoreGive(s_idx_mutex);
    ESP_LOGI(TAG, "侧边索引加载: %s (%lu 页, %lu 章)", path,
             (unsigned long)s_page_count, (unsigned long)s_chapter_count);
    return true;
}

static void book_progress_path(char *out, size_t n) {
    snprintf(out, n, "/sdcard/books/.progress/bk_%08lx.pos",
             (unsigned long)fnv1a(s_path));
}

static void book_save_progress(void) {
    if (!s_path[0]) return;
    char dir[128], path[300];
    snprintf(dir, sizeof(dir), "/sdcard/books/.progress");
    mkdir(dir, 0755);
    book_progress_path(path, sizeof(path));
    FILE *f = fopen(path, "wb");
    if (!f) return;
    prog_hdr_t h;
    memset(&h, 0, sizeof(h));
    memcpy(h.magic, "BBKPOS04", 8);
    h.fsz = s_file_size;
    h.mtime = s_mtime;
    h.layout = book_layout_id();
    h.font_w = (uint8_t)font_book_cell_w();
    h.font_h = (uint8_t)font_book_cell_h();
    h.margin = s_margin_id;
    h.lineh = s_lineh_id;
    h.gap = s_gap_id;
    h.fontsize = s_fontsize;
    h.bm_count = (uint8_t)s_bm_count;
    h.last_page = s_page;
    h.last_off = (s_page_off && s_page < s_page_count) ? s_page_off[s_page] : 0;
    fwrite(&h, 1, sizeof(h), f);
    for (uint32_t i = 0; i < s_bm_count; i++) {
        prog_bm_t b;
        memset(&b, 0, sizeof(b));
        b.off = s_bms[i].off;
        b.page = s_bms[i].page;
        b.tlen = (uint16_t)strlen(s_bms[i].title);
        memcpy(b.title, s_bms[i].title, sizeof(b.title));
        fwrite(&b, 1, sizeof(b), f);
    }
    fclose(f);
    ESP_LOGI(TAG, "进度已保存: 页 %lu 书签 %lu", (unsigned long)s_page, (unsigned long)s_bm_count);
}

/* V1.0.68 fix: 布局变化时记录的字节偏移, 页表就绪后按偏移恢复最近页 */
static uint32_t s_pending_last_off = 0;
static bool     s_pending_layout_diff = false;

static void book_load_progress(void) {
    s_bm_count = 0;
    s_loaded_page = 0;
    s_pending_last_off = 0;
    s_pending_layout_diff = false;
    char dir[128], path[300];
    snprintf(dir, sizeof(dir), "/sdcard/books/.progress");
    mkdir(dir, 0755);
    book_progress_path(path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) return;
    prog_hdr_t h;
    if (fread(&h, 1, sizeof(h), f) == sizeof(h) &&
        memcmp(h.magic, "BBKPOS04", 8) == 0 &&
        h.fsz == s_file_size && h.mtime == s_mtime) {
        /* 布局/字体一致 → 直接恢复页号; 不一致 → 记录字节偏移, 页表就绪后换算 */
        bool layout_match =
            h.layout == book_layout_id() &&
            h.font_w == (uint8_t)font_book_cell_w() &&
            h.font_h == (uint8_t)font_book_cell_h() &&
            h.margin == s_margin_id && h.lineh == s_lineh_id &&
            h.gap == s_gap_id && h.fontsize == s_fontsize;
        if (layout_match) {
            s_loaded_page = h.last_page;
            ESP_LOGI(TAG, "进度已加载: 页 %lu (布局一致)", (unsigned long)s_loaded_page);
        } else if (h.last_off > 0) {
            s_pending_last_off = h.last_off;
            s_pending_layout_diff = true;
            ESP_LOGI(TAG, "布局已变化, 待按字节偏移 %lu 恢复", (unsigned long)h.last_off);
        }
        /* 书签仅在布局一致时保留 (页号/偏移才对应) */
        if (layout_match && h.bm_count <= BOOK_MAX_BOOKMARKS) {
            s_bm_count = h.bm_count;
            for (uint32_t i = 0; i < s_bm_count; i++) {
                prog_bm_t b;
                memset(&b, 0, sizeof(b));
                if (fread(&b, 1, sizeof(b), f) != sizeof(b)) { s_bm_count = i; break; }
                if (b.tlen > 31) b.tlen = 31;
                s_bms[i].off = b.off;
                s_bms[i].page = b.page;
                memcpy(s_bms[i].title, b.title, 32);
                s_bms[i].title[31] = 0;
            }
            ESP_LOGI(TAG, "进度已加载: 页 %lu 书签 %lu", (unsigned long)s_loaded_page, (unsigned long)s_bm_count);
        }
    }
    fclose(f);
}

/* ============ 索引任务 ============ */

static void book_index_task(void *arg) {
    (void)arg;
    FILE *fp = fopen(s_path, "rb");
    if (!fp) {
        s_index_error = true;
        s_idx_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    idx_ctx_t c = s_idx;
    c.fp = fp;
    int64_t step_t0 = esp_timer_get_time();
    int step_count = 0;
    while (!s_idx_stop && c.cur < c.fsz) {
        uint32_t lim = c.cur + BOOK_SCAN_STEP;
        if (lim > c.fsz) lim = c.fsz;
        if (book_scan_run(&c, lim) != 0) { s_index_error = true; break; }
        s_indexed_bytes = c.cur;
        step_count++;
        if ((step_count % 15) == 0) {
            /* 周期性部分落盘: 中途关闭也不丢进度 */
            book_save_sidecar();
        }
        int64_t step_dt = esp_timer_get_time() - step_t0;
        if (step_dt > 150000) {
            ESP_LOGW(TAG, "索引步 %lu->%lu 耗时 %lld ms", (unsigned long)(lim - BOOK_SCAN_STEP),
                     (unsigned long)lim, (long long)step_dt / 1000);
        }
        step_t0 = esp_timer_get_time();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    fclose(fp);
    if (!s_idx_stop && !s_index_error) {
        s_index_done = true;
        ESP_LOGI(TAG, "后台索引完成: %lu 页, %lu 章",
                 (unsigned long)s_page_count, (unsigned long)s_chapter_count);
        book_save_sidecar();
    }
    s_idx_task = NULL;
    vTaskDelete(NULL);
}

static bool book_index_stop(void) {
    if (!s_idx_task) return true;
    s_idx_stop = true;
    uint32_t t0 = xTaskGetTickCount();
    while (s_idx_task && (uint32_t)(xTaskGetTickCount() - t0) < pdMS_TO_TICKS(8000)) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_idx_task) {
        ESP_LOGE(TAG, "索引任务停止超时, 放弃释放以避免悬空访问");
        return false;
    }
    s_idx_stop = false;
    return true;
}

/* 初始化索引并开始 (同步扫首段 + 后台任务), 返回 0 成功 */
static int book_scan_start(void) {
    if (!s_scan_chunk || !s_scan_win || !s_scan_brk || !s_scan_scratch || !s_scan_off) return -1;
    memset(&s_idx, 0, sizeof(s_idx));
    s_idx.fp = s_fp;
    s_idx.fsz = s_src_size;
    s_idx.cur = (s_resume_off > s_file_start) ? s_resume_off : s_file_start;
    s_idx.resume = (s_resume_off > s_file_start);
    s_idx.at_line_start = true;
    uint32_t first = s_file_start + BOOK_FIRST_CHUNK;
    if (first > s_src_size) first = s_src_size;
    if (book_scan_run(&s_idx, first) != 0) return -1;
    s_indexed_bytes = s_idx.cur;
    if (s_idx.cur >= s_src_size) {
        s_index_done = true;
        book_save_sidecar();
        return 0;
    }
    s_idx_stop = false;
    s_idx_task = NULL;
    if (xTaskCreate(book_index_task, "book_idx", 4096, NULL, 1, &s_idx_task) != pdPASS) {
        /* 后台任务创建失败 → 同步扫完 (慢但可用) */
        ESP_LOGW(TAG, "后台索引任务创建失败, 改为同步索引");
        while (s_idx.cur < s_src_size) {
            uint32_t lim = s_idx.cur + BOOK_SCAN_STEP;
            if (lim > s_src_size) lim = s_src_size;
            if (book_scan_run(&s_idx, lim) != 0) return -1;
            s_indexed_bytes = s_idx.cur;
        }
        s_index_done = true;
        book_save_sidecar();
    }
    return 0;
}

/* 等待索引覆盖到第 p 页 (超时/出错返回) */
static void book_wait_indexed_page(uint32_t p) {
    uint32_t t0 = xTaskGetTickCount();
    while (!s_index_done && !s_index_error && s_page_count <= p &&
           (uint32_t)(xTaskGetTickCount() - t0) < pdMS_TO_TICKS(800)) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* 旋转布局变化 → 重新分页 */
static void book_restart_index(void) {
    if (!book_index_stop()) {
        ESP_LOGE(TAG, "索引任务未停止, 取消重新分页");
        return;
    }
    if (s_idx_mutex) xSemaphoreTake(s_idx_mutex, portMAX_DELAY);
    if (s_page_off) { free(s_page_off); s_page_off = NULL; }
    if (s_chapters) { free(s_chapters); s_chapters = NULL; }
    s_page_cap = s_page_count = 0;
    s_chapter_cap = s_chapter_count = 0;
    s_index_done = false;
    s_index_error = false;
    s_indexed_bytes = 0;
    if (s_idx_mutex) xSemaphoreGive(s_idx_mutex);
    if (!book_load_sidecar()) {
        if (book_scan_start() != 0) {
            ESP_LOGE(TAG, "重新索引失败");
            book_reader_close();
            return;
        }
    }
    s_page = 0;
    s_menu = BM_READ;
}

/* ============ 渲染 ============ */

static bool book_load_win(uint32_t start, uint32_t need) {
    if (!s_win) return false;
    if (start >= s_win_base && start + need <= s_win_base + s_win_len) return true;
    if (start >= s_src_size || !s_fp) return false;
    uint32_t want = BOOK_WIN_SIZE;
    if (want > s_src_size - start) want = s_src_size - start;
    int64_t t0 = esp_timer_get_time();
    if (fseek(s_fp, (long)start, SEEK_SET) != 0) return false;
    size_t n = fread(s_win, 1, want, s_fp);
    int64_t dt = esp_timer_get_time() - t0;
    if (dt > 20000) {
        ESP_LOGW(TAG, "窗口读取 %u B 耗时 %lld ms (start=%u)", (unsigned)n, (long long)dt / 1000, start);
    }
    s_win_base = start;
    s_win_len = (uint32_t)n;
    return (start + need <= s_win_base + s_win_len);
}

/* 从渲染窗口解码当前页正文 (win_off = 当前页在窗口内的字节偏移)
 * 修复: 旧代码固定从 win[0] 解码, 窗口缓存命中时永远显示窗口第一页! */
static int render_decode_window(bf_ch_t *win, uint32_t win_off, int max_chars) {
    int n = 0;
    uint32_t pos = win_off;
    const uint8_t *pend = s_win + s_win_len;
    while (n < max_chars && pos < s_win_len) {
        bf_ch_t ch = bf_next_ch(s_win + pos, pend, s_enc);
        if (ch.adv == 0) break;
        win[n++] = ch;
        pos += ch.adv;
    }
    return n;
}

/* 两端对齐计算: 把本行剩余空间均匀分到字间距, 保证左右边距一致.
 * 以下行不拉伸 (左对齐): 段落末行 (含换行符/页底窗口边界)、单字行、
 * 填充率不足 2/3 的短行 (最后只剩两三个字时避免出现巨大字缝).
 * 输出 gaps/每间隙增量/余数. */
static void line_justify(const bf_ch_t *win, int start, int end, int line_max,
                         int *gaps, int *per_gap, int *rem, bool *justify) {
    int used = 0, nchars = 0;
    for (int j = start; j < end; j++) {
        const bf_ch_t *ch = &win[j];
        if (ch->kind == BF_CH_NEWLINE || ch->kind == BF_CH_SKIP) continue;
        int w = (ch->kind == BF_CH_ASCII) ? font_book_ascii_w() : font_book_cell_w();
        used += w + book_gap();
        nchars++;
    }
    *gaps = 0; *per_gap = 0; *rem = 0; *justify = false;
    if (nchars <= 1) return;
    bool last_of_para = (end > start) && (win[end - 1].kind == BF_CH_NEWLINE);
    if (last_of_para) return;
    if (used >= line_max) return;
    /* V1.0.68 fix: 少于 2/3 满的行不拉伸 (段末只剩两三个字/页底短行) */
    if (used * 3 < line_max * 2) return;
    int extra = line_max - used;
    *gaps = nchars - 1;
    *per_gap = extra / *gaps;
    *rem = extra % *gaps;
    *justify = true;
}

/* 横屏渲染 (旋转 上/下) */
static void render_landscape(st7305_handle_t *lcd) {
    if (!s_open || !lcd) return;
    int64_t rt0 = esp_timer_get_time();
    s_lcd = lcd;
    st7305_clear(lcd, ST7305_COLOR_WHITE);

    if (s_pagenum) {
        char page_str[32];
        if (!s_index_done) {
            uint32_t pct = s_src_size ? (s_indexed_bytes * 100 / s_src_size) : 100;
            snprintf(page_str, sizeof(page_str), "%lu/%lu %u%%",
                     (unsigned long)(s_page + 1), (unsigned long)s_page_count, (unsigned)pct);
        } else {
            snprintf(page_str, sizeof(page_str), "%lu/%lu",
                     (unsigned long)(s_page + 1), (unsigned long)s_page_count);
        }
        int digits = (int)strlen(page_str);
        int pw2 = digits * font_book_ascii_w();
        int px = ST7305_WIDTH - book_margin() - pw2;
        int py = ST7305_HEIGHT - font_book_ascii_h() - 2;
        for (int i = 0; page_str[i]; i++) {
            book_glyph_t g;
            if (font_book_glyph_ascii((uint8_t)page_str[i], &g)) {
                st7305_blit_1bit(lcd, px + i * font_book_ascii_w(), py, g.w, g.h, g.bitmap);
            }
        }
    }

    uint32_t start = 0, end = s_src_size;
    if (s_idx_mutex) xSemaphoreTake(s_idx_mutex, portMAX_DELAY);
    uint32_t pg = s_page;
    if (pg < s_page_count) start = s_page_off[pg];
    if (pg + 1 < s_page_count) end = s_page_off[pg + 1];
    if (s_idx_mutex) xSemaphoreGive(s_idx_mutex);
    uint32_t pagelen = end - start;
    if (!book_load_win(start, pagelen + 8)) return;
    if (!s_rwin || !s_rbrk || !s_rscratch) return;

    int n = render_decode_window(s_rwin, start - s_win_base, BF_MAX_WIN);
    if (n == 0) return;
    int64_t t_flow0 = esp_timer_get_time();
    bf_breaks(s_rwin, n, s_enc, s_rbrk, s_rscratch, FLOW_SCRATCH_SIZE);
    int line_end[BF_MAX_LINES], lc, boundary;
    bf_layout(s_rwin, n, s_rbrk, book_rows(), book_line_max(),
              font_book_ascii_w() + book_gap(), font_book_cell_w() + book_gap(),
              line_end, &lc, &boundary);
    int64_t t_flow1 = esp_timer_get_time();
    int prev = 0, y = book_margin();
    for (int k = 0; k < lc && k < book_rows(); k++) {
        int endk = line_end[k];
        int gaps, per_gap, rem;
        bool justify;
        line_justify(s_rwin, prev, endk, book_line_max(), &gaps, &per_gap, &rem, &justify);
        int x = 0, gi = 0;
        for (int j = prev; j < endk; j++) {
            const bf_ch_t *ch = &s_rwin[j];
            if (ch->kind == BF_CH_TAB) { x += font_book_cell_w() + book_gap(); gi++; continue; }
            if (ch->kind == BF_CH_NEWLINE || ch->kind == BF_CH_SKIP) continue;
            int w = ch_width(ch);
            draw_decoded_char(lcd, book_margin() + x, y, ch);
            x += w;
            if (justify && gi < gaps) x += per_gap + (gi < rem ? 1 : 0);
            gi++;
        }
        prev = endk;
        y += book_line_h();
    }
    int64_t t_draw1 = esp_timer_get_time();
    int64_t rt1 = esp_timer_get_time();
    if (rt1 - rt0 > 20000) {
        ESP_LOGW(TAG, "渲染耗时 %lld ms (flow=%lld 绘制=%lld)", (long long)(rt1 - rt0) / 1000,
                 (long long)(t_flow1 - t_flow0) / 1000, (long long)(t_draw1 - t_flow1) / 1000);
    }
}

/* 竖屏渲染 (旋转 左/右): 画进逻辑缓冲, 软件旋转映射回横屏帧缓冲 */
static void render_portrait(st7305_handle_t *lcd) {
    if (!s_open || !lcd || !s_pfb) return;
    s_lcd = lcd;
    uint8_t *fb = s_pfb;
    pfb_clear(fb);

    if (s_pagenum) {
        char page_str[32];
        if (!s_index_done) {
            uint32_t pct = s_src_size ? (s_indexed_bytes * 100 / s_src_size) : 100;
            snprintf(page_str, sizeof(page_str), "%lu/%lu %u%%",
                     (unsigned long)(s_page + 1), (unsigned long)s_page_count, (unsigned)pct);
        } else {
            snprintf(page_str, sizeof(page_str), "%lu/%lu",
                     (unsigned long)(s_page + 1), (unsigned long)s_page_count);
        }
        int pw = (int)strlen(page_str) * font_book_ascii_w();
        int px = BOOK_PORTRAIT_W - book_margin() - pw;
        int py = BOOK_PORTRAIT_H - font_book_ascii_h() - 2;
        for (int i = 0; page_str[i]; i++) {
            book_glyph_t g;
            if (font_book_glyph_ascii((uint8_t)page_str[i], &g)) {
                pfb_blit(fb, px + i * font_book_ascii_w(), py, &g);
            }
        }
    }

    uint32_t start = 0, end = s_src_size;
    if (s_idx_mutex) xSemaphoreTake(s_idx_mutex, portMAX_DELAY);
    uint32_t pg = s_page;
    if (pg < s_page_count) start = s_page_off[pg];
    if (pg + 1 < s_page_count) end = s_page_off[pg + 1];
    if (s_idx_mutex) xSemaphoreGive(s_idx_mutex);
    uint32_t pagelen = end - start;
    if (!book_load_win(start, pagelen + 8)) return;
    if (!s_rwin || !s_rbrk || !s_rscratch) return;

    int n = render_decode_window(s_rwin, start - s_win_base, BF_MAX_WIN);
    if (n == 0) return;
    bf_breaks(s_rwin, n, s_enc, s_rbrk, s_rscratch, FLOW_SCRATCH_SIZE);
    int line_end[BF_MAX_LINES], lc, boundary;
    bf_layout(s_rwin, n, s_rbrk, book_rows(), book_line_max(),
              font_book_ascii_w() + book_gap(), font_book_cell_w() + book_gap(),
              line_end, &lc, &boundary);
    int prev = 0, y = book_margin();
    for (int k = 0; k < lc && k < book_rows(); k++) {
        int endk = line_end[k];
        int gaps, per_gap, rem;
        bool justify;
        line_justify(s_rwin, prev, endk, book_line_max(), &gaps, &per_gap, &rem, &justify);
        int x = 0, gi = 0;
        for (int j = prev; j < endk; j++) {
            const bf_ch_t *ch = &s_rwin[j];
            if (ch->kind == BF_CH_TAB) { x += font_book_cell_w() + book_gap(); gi++; continue; }
            if (ch->kind == BF_CH_NEWLINE || ch->kind == BF_CH_SKIP) continue;
            int w = ch_width(ch);
            int yy = y;
            book_glyph_t g;
            bool ok;
            if (s_enc == BF_ENC_GBK) {
                ok = (ch->kind == BF_CH_ASCII) ? font_book_glyph_ascii((uint8_t)ch->cp, &g)
                                               : font_book_glyph_gb(ch->hi, ch->lo, &g);
            } else if (ch->cp < 0x80) {
                ok = font_book_glyph_ascii((uint8_t)ch->cp, &g);
            } else {
                ok = font_book_glyph_unicode(ch->cp, &g);
            }
            if ((s_enc != BF_ENC_GBK && is_ws_cp(ch->cp)) ||
                (s_enc == BF_ENC_GBK && ch->kind == BF_CH_CJK &&
                 ch->hi == 0xA1 && ch->lo == 0xA1)) {
                x += w;
                continue;
            }
            if (ok) pfb_blit(fb, book_margin() + x, yy, &g);
            else {
                for (int i = 0; i < 16; i++) {
                    pfb_px(fb, book_margin() + x, yy + i, 1);
                    pfb_px(fb, book_margin() + x + 15, yy + i, 1);
                    pfb_px(fb, book_margin() + x + i, yy, 1);
                    pfb_px(fb, book_margin() + x + i, yy + 15, 1);
                }
            }
            x += w;
            if (justify && gi < gaps) x += per_gap + (gi < rem ? 1 : 0);
            gi++;
        }
        prev = endk;
        y += book_line_h();
    }

}

/* 竖屏逻辑缓冲 -> 横屏帧缓冲 (纯软件旋转, 覆盖层绘制完成后调用) */
static void portrait_rotate(st7305_handle_t *lcd) {
    if (!s_open || !lcd || !s_pfb) return;
    uint8_t *fb = s_pfb;
    st7305_clear(lcd, ST7305_COLOR_WHITE);
    uint8_t *lfb = lcd->fb;
    for (int Y = 0; Y < BOOK_PORTRAIT_H; Y++) {
        for (int X = 0; X < BOOK_PORTRAIT_W; X++) {
            int black = pfb_get(fb, X, Y);
            int fx, fy;
            if (s_rot == 2) {
                fx = ST7305_WIDTH - 1 - Y;
                fy = X;
            } else {
                fx = Y;
                fy = ST7305_HEIGHT - 1 - X;
            }
            fb_set_px_landscape(lfb, fx, fy, black);
        }
    }
}

/* ============ 阅读菜单 ============ */

static const char *menu_str(const char *utf8, const char *gbk) {
    return (s_enc == BF_ENC_GBK) ? gbk : utf8;
}

#define M_READ_MENU   menu_str("\xE9\x98\x85\xE8\xAF\xBB\xE8\x8F\x9C\xE5\x8D\x95", "\xD4\xC4\xB6\xC1\xB2\xCB\xB5\xA5")
#define M_TOC         menu_str("\xE7\x9B\xAE\xE5\xBD\x95", "\xB7\xBF\xC2\xBC")
#define M_ADD_BM      menu_str("\xE6\xB7\xBB\xE5\x8A\xA0\xE4\xB9\xA6\xE7\xAD\xBE", "\xCC\xED\xBC\xD3\xCA\xE9\xC7\xA9")
#define M_BM_LIST     menu_str("\xE4\xB9\xA6\xE7\xAD\xBE\xE5\x88\x97\xE8\xA1\xA8", "\xCA\xE9\xC7\xA9\xC1\xD0\xB1\xED")
#define M_BACK_READ   menu_str("\xE8\xBF\x94\xE5\x9B\x9E\xE9\x98\x85\xE8\xAF\xBB", "\xB7\xB5\xBB\xD8\xD4\xC4\xB6\xC1")
#define M_BACK_LIB    menu_str("\xE8\xBF\x94\xE5\x9B\x9E\xE4\xB9\xA6\xE5\xBA\x93", "\xB7\xB5\xBB\xD8\xCA\xE9\xBF\xE2")  /* 返回书库 */
#define M_CONT_READ   menu_str("\xE7\xBB\xA7\xE7\xBB\xAD\xE9\x98\x85\xE8\xAF\xBB", "\xBC\xCD\xD0\xF8\xD4\xC4\xB6\xC1")  /* 继续阅读 */
#define M_BACK        menu_str("\xE8\xBF\x94\xE5\x9B\x9E", "\xB7\xB5\xBB\xD8")
#define M_CLEAR_BM    menu_str("\xE6\xB8\x85\xE7\xA9\xBA\xE4\xB9\xA6\xE7\xAD\xBE", "\xC7\xE5\xBF\xD5\xCA\xE9\xC7\xA9")
#define M_NO_TOC      menu_str("\xE6\x97\xA0\xE7\xAB\xA0\xE8\x8A\x82", "\xCE\xDE\xD5\xC2\xBD\xDA")
#define M_NO_BM       menu_str("\xE6\x97\xA0\xE4\xB9\xA6\xE7\xAD\xBE", "\xCE\xDE\xCA\xE9\xC7\xA9")
#define M_ADDED       menu_str("\xE5\xB7\xB2\xE6\xB7\xBB\xE5\x8A\xA0", "\xD2\xD1\xCC\xED\xBC\xD3")
#define M_BM_FULL     menu_str("\xE4\xB9\xA6\xE7\xAD\xBE\xE5\xB7\xB2\xE6\xBB\xA1", "\xCA\xE9\xC7\xA9\xD2\xD1\xC2\xFA")
#define M_CLEARED     menu_str("\xE5\xB7\xB2\xE6\xB8\x85\xE7\xA9\xBA", "\xD2\xD1\xC7\xE5\xBF\xD5")
#define M_EXIT_ASK    menu_str("\xE7\xA1\xAE\xE5\xAE\x9A\xE9\x80\x80\xE5\x87\xBA\xE9\x98\x85\xE8\xAF\xBB\xEF\xBC\x9F", "\xC8\xB7\xB6\xA8\xCD\xCB\xB3\xF6\xD4\xC4\xB6\xC1\xA3\xBF")
static void menu_fb_px(st7305_handle_t *lcd, int x, int y, bool black) {
    if (book_is_portrait() && s_pfb) {
        pfb_px(s_pfb, x, y, black ? 1 : 0);
    } else {
        fb_set_px_landscape(lcd->fb, x, y, black ? 1 : 0);
    }
}

/* 菜单文字 (按原编码绘制, invert=白字黑底) */
static void menu_draw_text(st7305_handle_t *lcd, int x, int y, const char *s, bool invert) {
    const uint8_t *p = (const uint8_t *)s;
    int cx = x;
    int x1 = (book_is_portrait() ? BOOK_PORTRAIT_W : ST7305_WIDTH) - 44;
    while (*p) {
        int w;
        book_glyph_t g;
        bool ok = false;
        if (s_enc == BF_ENC_GBK && p[0] >= 0x81 && p[1]) {
            ok = font_book_glyph_gb(p[0], p[1], &g);
            w = font_book_cell_w();
            p += 2;
        } else if (p[0] < 0x80) {
            ok = font_book_glyph_ascii(p[0], &g);
            w = font_book_ascii_w();
            p++;
        } else {
            const uint8_t *q = p;
            uint32_t cp = *q++;
            int extra = 0;
            if ((cp & 0xE0) == 0xC0) { extra = 1; cp &= 0x1F; }
            else if ((cp & 0xF0) == 0xE0) { extra = 2; cp &= 0x0F; }
            else if ((cp & 0xF8) == 0xF0) { extra = 3; cp &= 0x07; }
            else { p++; continue; }
            for (int k = 0; k < extra && *q; k++) {
                cp = (cp << 6) | (*q++ & 0x3F);
            }
            p = q;
            ok = font_book_glyph_unicode(cp, &g);
            w = font_book_cell_w();
        }
        if (cx + w > x1) break;
        if (ok && g.bitmap) {
            int rb = (g.w + 7) / 8;
            for (int row = 0; row < g.h; row++) {
                const uint8_t *src = g.bitmap + row * rb;
                for (int col = 0; col < g.w; col++) {
                    if (src[col >> 3] & (1u << (7 - (col & 7)))) {
                        menu_fb_px(lcd, cx + col, y + row, invert ? false : true);
                    }
                }
            }
        } else if (invert) {
            int cw = font_book_cell_w(), chh = font_book_cell_h();
            for (int i = 0; i < cw; i++) {
                menu_fb_px(lcd, cx + i, y, false);
                menu_fb_px(lcd, cx + i, y + chh - 1, false);
            }
            for (int i = 0; i < chh; i++) {
                menu_fb_px(lcd, cx, y + i, false);
                menu_fb_px(lcd, cx + cw - 1, y + i, false);
            }
        }
        cx += w;
    }
}

static int menu_item_count(void) {
    switch (s_menu) {
        case BM_MENU: return 4;
        case BM_TOC:  return (s_chapter_count == 0) ? 2 : (int)s_chapter_count + 1;
        case BM_BMKS: return (s_bm_count == 0) ? 2 : (int)s_bm_count + 2;
        default: return 0;
    }
}

static const char *menu_item_text(int i) {
    switch (s_menu) {
        case BM_MENU:
            /* V1.0.68: 添加书签 / 书签列表 / 返回书库 / 继续阅读 */
            switch (i) {
                case 0: return M_ADD_BM;
                case 1: return M_BM_LIST;
                case 2: return M_BACK_LIB;
                default: return M_CONT_READ;
            }
        case BM_TOC:
            if (s_chapter_count == 0) return (i == 0) ? M_NO_TOC : M_BACK;
            if (i < (int)s_chapter_count) return s_chapters[i].title;
            return M_BACK;
        case BM_BMKS:
            if (s_bm_count == 0) return (i == 0) ? M_NO_BM : M_BACK;
            if (i < (int)s_bm_count) return s_bms[i].title;
            if (i == (int)s_bm_count) return M_CLEAR_BM;
            return M_BACK;
        default: return "";
    }
}

static const char *menu_title_text(void) {
    switch (s_menu) {
        case BM_MENU: return "";   /* V1.0.68: 不显示"阅读菜单"标题 */
        case BM_TOC:  return M_TOC;
        case BM_BMKS: return M_BM_LIST;
        default: return "";
    }
}

/* 菜单文字宽度 (当前激活字号) */
static int menu_text_width(const char *s) {
    const uint8_t *p = (const uint8_t *)s;
    int w = 0;
    while (*p) {
        if (p[0] < 0x80) { w += font_book_ascii_w(); p++; }
        else if ((p[0] & 0xE0) == 0xC0) { w += font_book_cell_w(); p += 2; }
        else if ((p[0] & 0xF0) == 0xE0) { w += font_book_cell_w(); p += 3; }
        else if ((p[0] & 0xF8) == 0xF0) { w += font_book_cell_w(); p += 4; }
        else p++;
    }
    return w;
}

static void draw_reader_menu(st7305_handle_t *lcd) {
    int count = menu_item_count();
    if (count <= 0) return;
    int W = book_is_portrait() ? BOOK_PORTRAIT_W : ST7305_WIDTH;
    int H = book_is_portrait() ? BOOK_PORTRAIT_H : ST7305_HEIGHT;
    const int X0 = 28, X1 = W - 28;   /* V1.0.68: 更宽 */

    /* V1.0.68: 菜单用 32px 大字 + 大行距, 选项居中, 不显示标题.
     * 后台分页任务用的字号在 bf_paginate 调用时已作为参数快照, 临时切换字号不影响索引. */
    font_book_select(s_fontstyle, 2);   /* 最大字号 32px (书列表标题) */
    int chh = font_book_cell_h();
    const int row_step = chh + 14;    /* 选项间隔加大 */

    int vis = (H - 24) / row_step;
    if (vis < 4) vis = 4;
    if (s_menu_sel < s_menu_scroll) s_menu_scroll = s_menu_sel;
    if (s_menu_sel >= s_menu_scroll + (uint32_t)vis) s_menu_scroll = s_menu_sel - vis + 1;
    int shown = count - (int)s_menu_scroll;
    if (shown > vis) shown = vis;

    int msg_h = (s_menu_msg[0] ? chh + 10 : 0);
    int h = 12 + msg_h + shown * row_step + 12;
    if (h > H - 8) h = H - 8;
    int y0 = (H - h) / 2;
    if (y0 < 4) y0 = 4;
    int y1 = y0 + h;
    if (y1 > H - 4) y1 = H - 4;

    /* 实心白底 (覆盖底层正文, 不透字) */
    for (int y = y0; y <= y1; y++)
        for (int x = X0; x <= X1; x++)
            menu_fb_px(lcd, x, y, false);
    /* 2px 粗黑边框 */
    for (int x = X0; x <= X1; x++) { menu_fb_px(lcd, x, y0, true); menu_fb_px(lcd, x, y1, true); }
    for (int y = y0; y <= y1; y++) { menu_fb_px(lcd, X0, y, true); menu_fb_px(lcd, X1, y, true); }
    for (int x = X0; x <= X1; x++) { menu_fb_px(lcd, x, y0 + 1, true); menu_fb_px(lcd, x, y1 - 1, true); }
    for (int y = y0; y <= y1; y++) { menu_fb_px(lcd, X0 + 1, y, true); menu_fb_px(lcd, X1 - 1, y, true); }

    int ty = y0 + 12;
    if (s_menu_msg[0]) {
        int mw = menu_text_width(s_menu_msg);
        int mx = X0 + ((X1 - X0) - mw) / 2;
        if (mx < X0 + 1) mx = X0 + 1;
        menu_draw_text(lcd, mx, ty, s_menu_msg, false);
        ty += chh + 10;
        for (int x = X0 + 1; x < X1; x++) menu_fb_px(lcd, x, ty - 5, true);   /* 消息分隔线 */
    }
    for (int k = 0; k < shown; k++) {
        int idx = (int)s_menu_scroll + k;
        bool sel = (idx == (int)s_menu_sel);
        if (sel) {
            for (int yy = ty; yy < ty + chh; yy++)
                for (int xx = X0 + 1; xx < X1; xx++)
                    menu_fb_px(lcd, xx, yy, true);
        }
        const char *text = menu_item_text(idx);
        int tw = menu_text_width(text);
        int tx = X0 + ((X1 - X0) - tw) / 2;   /* V1.0.68: 选项居中 */
        if (tx < X0 + 1) tx = X0 + 1;
        menu_draw_text(lcd, tx, ty, text, sel);
        ty += row_step;
    }

    font_book_select(s_fontstyle, s_fontsize);   /* 恢复阅读字号 */
}

/* 退出确认弹窗 */
static void draw_exit_confirm(st7305_handle_t *lcd) {
    int W = book_is_portrait() ? BOOK_PORTRAIT_W : ST7305_WIDTH;
    int H = book_is_portrait() ? BOOK_PORTRAIT_H : ST7305_HEIGHT;
    int chh = book_line_h();
    int bw = W - 40;                       /* 横向宽条 */
    int bh = chh + 18;                     /* 单行提示 */
    int X0 = (W - bw) / 2, X1 = X0 + bw - 1;
    int y0 = (H - bh) / 2, y1 = y0 + bh - 1;
    /* 实心白底 */
    for (int y = y0; y <= y1; y++)
        for (int x = X0; x <= X1; x++)
            menu_fb_px(lcd, x, y, false);
    /* 2px 粗黑边框 */
    for (int x = X0; x <= X1; x++) { menu_fb_px(lcd, x, y0, true); menu_fb_px(lcd, x, y1, true); }
    for (int y = y0; y <= y1; y++) { menu_fb_px(lcd, X0, y, true); menu_fb_px(lcd, X1, y, true); }
    for (int x = X0; x <= X1; x++) { menu_fb_px(lcd, x, y0 + 1, true); menu_fb_px(lcd, x, y1 - 1, true); }
    for (int y = y0; y <= y1; y++) { menu_fb_px(lcd, X0 + 1, y, true); menu_fb_px(lcd, X1 - 1, y, true); }
    menu_draw_text(lcd, X0 + 8, y0 + 6, M_EXIT_ASK, false);
}

/* ============ 渲染入口 ============ */

void book_reader_render(st7305_handle_t *lcd) {
    if (!s_open || !lcd) return;
    /* V1.0.68 fix: 每 30s 自动保存, 慢读/长停留崩溃也不丢进度 */
    static uint32_t s_last_auto_save = 0;
    uint32_t now_tick = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    if (now_tick - s_last_auto_save >= 30000) {
        s_last_auto_save = now_tick;
        s_last_saved_page = s_page;
        book_save_progress();
    }
    if (book_is_portrait()) {
        render_portrait(lcd);
        /* 覆盖层画进竖屏画布, 与正文一起旋转 (横着显示) */
        if (s_exit_confirm) draw_exit_confirm(lcd);
        if (s_menu != BM_READ) draw_reader_menu(lcd);
        portrait_rotate(lcd);
    } else {
        render_landscape(lcd);
        if (s_exit_confirm) draw_exit_confirm(lcd);
        if (s_menu != BM_READ) draw_reader_menu(lcd);
    }
    if (s_night && !s_inverted) {
        st7305_set_inversion(lcd, true);
        s_inverted = true;
    } else if (!s_night && s_inverted) {
        st7305_set_inversion(lcd, false);
        s_inverted = false;
    }
    if (s_rot == 1 && s_rot_buf) {
        st7305_flush_rotated(lcd, 1, s_rot_buf);
    } else {
        st7305_flush(lcd);
    }
}

/* ============ 文件加载 ============ */

static const char *base_name(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static bool is_book_ext(const char *path, const char *ext) {
    const char *dot = strrchr(path, '.');
    if (!dot) return false;
    return strcasecmp(dot, ext) == 0;
}

/* 识别编码 + BOM 偏移 (返回后文件指针复位到 0) */
static uint8_t book_sniff_enc(FILE *f, uint32_t *bom) {
    uint8_t b[4] = {0};
    size_t n = fread(b, 1, 4, f);
    *bom = 0;
    if (n >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) { *bom = 3; fseek(f, 0, SEEK_SET); return BF_ENC_UTF8; }
    if (n >= 2 && b[0] == 0xFF && b[1] == 0xFE) { *bom = 2; fseek(f, 0, SEEK_SET); return BF_ENC_UTF16LE; }
    if (n >= 2 && b[0] == 0xFE && b[1] == 0xFF) { *bom = 2; fseek(f, 0, SEEK_SET); return BF_ENC_UTF16BE; }
    uint8_t *smp = heap_caps_malloc(16384, MALLOC_CAP_SPIRAM);
    if (!smp) { fseek(f, 0, SEEK_SET); return BF_ENC_UTF8; }
    size_t sn = fread(smp, 1, 16384, f);
    bool v = bf_utf8_valid(smp, sn);
    free(smp);
    fseek(f, 0, SEEK_SET);
    return v ? BF_ENC_UTF8 : BF_ENC_GBK;
}

/* 章节标题 (书签用): 返回当前页所在章节标题, 无则 NULL */
static const char *book_chapter_title_at(uint32_t off) {
    if (s_chapter_count == 0) return NULL;
    int lo = 0, hi = (int)s_chapter_count - 1, ans = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (s_chapters[mid].off <= off) { ans = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    return (ans >= 0) ? s_chapters[ans].title : NULL;
}

static uint32_t book_page_for_offset(uint32_t off) {
    if (s_page_count == 0) return 0;
    int lo = 0, hi = (int)s_page_count - 1, ans = 0;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (s_page_off[mid] <= off) { ans = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    return (uint32_t)ans;
}

static void book_add_bookmark(void) {
    if (s_bm_count >= BOOK_MAX_BOOKMARKS) {
        snprintf(s_menu_msg, sizeof(s_menu_msg), "%s", M_BM_FULL);
        return;
    }
    uint32_t off = 0;
    if (s_idx_mutex) xSemaphoreTake(s_idx_mutex, portMAX_DELAY);
    if (s_page < s_page_count) off = s_page_off[s_page];
    if (s_idx_mutex) xSemaphoreGive(s_idx_mutex);
    char title[32];
    const char *ct = book_chapter_title_at(off);
    if (ct) {
        snprintf(title, sizeof(title), "%s", ct);
    } else if (s_enc == BF_ENC_GBK) {
        snprintf(title, sizeof(title), "\xB5\xDA%lu\xD2\xB3", (unsigned long)(s_page + 1));
    } else {
        snprintf(title, sizeof(title), "\xE7\xAC\xAC%lu\xE9\xA1\xB5", (unsigned long)(s_page + 1));
    }
    s_bms[s_bm_count].off = off;
    s_bms[s_bm_count].page = s_page;
    snprintf(s_bms[s_bm_count].title, sizeof(s_bms[s_bm_count].title), "%s", title);
    s_bm_count++;
    book_save_progress();
    snprintf(s_menu_msg, sizeof(s_menu_msg), "%s", M_ADDED);
}

static void book_menu_select(void) {
    int idx = (int)s_menu_sel;
    int count = menu_item_count();
    if (idx < 0 || idx >= count) return;
    switch (s_menu) {
        case BM_MENU:
            switch (idx) {
                case 0: book_add_bookmark(); break;
                case 1: s_menu = BM_BMKS; s_menu_sel = 0; s_menu_scroll = 0; break;
                case 2: book_reader_close(); break;   /* 返回书库 (二级菜单) */
                default: s_menu = BM_READ; break;     /* 继续阅读 */
            }
            return;
        case BM_TOC:
            if (s_chapter_count == 0) return;
            if (idx < (int)s_chapter_count) {
                uint32_t p = book_page_for_offset(s_chapters[idx].off);
                book_wait_indexed_page(p);
                if (!s_index_error && s_page_count > p) {
                    s_page = p;
                    s_menu = BM_READ;
                }
            } else {
                s_menu = BM_MENU;
                s_menu_sel = 0;
                s_menu_scroll = 0;
            }
            return;
        case BM_BMKS:
            if (s_bm_count == 0) return;
            if (idx < (int)s_bm_count) {
                uint32_t p = s_bms[idx].page;
                if (p >= s_page_count) p = book_page_for_offset(s_bms[idx].off);
                book_wait_indexed_page(p);
                if (!s_index_error && s_page_count > p) {
                    s_page = p;
                    s_menu = BM_READ;
                }
            } else if (idx == (int)s_bm_count) {
                s_bm_count = 0;
                book_save_progress();
                snprintf(s_menu_msg, sizeof(s_menu_msg), "%s", M_CLEARED);
            } else {
                s_menu = BM_MENU;
                s_menu_sel = 0;
                s_menu_scroll = 0;
            }
            return;
        default:
            return;
    }
}

bool book_reader_open(const char *path) {
    if (!path) return false;
    if (s_open) book_reader_close();
    s_lcd = NULL;

    if (!font_book_init()) {
        ESP_LOGE(TAG, "字库初始化失败: %s", font_book_error());
        return false;
    }
    font_book_select(s_fontstyle, s_fontsize);   /* 按设置切换字号 */

    struct stat st;
    if (stat(path, &st) != 0) {
        ESP_LOGE(TAG, "stat 失败: %s", path);
        return false;
    }
    uint32_t fsz = (uint32_t)st.st_size;
    if (fsz == 0 || fsz > BOOK_MAX_FILE) {
        ESP_LOGE(TAG, "文件大小非法: %lu (上限 %u)", (unsigned long)fsz, BOOK_MAX_FILE);
        return false;
    }
    snprintf(s_path, sizeof(s_path), "%s", path);
    snprintf(s_src_path, sizeof(s_src_path), "%s", path);
    if (is_book_ext(path, ".fb2")) {
        /* FB2: 流式转换为缓存 TXT (按 路径+大小+时间 命名, 变更自动重建) */
        char dir[128];
        snprintf(dir, sizeof(dir), "/sdcard/books/.cache");
        mkdir(dir, 0755);
        char cache[320];
        snprintf(cache, sizeof(cache), "/sdcard/books/.cache/fb2_%08lx_%08lx_%08lx.txt",
                 (unsigned long)fnv1a(path), (unsigned long)(uint32_t)st.st_mtime,
                 (unsigned long)fsz);
        struct stat cst;
        if (stat(cache, &cst) != 0) {
            char bt[96] = {0};
            if (fb2_convert(path, cache, bt, sizeof(bt)) != 0) {
                ESP_LOGE(TAG, "FB2 转换失败: %s", path);
                return false;
            }
            if (bt[0]) ESP_LOGI(TAG, "FB2 书名: %s", bt);
        }
        snprintf(s_src_path, sizeof(s_src_path), "%s", cache);
    } else if (is_book_ext(path, ".epub")) {
        char dir[128];
        snprintf(dir, sizeof(dir), "/sdcard/books/.cache");
        mkdir(dir, 0755);
        char cache[320];
        snprintf(cache, sizeof(cache), "/sdcard/books/.cache/epub_%08lx_%08lx_%08lx.txt",
                 (unsigned long)fnv1a(path), (unsigned long)(uint32_t)st.st_mtime,
                 (unsigned long)fsz);
        struct stat cst;
        if (stat(cache, &cst) != 0) {
            char bt[96] = {0};
            if (epub_convert(path, cache, bt, sizeof(bt)) != 0) {
                ESP_LOGE(TAG, "EPUB 转换失败: %s", path);
                return false;
            }
            if (bt[0]) ESP_LOGI(TAG, "EPUB 书名: %s", bt);
        }
        snprintf(s_src_path, sizeof(s_src_path), "%s", cache);
    }
    FILE *f = fopen(s_src_path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "打开失败: %s", path);
        s_src_path[0] = 0;
        return false;
    }
    s_fp = f;
    s_file_size = fsz;
    struct stat sst;
    s_src_size = (stat(s_src_path, &sst) == 0) ? (uint32_t)sst.st_size : fsz;
    s_mtime = (uint32_t)st.st_mtime;
    s_enc = book_sniff_enc(f, &s_file_start);
    ESP_LOGI(TAG, "打开 %s: %lu 字节, 编码=%d, 起点=%lu",
             base_name(path), (unsigned long)fsz, s_enc, (unsigned long)s_file_start);

    if (!s_idx_mutex) s_idx_mutex = xSemaphoreCreateMutex();
    if (!s_win) {
        s_win = heap_caps_malloc(BOOK_WIN_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_win) { fclose(f); s_fp = NULL; ESP_LOGE(TAG, "渲染窗口分配失败"); return false; }
    }
    s_win_base = 0;
    s_win_len = 0;
    if (!s_scan_chunk) s_scan_chunk = heap_caps_malloc(BOOK_SCAN_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_scan_win)   s_scan_win   = heap_caps_malloc(sizeof(bf_ch_t) * BF_MAX_WIN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_scan_brk)   s_scan_brk   = heap_caps_malloc(BF_MAX_WIN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_scan_scratch) s_scan_scratch = heap_caps_malloc(FLOW_SCRATCH_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_scan_off)   s_scan_off   = heap_caps_malloc(BF_MAX_WIN * 4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_rwin)       s_rwin       = heap_caps_malloc(sizeof(bf_ch_t) * BF_MAX_WIN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_rbrk)       s_rbrk       = heap_caps_malloc(BF_MAX_WIN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_rscratch)   s_rscratch   = heap_caps_malloc(FLOW_SCRATCH_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_scan_chunk || !s_scan_win || !s_scan_brk || !s_scan_scratch || !s_scan_off ||
        !s_rwin || !s_rbrk || !s_rscratch) {
        ESP_LOGE(TAG, "排版内核工作区分配失败");
        fclose(f);
        s_fp = NULL;
        return false;
    }

    if (s_rot == 1 && !s_rot_buf) {
        s_rot_buf = heap_caps_malloc(15000, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (book_is_portrait() && !s_pfb) {
        s_pfb = heap_caps_malloc(PFB_ROW_BYTES * BOOK_PORTRAIT_H, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }

    s_page_off = NULL;
    s_page_cap = s_page_count = 0;
    s_chapters = NULL;
    s_chapter_cap = s_chapter_count = 0;
    s_index_done = false;
    s_index_error = false;
    s_indexed_bytes = 0;
    s_page = 0;
    s_resume_off = 0;
    s_menu = BM_READ;
    s_exit_confirm = false;
    s_menu_msg[0] = 0;

    bool sidecar_loaded = book_load_sidecar();   /* V1.0.68: 先载页表, 供偏移恢复 */
    book_load_progress();                          /* 再载进度 */
    if (s_pending_layout_diff && s_pending_last_off > 0 && s_page_off && s_page_count) {
        /* 布局/字体已变化: 按字节偏移换算回最近页 */
        s_loaded_page = book_page_for_offset(s_pending_last_off);
        if (s_loaded_page >= s_page_count) s_loaded_page = 0;
        ESP_LOGI(TAG, "布局已变, 按偏移 %lu 恢复至页 %lu",
                 (unsigned long)s_pending_last_off, (unsigned long)s_loaded_page);
        s_pending_layout_diff = false;
    }
    if (!sidecar_loaded || !s_index_done) {
        if (book_scan_start() != 0) {
            ESP_LOGE(TAG, "索引失败");
            fclose(f);
            s_fp = NULL;
            s_path[0] = 0;
            if (s_page_off) { free(s_page_off); s_page_off = NULL; }
            if (s_chapters) { free(s_chapters); s_chapters = NULL; }
            s_page_count = s_chapter_count = 0;
            s_bm_count = 0;
            return false;
        }
    }

    if (s_loaded_page < s_page_count) s_page = s_loaded_page;
    s_last_saved_page = s_page;

    const char *bn = base_name(path);
    snprintf(s_title, sizeof(s_title), "%.63s", bn);
    char *dot = strrchr(s_title, '.');
    if (dot && strcasecmp(dot, ".txt") == 0) *dot = '\0';

    s_open = true;
    input_set_screen_rotation(s_rot);   /* V1.0.68: 触摸跟随当前旋转方向 */
    ESP_LOGI(TAG, "阅读器就绪: %s, %lu 页, 断点=%lu",
             s_title, (unsigned long)s_page_count, (unsigned long)s_page);

    return true;
}

bool book_reader_is_open(void) {
    return s_open;
}

void book_reader_close(void) {
    if (!s_open) return;
    if (!book_index_stop()) {
        ESP_LOGE(TAG, "索引任务未退出, 保留页表避免崩溃");
        s_page_off = NULL;
        s_page_cap = s_page_count = 0;
        s_chapters = NULL;
        s_chapter_cap = s_chapter_count = 0;
    }
    book_save_progress();
    if (!s_index_done && s_page_count > 0) {
        /* 未完成也落盘部分索引, 下次断点续扫 */
        book_save_sidecar();
    }
    if (s_inverted && s_lcd) {
        st7305_set_inversion(s_lcd, false);
        s_inverted = false;
    }
    if (s_page_off) { free(s_page_off); s_page_off = NULL; }
    if (s_chapters) { free(s_chapters); s_chapters = NULL; }
    if (s_win) { free(s_win); s_win = NULL; }
    if (s_rot_buf) { free(s_rot_buf); s_rot_buf = NULL; }
    if (s_pfb) { free(s_pfb); s_pfb = NULL; }
    if (s_fp) { fclose(s_fp); s_fp = NULL; }
    if (s_scan_chunk) { free(s_scan_chunk); s_scan_chunk = NULL; }
    if (s_scan_win) { free(s_scan_win); s_scan_win = NULL; }
    if (s_scan_brk) { free(s_scan_brk); s_scan_brk = NULL; }
    if (s_scan_scratch) { free(s_scan_scratch); s_scan_scratch = NULL; }
    if (s_scan_off) { free(s_scan_off); s_scan_off = NULL; }
    if (s_rwin) { free(s_rwin); s_rwin = NULL; }
    if (s_rbrk) { free(s_rbrk); s_rbrk = NULL; }
    if (s_rscratch) { free(s_rscratch); s_rscratch = NULL; }
    s_page_cap = s_page_count = 0;
    s_chapter_cap = s_chapter_count = 0;
    s_page = 0;
    s_index_done = false;
    s_index_error = false;
    s_indexed_bytes = 0;
    s_loaded_page = 0;
    s_bm_count = 0;
    s_exit_confirm = false;
    s_open = false;
    s_path[0] = 0;
    s_src_path[0] = 0;
    input_set_screen_rotation(0);   /* V1.0.68: 退出阅读器, 触摸恢复横屏 */
    ESP_LOGI(TAG, "阅读器已关闭, 内存已释放");
}

const char *book_reader_title(void) {
    return s_title;
}

bool book_reader_knock_active(void) {
    return false;   /* 敲击翻页功能已删除 (V1.0.64) */
}

void book_reader_set_settings(bool knock, int sens, bool night, bool pagenum, int rot,
                              int fontstyle, int fontsize, int margin, int lineh, int gap) {
    bool was_portrait = book_is_portrait();
    (void)knock;
    (void)sens;
    s_night = night;
    s_pagenum = pagenum;
    bool layout_changed = (s_open &&
        (fontsize != (int)s_fontsize || margin != (int)s_margin_id ||
         lineh != (int)s_lineh_id || gap != (int)s_gap_id));
    bool style_changed = (s_open && fontstyle >= 0 && fontstyle != (int)s_fontstyle);
    s_fontstyle = (uint8_t)((fontstyle < 0) ? 0 : (fontstyle > 1) ? 1 : fontstyle);
    s_fontsize = (uint8_t)((fontsize < 0) ? 1 : (fontsize > 2) ? 2 : fontsize);
    s_margin_id = (uint8_t)((margin < 0) ? 1 : (margin > 2) ? 2 : margin);
    s_lineh_id = (uint8_t)((lineh < 0) ? 1 : (lineh > 2) ? 2 : lineh);
    s_gap_id = (uint8_t)((gap < 0) ? 0 : (gap > 1) ? 1 : gap);
    uint8_t new_rot = (uint8_t)((rot < 0) ? 0 : (rot > 3) ? 3 : rot);
    if (s_open && new_rot != s_rot) {
        s_rot = new_rot;
        bool now_portrait = book_is_portrait();
        if (s_rot == 1 && !s_rot_buf) {
            s_rot_buf = heap_caps_malloc(15000, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        } else if (s_rot != 1 && s_rot_buf) {
            free(s_rot_buf);
            s_rot_buf = NULL;
        }
        if (now_portrait && !s_pfb) {
            s_pfb = heap_caps_malloc(PFB_ROW_BYTES * BOOK_PORTRAIT_H, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        } else if (!now_portrait && s_pfb) {
            free(s_pfb);
            s_pfb = NULL;
        }
        if (now_portrait != was_portrait || layout_changed) {
            if (layout_changed || style_changed) font_book_select(s_fontstyle, s_fontsize);
            book_restart_index();
        } else {
            s_page = 0;
            s_menu = BM_READ;
        }
    } else {
        s_rot = new_rot;
        if (layout_changed || style_changed) font_book_select(s_fontstyle, s_fontsize);
    }
    /* V1.0.68: 旋转方向变化时同步输入层, 触摸跟随旋转.
     * 仅阅读器打开时生效: 否则会把竖屏旋转泄漏到书籍二级菜单/主菜单的触摸坐标,
     * 导致点击位置偏移、返回主菜单后左右反向/反应迟钝 (重启才恢复). */
    if (s_open) input_set_screen_rotation(s_rot);
}

/* ============ 按键 ============ */

static void book_reader_prev_page(void) {
    if (s_page > 0) s_page--;
    /* V1.0.68 fix: 触摸/按键翻页统一周期保存, 崩溃/重启最多丢 4 页 */
    if ((s_page % 5) == 0 && s_page != s_last_saved_page) {
        s_last_saved_page = s_page;
        book_save_progress();
    }
}

static void book_reader_next_page(void) {
    if (s_page + 1 < s_page_count) {
        s_page++;
        ESP_LOGI(TAG, "翻页 -> %lu/%lu", (unsigned long)(s_page + 1), (unsigned long)s_page_count);
    } else if (!s_index_done && !s_index_error) {
        book_wait_indexed_page(s_page + 1);
        if (!s_index_error && s_page + 1 < s_page_count) {
            s_page++;
            ESP_LOGI(TAG, "翻页 -> %lu/%lu", (unsigned long)(s_page + 1), (unsigned long)s_page_count);
        } else {
            ESP_LOGW(TAG, "翻页被边界挡住: 页 %lu (已索引 %lu 页)", (unsigned long)(s_page + 1), (unsigned long)s_page_count);
            return;
        }
    } else {
        return;
    }
    /* V1.0.68 fix: 触摸/按键翻页统一周期保存, 崩溃/重启最多丢 4 页 */
    if ((s_page % 5) == 0 && s_page != s_last_saved_page) {
        s_last_saved_page = s_page;
        book_save_progress();
    }
}

bool book_reader_handle_action(int action) {
    if (!s_open) return false;
    if (s_exit_confirm) {
        switch (action) {
            case BOOK_ACTION_CONFIRM:
                book_reader_close();
                return true;
            case BOOK_ACTION_BACK:
            case BOOK_ACTION_HOME:
                s_exit_confirm = false;
                return true;
            default:
                s_exit_confirm = false;   /* 其他键取消确认 */
                return true;
        }
    }
    if (s_menu != BM_READ) {
        switch (action) {
            case BOOK_ACTION_UP:
            case BOOK_ACTION_LEFT:
                if (s_menu_sel > 0) s_menu_sel--;
                return true;
            case BOOK_ACTION_DOWN:
                if (s_menu_sel + 1 < (uint32_t)menu_item_count()) s_menu_sel++;
                return true;
            case BOOK_ACTION_RIGHT:
            case BOOK_ACTION_CONFIRM:
                book_menu_select();
                return true;
            case BOOK_ACTION_BACK:
            case BOOK_ACTION_HOME:
            case BOOK_ACTION_LONG_LEFT:
                s_menu = BM_READ;
                return true;
            default:
                return false;
        }
    }
    switch (action) {
        case BOOK_ACTION_DOWN:
            book_reader_next_page();
            if ((s_page % 5) == 0 && s_page != s_last_saved_page) {
                s_last_saved_page = s_page;
                book_save_progress();
            }
            return true;
        case BOOK_ACTION_RIGHT:
            /* V1.0.68: 左旋 (s_rot==2) 时 BOOT 键(右向) = 上一页, 与确定键对调 */
            if (s_rot == 2) {
                book_reader_prev_page();
            } else {
                book_reader_next_page();
                if ((s_page % 5) == 0 && s_page != s_last_saved_page) {
                    s_last_saved_page = s_page;
                    book_save_progress();
                }
            }
            return true;
        case BOOK_ACTION_CONFIRM:
            /* V1.0.68: 竖屏方向决定确定键翻页方向: 左旋=下一页, 右旋=上一页 (两键对调) */
            if (s_rot == 3) {
                book_reader_prev_page();
            } else {
                book_reader_next_page();
                if ((s_page % 5) == 0 && s_page != s_last_saved_page) {
                    s_last_saved_page = s_page;
                    book_save_progress();
                }
            }
            return true;
        case BOOK_ACTION_LEFT:
        case BOOK_ACTION_UP:
            book_reader_prev_page();
            return true;
        case BOOK_ACTION_BACK:
            /* V1.0.68: 长按 BOOT (返回键) 在阅读页 = 退出阅读器回书库二级菜单 */
            book_reader_close();
            return true;
        case BOOK_ACTION_HOME:
            s_exit_confirm = true;
            return true;
        case BOOK_ACTION_LONG_LEFT:
            s_menu = BM_MENU;
            s_menu_sel = 0;
            s_menu_scroll = 0;
            s_menu_msg[0] = 0;
            s_exit_confirm = false;
            return true;
        default:
            return false;
    }
}

/* V1.0.68: 阅读器二级菜单 (设置/目录/书签) 触摸命中.
 * 与 draw_reader_menu 新布局一致: 32px 大字, 无标题, 行距 row_step, 垂直居中. */
static bool book_menu_handle_touch(int x, int y) {
    int count = menu_item_count();
    if (count <= 0) return false;
    int W = book_is_portrait() ? BOOK_PORTRAIT_W : ST7305_WIDTH;
    int H = book_is_portrait() ? BOOK_PORTRAIT_H : ST7305_HEIGHT;
    const int X0 = 28, X1 = W - 28;
    if (x < X0 || x > X1) return false;

    font_book_select(s_fontstyle, 2);   /* 与 draw_reader_menu 相同: 32px */
    int chh = font_book_cell_h();
    int row_step = chh + 14;
    int vis = (H - 24) / row_step;
    if (vis < 4) vis = 4;
    int scroll = s_menu_scroll;
    if (s_menu_sel < (uint32_t)scroll) scroll = s_menu_sel;
    if (s_menu_sel >= (uint32_t)scroll + (uint32_t)vis) scroll = s_menu_sel - vis + 1;
    int shown = count - scroll;
    if (shown > vis) shown = vis;
    int msg_h = (s_menu_msg[0] ? chh + 10 : 0);
    int h = 12 + msg_h + shown * row_step + 12;
    if (h > H - 8) h = H - 8;
    int y0 = (H - h) / 2;
    if (y0 < 4) y0 = 4;
    int ty = y0 + 12;
    if (s_menu_msg[0]) ty += chh + 10;
    font_book_select(s_fontstyle, s_fontsize);

    int k = (y - ty) / row_step;
    if (k < 0) return false;
    int idx = scroll + k;
    if (idx >= count) return false;
    s_menu_sel = (uint32_t)idx;
    book_menu_select();
    return true;
}

bool book_reader_handle_touch(int x, int y) {
    if (!s_open || s_exit_confirm) return false;

    if (s_menu != BM_READ) {
        /* 二级菜单: 点菜单项 */
        return book_menu_handle_touch(x, y);
    }

    (void)x;   /* 只关心逻辑纵向 y (已由 input 层旋转到逻辑坐标) */
    int lh = (s_rot == 2 || s_rot == 3) ? BOOK_PORTRAIT_H : ST7305_HEIGHT;

    if (y < lh / 3) {
        /* 上半页 -> 上一页 */
        book_reader_prev_page();
    } else if (y > lh * 2 / 3) {
        /* 下半页 -> 下一页 */
        book_reader_next_page();
    } else {
        /* 中间 -> 进设置菜单 */
        s_menu = BM_MENU;
        s_menu_sel = 0;
        s_menu_scroll = 0;
        s_menu_msg[0] = 0;
        s_exit_confirm = false;
    }
    return true;
}

bool book_reader_poll(void) {
    return false;   /* 敲击翻页功能已删除 (V1.0.64) */
}
