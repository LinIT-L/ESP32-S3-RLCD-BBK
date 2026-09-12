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
extern void input_set_swipe_turn(bool enable);   /* 阅读器横滑翻页开关 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "audio_player.h"
#include "font_book.h"
#include "book_flow.h"
#include "fb2_convert.h"
#include "epub_convert.h"
#include "input.h"

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
#define BOOK_WIN_SIZE       (64 * 1024)            /* 渲染窗口 (最大) */
#define BOOK_MAX_WIN_READ   (8 * 1024)             /* 单次渲染 SD 读上限: 一屏仅数百字节, 8KB 足够且秒读 */
#define BOOK_SCAN_CHUNK     (32 * 1024)            /* 索引读缓冲 */
#define BOOK_SCAN_STEP      (256u * 1024)          /* 后台任务每步扫描量 */
#define BOOK_FIRST_CHUNK    (48u * 1024)           /* 打开时同步索引量 (提速进入) */
#define BOOK_TOC_REBUILD_MIN 5u                    /* 侧边索引目录数低于此视为旧缓存, 打开后后台重建目录 */

#define BOOK_CONTENT_X0   8
#define BOOK_CONTENT_Y0   2                    /* 全屏: 无标题栏 */
/* 竖屏布局 (旋转 90/270): 逻辑 300 宽 x 400 高, 17 列 x 22 行 */
#define BOOK_PORTRAIT_W      300
#define BOOK_PORTRAIT_H      400

#define FLOW_SCRATCH_SIZE   (BF_MAX_WIN * 8 + 8)

/* === 阅读器状态 === */
static bool         s_open = false;
static FILE        *s_fp = NULL;          /* 渲染句柄 (整个阅读期间保持) */
EXT_RAM_BSS_ATTR static char         s_path[256] = {0};
EXT_RAM_BSS_ATTR static char         s_src_path[320] = {0}; /* 实际阅读源 (FB2 时为转换缓存) */
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
EXT_RAM_BSS_ATTR static book_bm_t   s_bms[BOOK_MAX_BOOKMARKS];
static uint32_t    s_bm_count = 0;
static uint32_t    s_loaded_page = 0;     /* 断点续读页 */
static uint32_t    s_last_saved_page = 0; /* 上次落盘页 (每 5 页保存) */

/* === 索引任务同步 === */
static SemaphoreHandle_t s_idx_mutex = NULL;
static TaskHandle_t s_idx_task = NULL;
static volatile bool s_idx_stop = false;
static bool          s_toc_prescanned = false;   /* 章节目录已由预扫描填满, 分页不再重复追加 */

/* === 阅读菜单 === */
enum { BM_READ = 0, BM_MENU = 1, BM_TOC = 2, BM_BMKS = 3, BM_FONT = 4 };
static uint8_t      s_menu = BM_READ;
static uint32_t     s_menu_sel = 0;
static uint32_t     s_menu_scroll = 0;
static char         s_menu_msg[40] = {0};
static bool         s_menu_dragged = false;   /* 列表刚被拖动过: 抑制松手点击跳转 */
static bool         s_exit_confirm = false;   /* 返回菜单键退出确认 */
static int          s_menu_pix = 0;           /* 列表像素级滚动偏移 (0..ROW_H-1) 实现跟手 */
#define TOC_PER_PAGE  50                      /* 目录每页显示章数 */
#define TOC_PAGER_H   30                      /* 目录底部页码条高度 */
#define SCROLLBAR_W   40                      /* 目录右侧滚动条宽度 */
static uint32_t     s_toc_page = 0;           /* 目录页码 (0 起) */

/* V1.0.88: TF 卡字体 
 * V1.0.96: s_sd_fonts 移到 PSRAM (仅在字体选择/扫描时用, 低频, 省 1KB 内部 RAM). */
EXT_RAM_BSS_ATTR static char s_sd_fonts[16][64];      /* TF 卡字体文件名列表 */
static int          s_sd_font_count = 0;     /* TF 卡字体数量 */

/* === 旋转/夜间 === */
static uint8_t      s_rot = 0;            /* 0上 1下 2左 3右 */
static bool         s_night = false;
static bool         s_pagenum = false;    /* 默认不显示进度信息 */
static uint32_t     s_open_tick_ms = 0;   /* 本次阅读会话起始时间 (ms, 用于"已读时长") */
static uint8_t      s_fontstyle = 0;       /* 0=仿宋 1=黑体(菜单字体) */
static uint8_t      s_fontsize = 1;       /* 字号档 0=20 1=24 2=28 3=32 (默认 24 档=1) */
/* 字号档 → 实际像素 (font_book_select 按像素渲染/缩放) */
static int book_reader_size_px(int id) { static const int t[4] = {20,24,28,32}; return t[(id<0)?1:(id>3)?3:id]; }
static uint8_t      s_margin_id = 1;      /* 0=窄 1=中 2=宽 */
static uint8_t      s_lineh_id = 1;       /* 0=紧凑 1=标准 2=宽松 */
static uint8_t      s_gap_id = 0;         /* 0=标准 1=宽松 */
static bool         s_indent = true;      /* 段落首行缩进两全角空格 (开/关) */
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
/* 字间距: 0=紧凑(-2px) 1=标准(0) 2=宽松(+2px) */
static inline int book_gap(void) {
    static const int g[3] = { -2, 0, 2 };
    return g[s_gap_id > 2 ? 1 : s_gap_id];
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

/* V1.1.0: 文本块在内容区水平/垂直居中, 消除四边距不均 (字符按整格/整行排,
 * 取整产生的余量只有右侧/下侧, 导致左右、上下的留白不一样). 居中后对称. */
static void book_center(int *x0, int *y0, int maxw_cells, int rows, bool portrait) {
    int marg = book_margin();
    int cw = font_book_cell_w(), cg = book_gap();
    int W = portrait ? BOOK_PORTRAIT_W : ST7305_WIDTH;
    int H = portrait ? BOOK_PORTRAIT_H : ST7305_HEIGHT;
    int content_w = W - 2 * marg, content_h = H - 2 * marg;
    int block_w = maxw_cells * (cw + cg) - cg;
    if (block_w > content_w) block_w = content_w;
    int xoff = (content_w - block_w) / 2; if (xoff < 0) xoff = 0;
    int used_h = rows * book_line_h();
    int yoff = (content_h - used_h) / 2;  if (yoff < 0) yoff = 0;
    *x0 = marg + xoff;
    *y0 = marg + yoff;
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
        uint32_t nc = s_page_cap ? s_page_cap * 2 : 2048;
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
/* 规则集参考 Legado txtTocRule.json (精选高频规则), 行首 ^ 锚定:
 *   A: 第X章/节/卷/回/话/部/篇
 *   B: 特殊章节名 (序章/楔子/正文/终章/后记/尾声/番外/前言/简介)
 *   C: 数字/中文数字 + 分隔符/括号 + 标题  (1、xxx / 一、xxx / 24章 xxx / (12)xxx / 【第一章 xxx】)
 *   D: 英文 Part/Episode/No./Chapter/Volume/Section
 *   F: 兜底字数分节 (无任何章节时, 见 book_toc_fallback) */

static bool utf8_num_word(const uint8_t *p) {
    /* 0-9/十百千万两 + 财务大写壹贰叁... 拾佰仟 + 〇.
     * 注意: 与 utf8_suffix 一致, 数组里存的是 UTF-8 三字节码(如 一=E4B880),
     * 不是 Unicode 码点(一=4E00). 旧实现误用码点导致中文数字永不匹配. */
    static const uint32_t words[] = {
        0xE99BB6, 0xE4B880, 0xE4BA8C, 0xE4B889, 0xE59B9B, 0xE4BA94, 0xE585AD,
        0xE4B883, 0xE585AB, 0xE4B99D, 0xE58D81, 0xE799BE, 0xE58D83, 0xE4B887, 0xE4B8A4,
        0xE38087, 0xE5A3B9, 0xE8B4B0, 0xE58F81, 0xE88286, 0xE4BC8D, 0xE99986,
        0xE69F92, 0xE68D8C, 0xE78E96, 0xE68BBE, 0xE4BDB0, 0xE4BB9F
    };
    uint32_t cp = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        if (cp == words[i]) return true;
    }
    return false;
}

static bool gbk_num_word(const uint8_t *p) {
    /* 0-9/十百千万两 + 财务大写壹贰叁... 拾佰仟 + 〇 (与 utf8_num_word 对齐的 GBK 码) */
    static const uint16_t words[] = {
        0xC1E3, 0xD2BB, 0xB6FE, 0xC8FD, 0xCBC4, 0xCEE5, 0xC1F9, 0xC6DF,
        0xB0CB, 0xBEC5, 0xCAAE, 0xB0D9, 0xC7A7, 0xCDF2, 0xC1BD,
        0xA996, 0xD2BC, 0xB7A1, 0xC8FE, 0xCBC1, 0xCEE9, 0xC2BD, 0xC6E2,
        0xB0C6, 0xBEC1, 0xCAB0, 0xB0DB, 0xC7AA
    };
    uint16_t w = (uint16_t)((p[0] << 8) | p[1]);
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        if (w == words[i]) return true;
    }
    return false;
}

/* 章节单位后缀: 章/回/节/卷/部/话/集/篇 */
static bool utf8_suffix(const uint8_t *p) {
    static const uint32_t suf[] = {
        0xE7ABA0, 0xE59B9E, 0xE88A82, 0xE58DB7, 0xE983A8, 0xE8AF9D, 0xE99B86, 0xE7AF87
    };
    uint32_t cp = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
    for (size_t i = 0; i < sizeof(suf) / sizeof(suf[0]); i++) {
        if (cp == suf[i]) return true;
    }
    return false;
}

static bool gbk_suffix(const uint8_t *p) {
    static const uint16_t suf[] = {
        0xB5C2, 0xBBD8, 0xBDDA, 0xBEED, 0xB2BF, 0xBBB0, 0xBCAF, 0xC6AA
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

/* 单元后缀后紧跟常见正文字 → 判定非章节 (Legado 负向预查: 部+分赛游帮 = 部分/比赛/游戏/部队) */
static bool toc_suffix_bad_follow(const uint8_t *s, bool gbk) {
    if (gbk) {
        if (s[0] == 0xB2 && s[1] == 0xBF) {   /* 部 */
            if (s[2] == 0xB7 && s[3] == 0xD6) return true;   /* 分 */
            if (s[2] == 0xC8 && s[3] == 0xFC) return true;   /* 赛 */
            if (s[2] == 0xD3 && s[3] == 0xCE) return true;   /* 游 */
            if (s[2] == 0xB0 && s[3] == 0xEF) return true;   /* 帮 */
        }
    } else {
        if (s[0] == 0xE9 && s[1] == 0x83 && s[2] == 0xA8) { /* 部 */
            if (s[3] == 0xE5 && s[4] == 0x88 && s[5] == 0x86) return true; /* 分 */
            if (s[3] == 0xE8 && s[4] == 0xB5 && s[5] == 0x9B) return true; /* 赛 */
            if (s[3] == 0xE6 && s[4] == 0xB8 && s[5] == 0xB8) return true; /* 游 */
            if (s[3] == 0xE5 && s[4] == 0xB8 && s[5] == 0xAE) return true; /* 帮 */
        }
    }
    return false;
}

/* 规则 A: 第X章/节/卷/回/话/部/篇 (行首), 含反证排除 (Legado 负向预查, 防"第一部分"误判) */
static bool toc_rule_a(const uint8_t *p, int n, bool gbk) {
    if (gbk) {
        if (n >= 2 && p[0] == 0xB5 && p[1] == 0xDA) {
            int j = 2, nd = 0;
            while (j < n) {
                if (p[j] >= '0' && p[j] <= '9') { j++; nd++; }
                else if (j + 1 < n && gbk_num_word(p + j)) { j += 2; nd++; }
                else break;
            }
            if (nd > 0 && j + 1 < n && gbk_suffix(p + j)) {
                /* 部 + 分/赛/游/帮 → 非章节 */
                if (j + 3 < n && toc_suffix_bad_follow(p + j, true)) return false;
                return true;
            }
        }
    } else {
        if (n >= 3 && p[0] == 0xE7 && p[1] == 0xAC && p[2] == 0xAC) {
            int j = 3, nd = 0;
            while (j < n) {
                if (p[j] >= '0' && p[j] <= '9') { j++; nd++; }
                else if (j + 2 < n && utf8_num_word(p + j)) { j += 3; nd++; }
                else break;
            }
            if (nd > 0 && j + 2 < n && utf8_suffix(p + j)) {
                /* 第X部 + (分/赛/游/帮) → "第一部分/比赛/游戏" 非章节 */
                if (j + 5 < n && toc_suffix_bad_follow(p + j, false)) return false;
                return true;
            }
        }
    }
    return false;
}

/* 规则 B: 特殊章节名 (行首独立成章) */
static bool toc_special(const uint8_t *p, int n, bool gbk) {
    static const uint8_t *const W[][2] = {
        { (const uint8_t *)"\xE5\xBA\x8F\xE7\xAB\xA0", (const uint8_t *)"\xD0\xF2\xD5\xC2" }, /* 序章 */
        { (const uint8_t *)"\xE6\xA5\x94\xE5\xAD\x90", (const uint8_t *)"\xD0\xA8\xD7\xD3" }, /* 楔子 */
        { (const uint8_t *)"\xE6\xAD\xA3\xE6\x96\x87", (const uint8_t *)"\xD5\xFD\xCE\xC4" }, /* 正文 */
        { (const uint8_t *)"\xE7\xBB\x88\xE7\xAB\xA0", (const uint8_t *)"\xD6\xD5\xD5\xC2" }, /* 终章 */
        { (const uint8_t *)"\xE5\x90\x8E\xE8\xAE\xB0", (const uint8_t *)"\xBA\xF3\xBC\xC7" }, /* 后记 */
        { (const uint8_t *)"\xE5\xB0\xBE\xE5\xA3\xB0", (const uint8_t *)"\xCE\xB2\xC9\xF9" }, /* 尾声 */
        { (const uint8_t *)"\xE7\x95\xAA\xE5\xA4\x96", (const uint8_t *)"\xB7\xAC\xCD\xE2" }, /* 番外 */
        { (const uint8_t *)"\xE5\x89\x8D\xE8\xA8\x80", (const uint8_t *)"\xC7\xB0\xD1\xD4" }, /* 前言 */
        { (const uint8_t *)"\xE7\xAE\x80\xE4\xBB\x8B", (const uint8_t *)"\xBC\xF2\xBD\xE9" }, /* 简介 */
    };
    for (size_t w = 0; w < sizeof(W) / sizeof(W[0]); w++) {
        const uint8_t *seq = gbk ? W[w][1] : W[w][0];
        int l = gbk ? 4 : 6;
        if (n >= l && memcmp(p, seq, (size_t)l) == 0) return true;
    }
    return false;
}

/* 规则 C: 数字/中文数字 + 分隔符/括号 + 标题 (行首) */
static bool toc_numbered(const uint8_t *p, int n, bool gbk) {
    int i = 0;
    /* 可选左括号: 【（[ ( */
    int open = 0;
    if (i < n) {
        if (!gbk && n - i >= 3 &&
            ((p[i] == 0xE3 && p[i + 1] == 0x80 && p[i + 2] == 0x90) ||   /* 【 */
             (p[i] == 0xEF && p[i + 1] == 0xBC && p[i + 2] == 0x88))) {  /* （ */
            i += 3; open = 1;
        } else if (gbk && n - i >= 2 &&
                   ((p[i] == 0xA1 && p[i + 1] == 0xBE) ||                 /* 【 */
                    (p[i] == 0xA3 && p[i + 1] == 0xA8))) {                /* （ */
            i += 2; open = 1;
        } else if (p[i] == '(' || p[i] == '[') { i++; open = 1; }
    }
    /* 括号内可能直接是 第X章 (如【第一章 xxx】) → 走规则 A */
    if (open) {
        if (toc_rule_a(p + i, n - i, gbk)) return true;
    }
    /* 数字: 半角数字 或 中文数字 */
    int nd = 0;
    while (i < n) {
        if (p[i] >= '0' && p[i] <= '9') { i++; nd++; continue; }
        if (gbk) {
            if (n - i >= 2 && gbk_num_word(p + i)) { i += 2; nd++; continue; }
        } else if (n - i >= 3 && utf8_num_word(p + i)) { i += 3; nd++; continue; }
        break;
    }
    if (nd == 0) return false;
    /* 可选章节单位 */
    int unit = 0;
    if (gbk) {
        if (n - i >= 2 && gbk_suffix(p + i)) {
            /* 一"部"分 → 非章节 (负向预查) */
            if (n - i >= 4 && toc_suffix_bad_follow(p + i, true)) return false;
            i += 2; unit = 1;
        }
    } else if (n - i >= 3 && utf8_suffix(p + i)) {
        if (n - i >= 6 && toc_suffix_bad_follow(p + i, false)) return false;
        i += 3; unit = 1;
    }
    /* 可选右括号: 】）] ) */
    int close = 0;
    if (i < n) {
        if (!gbk && n - i >= 3 &&
            ((p[i] == 0xE3 && p[i + 1] == 0x80 && p[i + 2] == 0x91) ||   /* 】 */
             (p[i] == 0xEF && p[i + 1] == 0xBC && p[i + 2] == 0x89))) {  /* ） */
            i += 3; close = 1;
        } else if (gbk && n - i >= 2 &&
                   ((p[i] == 0xA1 && p[i + 1] == 0xBF) ||                 /* 】 */
                    (p[i] == 0xA3 && p[i + 1] == 0xA9))) {                /* ） */
            i += 2; close = 1;
        } else if (p[i] == ')' || p[i] == ']') { i++; close = 1; }
    }
    /* 分隔符: 半角 . : 空格 tab - = _ / 全角 、 ． ： 。 */
    int sep = 0;
    if (i < n) {
        uint8_t c = p[i];
        if (c == '.' || c == ':' || c == ' ' || c == '\t' ||
            c == '-' || c == '=' || c == '_') { i++; sep = 1; }
        else if (!gbk && n - i >= 3 &&
                 ((p[i] == 0xE3 && p[i + 1] == 0x80 && p[i + 2] == 0x81) ||   /* 、 */
                  (p[i] == 0xEF && p[i + 1] == 0xBC && p[i + 2] == 0x8E) ||   /* ． */
                  (p[i] == 0xEF && p[i + 1] == 0xBC && p[i + 2] == 0x9A) ||   /* ： */
                  (p[i] == 0xE3 && p[i + 1] == 0x80 && p[i + 2] == 0x82))) {  /* 。 */
            i += 3; sep = 1;
        } else if (gbk && n - i >= 2 &&
                   ((p[i] == 0xA1 && p[i + 1] == 0xA2) ||   /* 、 */
                    (p[i] == 0xA3 && p[i + 1] == 0xAE) ||   /* ． */
                    (p[i] == 0xA3 && p[i + 1] == 0xBA) ||   /* ： */
                    (p[i] == 0xA1 && p[i + 1] == 0xA3))) {  /* 。 */
            i += 2; sep = 1;
        }
    }
    if (i >= n) return false;                 /* 数字后必须有标题内容 */
    if (!sep && !unit && !open && !close) return false;
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

    if (toc_rule_a(p, n, gbk)) return true;   /* 规则 A */
    if (toc_special(p, n, gbk)) return true;   /* 规则 B */
    if (toc_numbered(p, n, gbk)) return true;  /* 规则 C */

    const char *prefixes[] = { "chapter", "chap", "volume", "vol", "section", "sec",
                               "part", "episode", "no", "prologue", "story", "act" };
    for (size_t pi = 0; pi < sizeof(prefixes) / sizeof(prefixes[0]); pi++) {
        if (!ascii_prefix_cmp(p, n, prefixes[pi])) continue;
        int j = (int)strlen(prefixes[pi]);
        while (j < n && (p[j] == ' ' || p[j] == '\t' || p[j] == ':' || p[j] == '-' ||
                         p[j] == '.')) j++;
        if (j < n && p[j] >= '0' && p[j] <= '9') return true;
        break;
    }
    /* 规则 E: 卷 前缀式 (卷X/卷一, 无"第", Legado rule 17 对齐). 仅卷: 章/节前缀与正文(如人名"章三三")易误判 */
    int j = 0;
    if (gbk) {
        if (n >= 2 && p[0] == 0xBE && p[1] == 0xED) j = 2;                        /* 卷 */
        else j = 0;
    } else {
        if (n >= 3 && p[0] == 0xE5 && p[1] == 0x8D && p[2] == 0xB7) j = 3;        /* 卷 */
        else j = 0;
    }
    if (j > 0 && j < n) {
        int nd = 0;
        while (j < n) {
            if (p[j] >= '0' && p[j] <= '9') { j++; nd++; }
            else if (gbk && j + 1 < n && gbk_num_word(p + j)) { j += 2; nd++; }
            else if (!gbk && j + 2 < n && utf8_num_word(p + j)) { j += 3; nd++; }
            else break;
        }
        if (nd > 0) {
            /* 卷 + 数字 + 分/赛/游 → "卷一分" 非章节 */
            if (gbk && j + 1 < n && p[j] == 0xB7 && p[j + 1] == 0xD6) return false;
            if (!gbk && j + 2 < n && p[j] == 0xE5 && p[j + 1] == 0x88 && p[j + 2] == 0x86) return false;
            return true;
        }
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
    bool    at_para;        /* 段落首行状态 (跨步持续跟踪, 与分页一致) */
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
            /* 目录已由预扫描填满 → 分页不再重复追加 (s_toc_prescanned) */
            if (!s_toc_prescanned && n > 0 && toc_match(s, n, s_enc == BF_ENC_GBK)) {
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
                        s_scan_off,
                        &c->at_para,
                        s_indent ? 2 * (font_book_cell_w() + book_gap()) : 0,
                        limit, c->resume, &next);
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
    uint8_t  indent;
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
    uint8_t  indent;
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
    memcpy(h.magic, "BBKIDX07", 8);   /* v07: 目录去缩进/屏蔽简介噪声, 旧缓存失效重建 */
    h.fsz = s_file_size;
    h.mtime = s_mtime;
    h.enc = s_enc;
    h.layout = book_layout_id();
    h.font_w = (uint8_t)font_book_cell_w();
    h.font_h = (uint8_t)font_book_cell_h();
    h.margin = s_margin_id;
    h.lineh = s_lineh_id;
    h.gap = s_gap_id;
    h.indent = s_indent ? 1 : 0;
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
        memcmp(h.magic, "BBKIDX06", 8) != 0 ||
        h.fsz != s_file_size || h.mtime != s_mtime || h.enc != s_enc ||
        h.layout != book_layout_id() ||
        h.font_w != (uint8_t)font_book_cell_w() ||
        h.font_h != (uint8_t)font_book_cell_h() ||
        h.margin != s_margin_id || h.lineh != s_lineh_id || h.gap != s_gap_id ||
        h.indent != (s_indent ? 1 : 0) ||
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
    memcpy(h.magic, "BBKPOS05", 8);
    h.fsz = s_file_size;
    h.mtime = s_mtime;
    h.layout = book_layout_id();
    h.font_w = (uint8_t)font_book_cell_w();
    h.font_h = (uint8_t)font_book_cell_h();
    h.margin = s_margin_id;
    h.lineh = s_lineh_id;
    h.gap = s_gap_id;
    h.indent = s_indent ? 1 : 0;
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
    /* 设备级兜底: 记录最近阅读的书+页, TF 卡损坏/换卡后可回到最近这本 */
    nvs_handle_t nh;
    if (nvs_open("os_book", NVS_READWRITE, &nh) == ESP_OK) {
        nvs_set_str(nh, "last_path", s_path);
        nvs_set_i32(nh, "last_page", (int32_t)s_page);
        nvs_commit(nh);
        nvs_close(nh);
    }
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
    if (!f) {
        /* TF 卡无 .pos (损坏/换卡): 仅当这本书就是"最近阅读"时, 用 NVS 页号兜底 */
        nvs_handle_t h;
        if (nvs_open("os_book", NVS_READONLY, &h) == ESP_OK) {
            char last_path[256] = {0};
            size_t lp_len = sizeof(last_path);
            int32_t lp = 0;
            if (nvs_get_str(h, "last_path", last_path, &lp_len) == ESP_OK &&
                strcmp(last_path, s_path) == 0 &&
                nvs_get_i32(h, "last_page", &lp) == ESP_OK && lp > 0) {
                s_loaded_page = (uint32_t)lp;
                ESP_LOGI(TAG, "NVS 兜底恢复页 %lu", (unsigned long)s_loaded_page);
            }
            nvs_close(h);
        }
        return;
    }
    prog_hdr_t h;
    if (fread(&h, 1, sizeof(h), f) == sizeof(h) &&
        memcmp(h.magic, "BBKPOS05", 8) == 0 &&
        h.fsz == s_file_size && h.mtime == s_mtime) {
        /* 始终记录最近字节偏移, 重开时按偏移恢复 (页号因重开页数可能不同而不可靠) */
        if (h.last_off > 0) s_pending_last_off = h.last_off;
        /* 布局/字体一致 → 直接恢复页号; 不一致 → 记录字节偏移, 页表就绪后换算 */
        bool layout_match =
            h.layout == book_layout_id() &&
            h.font_w == (uint8_t)font_book_cell_w() &&
            h.font_h == (uint8_t)font_book_cell_h() &&
            h.margin == s_margin_id && h.lineh == s_lineh_id &&
            h.gap == s_gap_id && h.fontsize == s_fontsize &&
            h.indent == (s_indent ? 1 : 0);
        if (layout_match) {
            s_loaded_page = h.last_page;
            ESP_LOGI(TAG, "进度已加载: 页 %lu (布局一致)", (unsigned long)s_loaded_page);
        } else if (h.last_off > 0) {
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

/* 规则 F: 兜底字数分节 — 无任何规则命中时按 5000 字自动分节, 保证有目录可跳.
 * 在索引完成后调用 (仅在 s_chapter_count==0 时生效), 按行起始位置插入 "第N节". */
#define TOC_FALLBACK_CHARS 5000
static void book_toc_fallback(void) {
    if (s_chapter_count > 0 || !s_src_path[0]) return;
    FILE *fp = fopen(s_src_path, "rb");
    if (!fp) return;
    uint8_t *buf = heap_caps_malloc(BOOK_SCAN_CHUNK, MALLOC_CAP_SPIRAM);
    if (!buf) { fclose(fp); return; }
    uint32_t pos = s_file_start;
    uint32_t line_off = s_file_start;
    bool at_line_start = true;
    uint32_t chars = 0, marker = 0;
    uint8_t enc = s_enc;
    while (pos < s_src_size) {
        uint32_t want = BOOK_SCAN_CHUNK;
        if (want > s_src_size - pos) want = s_src_size - pos;
        if (fseek(fp, (long)pos, SEEK_SET) != 0) break;
        size_t got = fread(buf, 1, want, fp);
        if (got == 0) break;
        const uint8_t *p = buf, *end = buf + got;
        while (p < end) {
            bf_ch_t ch = bf_next_ch(p, end, enc);
            if (ch.adv == 0) break;
            if (ch.kind == BF_CH_NEWLINE) {
                at_line_start = true;
            } else if (ch.kind != BF_CH_SKIP) {
                if (at_line_start) {
                    line_off = pos + (uint32_t)(p - buf);
                    at_line_start = false;
                }
                chars++;
                if (chars >= TOC_FALLBACK_CHARS) {
                    chars = 0;
                    marker++;
                    char title[24];
                    if (s_enc == BF_ENC_GBK)
                        snprintf(title, sizeof(title), "\xB5\xDA%lu\xBD\xDA", (unsigned long)marker); /* 第N节 */
                    else
                        snprintf(title, sizeof(title), "\xE7\xAC\xAC%lu\xE8\x8A\x82", (unsigned long)marker);
                    idx_chapters_append(line_off, (const uint8_t *)title, (uint8_t)strlen(title));
                }
            }
            p += ch.adv;
        }
        pos += got;
    }
    free(buf);
    fclose(fp);
    if (s_chapter_count) ESP_LOGI(TAG, "兜底分节: %lu 章", (unsigned long)s_chapter_count);
}

/* 定义为 book_index_task 之前的前向声明 (定义在下方) */
static void book_toc_pre_scan(FILE *fp);

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
    /* 目录先行: 改用本任务独立句柄逐行扫描整本书填满章节目录 (不阻塞主线程).
     * 必须在分页追加入目录之前完成, 并把 s_toc_prescanned 置真, 使分页不再重复追加. */
    if (!s_toc_prescanned) {
        book_toc_pre_scan(fp);
        s_toc_prescanned = true;
    }
    int64_t step_t0 = esp_timer_get_time();
    int step_count = 0;
    while (!s_idx_stop && c.cur < c.fsz) {
        esp_task_wdt_reset();   /* 喂任务看门狗: 长索引不触发 WDT */
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
        book_toc_fallback();   /* 规则 F: 无章节时兜底字数分节 */
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

/* 后台目录重建任务: 侧边索引页表已完备但目录缺失/过少(旧缓存)时,
 * 后台逐行重扫全书生成完整目录并回写 .idx, 全程让出CPU+喂看门狗, 不阻塞阅读. */
static void book_toc_rebuild_task(void *arg) {
    (void)arg;
    FILE *fp = fopen(s_path, "rb");
    if (!fp) { s_idx_task = NULL; vTaskDelete(NULL); return; }
    esp_task_wdt_reset();
    if (!s_toc_prescanned) {
        book_toc_pre_scan(fp);   /* 分步逐行扫描(内部让出CPU), 填满章节目录 */
        s_toc_prescanned = true;
    }
    fclose(fp);
    if (!s_idx_stop) {
        esp_task_wdt_reset();
        book_save_sidecar();     /* 把完整目录回写 .idx, 下次秒开 */
        ESP_LOGI(TAG, "目录后台重建完成: %lu 章, 已回写 sidecar", (unsigned long)s_chapter_count);
    }
    s_idx_task = NULL;
    vTaskDelete(NULL);
}

/* 启动后台目录重建 (已在跑则跳过) */
static void book_toc_rebuild_start(void) {
    if (s_idx_task) return;   /* 已在跑 */
    s_idx_stop = false;
    if (xTaskCreate(book_toc_rebuild_task, "book_toc", 8192, NULL, 1, &s_idx_task) != pdPASS) {
        ESP_LOGW(TAG, "目录重建任务创建失败");
        s_idx_task = NULL;
    }
}

/* 页首是否段落首行: 检查页起点前是否有空行 (>=2 连续换行) 或书首.
 * 与扫描器在页边界处的段落状态一致 (仅取决于前一页末行是否空行). */
static uint32_t s_para_off = 0xFFFFFFFF;
static bool     s_para_val = false;
static bool book_page_para_compute(uint32_t off) {
    if (off <= s_file_start) return true;
    uint32_t want = 48;
    uint32_t base = (off > want) ? (off - want) : s_file_start;
    if (base < s_file_start) base = s_file_start;
    want = off - base;
    uint8_t buf[64];
    if (fseek(s_fp, (long)base, SEEK_SET) != 0) return false;
    size_t got = fread(buf, 1, want, s_fp);
    if (got == 0) return true;
    int nls = 0;
    if (s_enc == BF_ENC_UTF16LE || s_enc == BF_ENC_UTF16BE) {
        size_t i = got;
        while (i >= 2) {
            uint16_t u = (s_enc == BF_ENC_UTF16LE)
                ? (uint16_t)(buf[i - 2] | (uint16_t)(buf[i - 1] << 8))
                : (uint16_t)((buf[i - 2] << 8) | buf[i - 1]);
            if (u == '\n' || u == '\r') { nls++; i -= 2; }
            else break;
        }
    } else {
        size_t i = got;
        while (i > 0) {
            uint8_t c = buf[i - 1];
            if (c == '\n') { nls++; i--; }
            else if (c == '\r') { nls++; i--; if (i > 0 && buf[i - 1] == '\n') i--; }
            else break;
        }
    }
    return nls >= 2;
}
static bool book_page_para(uint32_t off) {
    if (off == s_para_off) return s_para_val;
    s_para_off = off;
    s_para_val = book_page_para_compute(off);
    return s_para_val;
}

/* 目录先行预扫描: 不等完整分页, 先用快速逐行扫描识别所有章节标题行, 尽快填满章节目录.
 * 与分页索引解耦 (不做排版分页, 只按换行切行 + toc_match), 大幅提速"目录生成".
 * 复用 idx_chapters_append (含锁/上限/截断回退). 仅支持 UTF-8 / GBK (UTF-16 少见, 交分页后台). */
static void book_toc_pre_scan(FILE *fp) {
    if (!fp || s_src_size == 0) return;
    if (s_enc == BF_ENC_UTF16LE || s_enc == BF_ENC_UTF16BE) return;
    bool gbk = (s_enc == BF_ENC_GBK);
    /* 用堆分配读缓冲 (栈上放 32KB 会溢出任务栈导致崩溃) */
    uint8_t *chunk = heap_caps_malloc(BOOK_SCAN_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!chunk) { ESP_LOGW(TAG, "目录预扫描缓冲分配失败, 跳过"); return; }
    /* 目录以预扫描结果为准: 清空分页同步首段可能已追加入的重复章节 (避免重复目录项) */
    if (s_idx_mutex) xSemaphoreTake(s_idx_mutex, portMAX_DELAY);
    s_chapter_count = 0;
    if (s_idx_mutex) xSemaphoreGive(s_idx_mutex);
    uint32_t off = s_file_start;          /* 跳过 BOM */
    size_t carry = 0;                     /* 跨块残留半行 */
    int64_t t0 = esp_timer_get_time();
    uint32_t hits = 0;
    uint32_t throt = 0;
    uint32_t di_第 = 0, di_m = 0, di_lines = 0;   /* 诊断计数 */
    while (off < s_src_size && !s_idx_stop) {
        /* 每块最多读到缓冲尾部, 防止跨块残留(carry)大到把 fread 写越界到堆, 腐蚀 PSRAM 堆 → 后续 free 崩溃 */
        uint32_t want = s_src_size - off;
        if (carry < BOOK_SCAN_CHUNK) {
            uint32_t room = BOOK_SCAN_CHUNK - (uint32_t)carry;
            if (want > room) want = room;
        } else {
            want = 0;   /* 单行超过整个缓冲(>32KB 无换行): 无法安全预扫描, 结束, 目录交兜底分节 */
        }
        if (want == 0) break;
        if (fseek(fp, (long)off, SEEK_SET) != 0) break;
        size_t got = fread(chunk + carry, 1, want, fp);
        if (got == 0) break;
        size_t len = carry + got;
        size_t line_start = 0;
        for (size_t i = 0; i < len; i++) {
            if (chunk[i] == '\n') {
                di_lines++;
                /* 行范围 [line_start, i) 去除行尾 \r */
                size_t e = i;
                if (e > line_start && chunk[e - 1] == '\r') e--;
                if (e > line_start) {
                    const uint8_t *ln = chunk + line_start;
                    int ll = (int)(e - line_start);
                    /* 诊断: 先剥行首空格, 判是否「第」开头, 打印原始字节 */
                    int sp = 0;
                    while (sp < ll && (ln[sp] == ' ' || ln[sp] == '\t' || ln[sp] == '\r')) sp++;
                    if (!gbk && ll - sp >= 3 && ln[sp] == 0xE7 && ln[sp + 1] == 0xAC && ln[sp + 2] == 0xAC && di_第 < 8) {
                        di_第++;
                        ESP_LOGI(TAG, "DIAG 第line%u ll=%d sp=%d hex=%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X",
                                 (unsigned)(di_第 - 1), ll, sp,
                                 (unsigned)ln[sp], (unsigned)ln[sp+1], (unsigned)ln[sp+2], (unsigned)ln[sp+3],
                                 (unsigned)ln[sp+4], (unsigned)ln[sp+5], (unsigned)ln[sp+6], (unsigned)ln[sp+7],
                                 (unsigned)ln[sp+8], (unsigned)ln[sp+9], (unsigned)ln[sp+10], (unsigned)ln[sp+11]);
                    }
                    if (toc_match(ln, ll, gbk)) {
                        /* 标题去行首空格 (存干净的「第一卷/第一节:x」而非含缩进) */
                        const uint8_t *tl = ln;
                        int tll = ll;
                        while (tll > 0 && (tl[0]==' ' || tl[0]=='\t' || tl[0]=='\r')) { tl++; tll--; }
                        /* 屏蔽书籍简介类噪声 */
                        bool is_intro = (tll == 6 && memcmp(tl, "\xE7\xAE\x80\xE4\xBB\x8B", 6) == 0); /* 简介 */
                        /* 屏蔽裸「第X」(第+单数字, 无 卷/节/章 后缀) */
                        bool is_bare = (tll == 6 && tl[0]==0xE7 && tl[1]==0xAC && tl[2]==0xAC &&
                                        utf8_num_word(tl + 3));
                        if (!is_intro && !is_bare) {
                            if (di_m < 5) {
                                di_m++;
                                ESP_LOGI(TAG, "DIAG 命中%u len=%d", (unsigned)(di_m - 1), tll);
                            }
                            idx_chapters_append(off + (uint32_t)(line_start + (tl - ln)), tl,
                                                (uint8_t)(tll > 30 ? 30 : tll));
                            hits++;
                        }
                    }
                }
                line_start = i + 1;
            }
        }
        /* 保留不完整尾行到下一块 (carry 恒 < BOOK_SCAN_CHUNK, 不越界) */
        carry = len - line_start;
        if (carry) memmove(chunk, chunk + line_start, carry);
        off += want;
        /* 温柔让出 CPU+SD: 每 2 块 yield 一次, 避免后台预扫描霸占读卡, 卡住首屏/翻页 */
        if ((++throt & 0x1) == 0) vTaskDelay(pdMS_TO_TICKS(1));
    }
    /* 处理文件末尾残行 */
    if (carry > 0 && toc_match(chunk, (int)carry, gbk)) {
        idx_chapters_append(off, chunk, (uint8_t)carry);
        hits++;
    }
    heap_caps_free(chunk);
    int64_t dt = esp_timer_get_time() - t0;
    ESP_LOGI(TAG, "目录预扫描完成: %lu 章, 共%lu 行, 命中%lu, 耗时 %lld ms",
             (unsigned long)s_chapter_count, (unsigned long)di_lines, (unsigned long)hits, (long long)dt / 1000);
    (void)hits;
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
    s_idx.at_para = book_page_para(s_idx.cur);   /* 续扫时按页前空行恢复段落状态 */
    /* 目录预扫描已移至后台索引任务 (book_index_task), 主线程不做整本扫描, 避免打开大书卡死 */
    uint32_t first = s_file_start + BOOK_FIRST_CHUNK;
    if (first > s_src_size) first = s_src_size;
    if (book_scan_run(&s_idx, first) != 0) return -1;
    s_indexed_bytes = s_idx.cur;
    if (s_idx.cur >= s_src_size) {
        s_index_done = true;
        book_toc_fallback();
        book_save_sidecar();
        return 0;
    }
    s_idx_stop = false;
    s_idx_task = NULL;
    if (xTaskCreate(book_index_task, "book_idx", 8192, NULL, 1, &s_idx_task) != pdPASS) {
        /* 后台任务创建失败: 仅同步扫书首一小段, 保证至少第1页可显示 (有界且快, 不触发任务看门狗) */
        ESP_LOGW(TAG, "后台索引任务创建失败, 仅同步扫书首");
        uint32_t first = s_file_start + BOOK_FIRST_CHUNK;
        if (first > s_src_size) first = s_src_size;
        if (book_scan_run(&s_idx, first) != 0) return -1;
        s_indexed_bytes = s_idx.cur;
        s_index_done = (s_idx.cur >= s_src_size);
        if (s_index_done) { book_toc_fallback(); book_save_sidecar(); }
    }
    /* 主线程不再做任何同步索引: 立即返回, 用户先进书先看, 剩余分页/目录由后台任务分步完成 */
    return 0;
}

/* 等待索引覆盖到第 p 页 (超时/出错返回) */
static void book_wait_indexed_page(uint32_t p) {
    uint32_t t0 = xTaskGetTickCount();
    while (!s_index_done && !s_index_error && s_page_count <= p &&
           (uint32_t)(xTaskGetTickCount() - t0) < pdMS_TO_TICKS(4000)) {
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
    s_toc_prescanned = false;   /* 需要重新预扫描目录 */
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
    uint32_t want = need + 4096;                /* 按需读 + 余量, 避免整窗 */
    if (want > BOOK_MAX_WIN_READ) want = BOOK_MAX_WIN_READ;   /* 单次渲染读上限(快) */
    if (want > BOOK_WIN_SIZE) want = BOOK_WIN_SIZE;
    if (want > s_src_size - start) want = s_src_size - start;
    if (want < 256) want = 256;
    int64_t t0 = esp_timer_get_time();
    if (fseek(s_fp, (long)start, SEEK_SET) != 0) return false;
    size_t n = fread(s_win, 1, want, s_fp);
    int64_t dt = esp_timer_get_time() - t0;
    if (dt > 20000) {
        ESP_LOGW(TAG, "窗口读取 %u B 耗时 %lld ms (start=%u)", (unsigned)n, (long long)dt / 1000, start);
    }
    s_win_base = start;
    s_win_len = (uint32_t)n;
    return (s_win_len > 0);   /* 读到数据即可渲染 (粗分页页较大时按需读上限内先显示, 不整页空白) */
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

/* 两段对齐: 把本行剩余空间均匀分到字间距, 保证左右边距一致.
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
        /* 底部进度条: 左下角百分比(阅读进度), 右下角本次阅读时长 */
        uint32_t cur_off = (s_page < s_page_count) ? s_page_off[s_page] : s_src_size;
        uint32_t pct = s_src_size ? (cur_off * 100 / s_src_size) : 100;
        if (pct > 100) pct = 100;
        char pct_str[8];
        snprintf(pct_str, sizeof(pct_str), "%u%%", (unsigned)pct);
        /* 本次阅读时长: 会话开始至今 (mm:ss, 超过 99 分用 h:mm) */
        uint32_t elapsed = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS) - s_open_tick_ms;
        elapsed /= 1000;
        char dur_str[16];
        uint32_t min = elapsed / 60, sec = elapsed % 60;
        if (min >= 60) snprintf(dur_str, sizeof(dur_str), "%lu:%02lu:%02lu", (unsigned long)(min / 60), (unsigned long)(min % 60), (unsigned long)sec);
        else           snprintf(dur_str, sizeof(dur_str), "%lu:%02lu", (unsigned long)min, (unsigned long)sec);
        int py = ST7305_HEIGHT - font_book_ascii_h() - 2;
        /* 左下角: 百分比 (从左边距起) */
        int px_left = book_margin();
        for (int i = 0; pct_str[i]; i++) {
            book_glyph_t g;
            if (font_book_glyph_ascii((uint8_t)pct_str[i], &g))
                st7305_blit_1bit(lcd, px_left + i * font_book_ascii_w(), py, g.w, g.h, g.bitmap);
        }
        /* 右下角: 阅读时长 */
        int dw = (int)strlen(dur_str) * font_book_ascii_w();
        int px_right = ST7305_WIDTH - book_margin() - dw;
        for (int i = 0; dur_str[i]; i++) {
            book_glyph_t g;
            if (font_book_glyph_ascii((uint8_t)dur_str[i], &g))
                st7305_blit_1bit(lcd, px_right + i * font_book_ascii_w(), py, g.w, g.h, g.bitmap);
        }
    }

    uint32_t start = 0, end = s_src_size;
    if (s_idx_mutex) xSemaphoreTake(s_idx_mutex, portMAX_DELAY);
    uint32_t pg = s_page;
    if (pg < s_page_count) start = s_page_off[pg];
    if (pg + 1 < s_page_count) end = s_page_off[pg + 1];
    if (s_idx_mutex) xSemaphoreGive(s_idx_mutex);
    if (s_page_count == 0) { start = s_file_start; end = start + 8192; if (end > s_src_size) end = s_src_size; }
    uint32_t pagelen = end - start;
    if (!book_load_win(start, pagelen + 8)) return;
    if (!s_rwin || !s_rbrk || !s_rscratch) return;

    int n = render_decode_window(s_rwin, start - s_win_base, BF_MAX_WIN);
    if (n == 0) return;
    int64_t t_flow0 = esp_timer_get_time();
    bf_breaks(s_rwin, n, s_enc, s_rbrk, s_rscratch, FLOW_SCRATCH_SIZE);
    bool para = book_page_para(start);           /* 页首是否段落首行 */
    int indent_px = s_indent ? 2 * (font_book_cell_w() + book_gap()) : 0;
    uint8_t line_indent[BF_MAX_LINES];
    int line_end[BF_MAX_LINES], lc, boundary;
    bf_layout(s_rwin, n, s_rbrk, book_rows(), book_line_max(),
              font_book_ascii_w() + book_gap(), font_book_cell_w() + book_gap(),
              &para, indent_px, line_end, line_indent, &lc, &boundary);
    int64_t t_flow1 = esp_timer_get_time();
    int actual = lc; if (actual > book_rows()) actual = book_rows();
    int x0, y0; book_center(&x0, &y0, book_line_max(), actual, false);   /* V1.1.0 居中 */
    int prev = 0, y = y0;
    for (int k = 0; k < lc && k < book_rows(); k++) {
        int endk = line_end[k];
        int ind = line_indent[k] ? indent_px : 0;
        int lm = book_line_max() - ind;
        int gaps, per_gap, rem;
        bool justify;
        line_justify(s_rwin, prev, endk, lm, &gaps, &per_gap, &rem, &justify);
        int x = ind, gi = 0;
        for (int j = prev; j < endk; j++) {
            const bf_ch_t *ch = &s_rwin[j];
            if (ch->kind == BF_CH_TAB) { x += font_book_cell_w() + book_gap(); gi++; continue; }
            if (ch->kind == BF_CH_NEWLINE || ch->kind == BF_CH_SKIP) continue;
            int w = ch_width(ch);
            draw_decoded_char(lcd, x0 + x, y, ch);
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
        /* 底部进度条: 左下角百分比(阅读进度), 右下角本次阅读时长 */
        uint32_t cur_off = (s_page < s_page_count) ? s_page_off[s_page] : s_src_size;
        uint32_t pct = s_src_size ? (cur_off * 100 / s_src_size) : 100;
        if (pct > 100) pct = 100;
        char pct_str[8];
        snprintf(pct_str, sizeof(pct_str), "%u%%", (unsigned)pct);
        uint32_t elapsed = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS) - s_open_tick_ms;
        elapsed /= 1000;
        char dur_str[16];
        uint32_t min = elapsed / 60, sec = elapsed % 60;
        if (min >= 60) snprintf(dur_str, sizeof(dur_str), "%lu:%02lu:%02lu", (unsigned long)(min / 60), (unsigned long)(min % 60), (unsigned long)sec);
        else           snprintf(dur_str, sizeof(dur_str), "%lu:%02lu", (unsigned long)min, (unsigned long)sec);
        int py = BOOK_PORTRAIT_H - font_book_ascii_h() - 2;
        /* 左下角: 百分比 */
        int px_left = book_margin();
        for (int i = 0; pct_str[i]; i++) {
            book_glyph_t g;
            if (font_book_glyph_ascii((uint8_t)pct_str[i], &g)) pfb_blit(fb, px_left + i * font_book_ascii_w(), py, &g);
        }
        /* 右下角: 阅读时长 */
        int dw = (int)strlen(dur_str) * font_book_ascii_w();
        int px_right = BOOK_PORTRAIT_W - book_margin() - dw;
        for (int i = 0; dur_str[i]; i++) {
            book_glyph_t g;
            if (font_book_glyph_ascii((uint8_t)dur_str[i], &g)) pfb_blit(fb, px_right + i * font_book_ascii_w(), py, &g);
        }
    }

    uint32_t start = 0, end = s_src_size;
    if (s_idx_mutex) xSemaphoreTake(s_idx_mutex, portMAX_DELAY);
    uint32_t pg = s_page;
    if (pg < s_page_count) start = s_page_off[pg];
    if (pg + 1 < s_page_count) end = s_page_off[pg + 1];
    if (s_idx_mutex) xSemaphoreGive(s_idx_mutex);
    /* 无页表(首次打开/换布局): 立即渲染文件开头第一屏, 后台索引慢慢补分页 */
    if (s_page_count == 0) { start = s_file_start; end = start + 8192; if (end > s_src_size) end = s_src_size; }
    uint32_t pagelen = end - start;
    if (!book_load_win(start, pagelen + 8)) return;
    if (!s_rwin || !s_rbrk || !s_rscratch) return;

    int n = render_decode_window(s_rwin, start - s_win_base, BF_MAX_WIN);
    if (n == 0) return;
    bf_breaks(s_rwin, n, s_enc, s_rbrk, s_rscratch, FLOW_SCRATCH_SIZE);
    bool para = book_page_para(start);           /* 页首是否段落首行 */
    int indent_px = s_indent ? 2 * (font_book_cell_w() + book_gap()) : 0;
    uint8_t line_indent[BF_MAX_LINES];
    int line_end[BF_MAX_LINES], lc, boundary;
    bf_layout(s_rwin, n, s_rbrk, book_rows(), book_line_max(),
              font_book_ascii_w() + book_gap(), font_book_cell_w() + book_gap(),
              &para, indent_px, line_end, line_indent, &lc, &boundary);
    int actual = lc; if (actual > book_rows()) actual = book_rows();
    int x0, y0; book_center(&x0, &y0, book_line_max(), actual, true);       /* V1.1.0 居中 */
    int prev = 0, y = y0;
    for (int k = 0; k < lc && k < book_rows(); k++) {
        int endk = line_end[k];
        int ind = line_indent[k] ? indent_px : 0;
        int lm = book_line_max() - ind;
        int gaps, per_gap, rem;
        bool justify;
        line_justify(s_rwin, prev, endk, lm, &gaps, &per_gap, &rem, &justify);
        int x = ind, gi = 0;
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
            if (ok) pfb_blit(fb, x0 + x, yy, &g);
            else {
                for (int i = 0; i < 16; i++) {
                    pfb_px(fb, x0 + x, yy + i, 1);
                    pfb_px(fb, x0 + x + 15, yy + i, 1);
                    pfb_px(fb, x0 + x + i, yy, 1);
                    pfb_px(fb, x0 + x + i, yy + 15, 1);
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

/* 竖屏旋转已交驱动 st7305_set_rotation+flush 统一处理 (与书架同套), 此软件旋转已删除. */

/* ============ 阅读菜单 ============ */

static const char *menu_str(const char *utf8, const char *gbk) {
    return (s_enc == BF_ENC_GBK) ? gbk : utf8;
}

#define M_READ_MENU   menu_str("\xE9\x98\x85\xE8\xAF\xBB\xE8\x8F\x9C\xE5\x8D\x95", "\xD4\xC4\xB6\xC1\xB2\xCB\xB5\xA5")
#define M_TOC         menu_str("\xE7\xAB\xA0\xE8\x8A\x82\xE7\x9B\xAE\xE5\xBD\x95", "\xD5\xC2\xBD\xDA\xC4\xBF\xC2\xBC")  /* 章节目录 */
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
#define M_FONT_SEL    menu_str("\xE5\xAD\x97\xE4\xBD\x93\xE9\x80\x89\xE6\x8B\xA9", "\xD7\xD6\xCC\xE5\xD1\xA1\xD4\xF1")  /* 字体选择 */
#define M_EMB_FS      menu_str("\xE5\x86\x85\xE5\xB5\x8C\xE4\xBB\xBF\xE5\xAE\x8B", "\xC4\xDA\xC7\xB6\xB7\xC2\xCB\xCE")  /* 内嵌仿宋 */
#define M_EMB_HT      menu_str("\xE5\x86\x85\xE5\xB5\x8C\xE9\xBB\x91\xE4\xBD\x93", "\xC4\xDA\xC7\xB6\xBA\xDA\xCC\xE5")  /* 内嵌黑体 */
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

/* 目录分页 (50章/页): 当前页 global 起始下标、当前页章数、总页数 */
static uint32_t toc_page_base(void)   { return s_toc_page * TOC_PER_PAGE; }
static uint32_t toc_pages_count(void) {
    if (s_chapter_count == 0) return 1;
    return (s_chapter_count + TOC_PER_PAGE - 1) / TOC_PER_PAGE;
}
static uint32_t toc_page_items(void) {
    uint32_t base = toc_page_base();
    if (base >= s_chapter_count) return 0;
    uint32_t n = s_chapter_count - base;
    return (n > TOC_PER_PAGE) ? (uint32_t)TOC_PER_PAGE : n;
}
static const char *toc_display_title(uint32_t gi);   /* 章题转数字 (定义在后) */

static int menu_item_count(void) {
    switch (s_menu) {
        case BM_MENU: return 4;   /* 目录/添加书签/书签列表/返回书库 (旋转方向统一由书架"阅读优化"设置) */
        case BM_TOC:  return (s_chapter_count == 0) ? 1 : (int)toc_page_items();
        case BM_BMKS: return (s_bm_count == 0) ? 1 : (int)s_bm_count + 1;   /* ... + 清空 */
        case BM_FONT: return s_sd_font_count + 2;  /* SD字体 + 内嵌仿宋 + 内嵌黑体 */
        default: return 0;
    }
}

static const char *menu_item_text(int i) {
    switch (s_menu) {
        case BM_MENU:
            /* 目录/添加书签/书签列表/返回书库 (旋转方向统一由书架"阅读优化"设置) */
            switch (i) {
                case 0: return M_TOC;
                case 1: return M_ADD_BM;
                case 2: return M_BM_LIST;
                case 3: return M_BACK_LIB;
            }
            return "";
        case BM_TOC:
            if (s_chapter_count == 0) return M_NO_TOC;
            { uint32_t gi = toc_page_base() + (uint32_t)i;
              return (gi < s_chapter_count) ? toc_display_title(gi) : ""; }
        case BM_BMKS:
            if (s_bm_count == 0) return M_NO_BM;
            if (i < (int)s_bm_count) return s_bms[i].title;
            return M_CLEAR_BM;
        case BM_FONT:
            if (i < s_sd_font_count) return s_sd_fonts[i];
            if (i == s_sd_font_count) return M_EMB_FS;
            return M_EMB_HT;
        default: return "";
    }
}

static const char *menu_title_text(void) {
    switch (s_menu) {
        case BM_MENU: return "";   /* V1.0.68: 不显示"阅读菜单"标题 */
        case BM_TOC:  return M_TOC;
        case BM_BMKS: return M_BM_LIST;
        case BM_FONT: return M_FONT_SEL;
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

/* 阅读菜单几何 (绘制与触摸共用同一套, 彻底消除点击偏移):
 * 固定宽 200, 高度随内容自适应; 结构: 顶距6 + 标题区(可选26) + 列表行(32px) + 底部返回行44 + 底距6. */
static void book_menu_geom(int *bx, int *by, int *bw, int *bh, int *ty, int *content_bot, int *foot_y) {
    int W = book_is_portrait() ? BOOK_PORTRAIT_W : ST7305_WIDTH;
    int H = book_is_portrait() ? BOOK_PORTRAIT_H : ST7305_HEIGHT;
    const char *title = menu_title_text();
    int title_h = title[0] ? 26 : 0;
    int n = menu_item_count();
    int w, h;
    if (s_menu == BM_TOC) {
        /* 目录 = 超长窗口: 占满整屏(扣除边距), 一次拖动可看/选更多章节 */
        w = W - 8;
        h = H - 8;
    } else {
        w = 200;
        h = 6 + title_h + n * 32 + 44 + 6;
        if (h > H - 8) h = H - 8;   /* 长列表超高时裁到屏内, 内部滚动 */
        if (h < 120) h = 120;
    }
    *bw = w; *bh = h;
    *bx = (W - w) / 2;
    *by = (H - h) / 2;
    *ty = *by + 6 + title_h;
    *foot_y = *by + h - 44;                        /* 底部"返回"行顶 */
    *content_bot = *foot_y - ((s_menu == BM_TOC) ? TOC_PAGER_H : 0);  /* 目录预留页码条 */
}

/* ---- 章页标题整理: "第一千一百一十一章" -> "第1111章" (省宽度) ---- */
static const uint32_t k_cnu[] = { 0xE5A3B9,0xE8B4B0,0xE58F81,0xE88286,0xE4BC8D,0xE99986,0xE69F92,0xE68D8C,0xE78E96 }; /* 壹贰叁肆伍陆柒捌玖 */
static const uint32_t k_cn[] = { 0xE99BB6,0xE4B880,0xE4BA8C,0xE4B889,0xE59B9B,0xE4BA94,0xE585AD,0xE4B883,0xE585AB,0xE4B99D, /* 零一二三四五六七八九 */
                                 0xE4B8A4 /*两*/ ,0xE58097 /*两*/ };
static bool cn_is_digit(const uint8_t*p, int *d) {
    uint32_t c=((uint32_t)p[0]<<16)|((uint32_t)p[1]<<8)|p[2];
    for (int i=0;i<10;i++) if(c==k_cn[i]){*d=i;return true;}
    for (int i=0;i<9;i++)  if(c==k_cnu[i]){*d=i+1;return true;}
    if(c==0xE4B8A4||c==0xE58097){*d=2;return true;}   /* 两 */
    return false;
}
/* 解析 "第X章"(X=中文/数字) -> 数值, 失败返回 -1 */
static long toc_chap_num(const uint8_t*t) {
    if (!(t[0]==0xE7&&t[1]==0xAC&&t[2]==0xAC)) return -1;   /* 第 */
    t += 3;
    long total=0, sec=0, num=0; bool any=false;
    while (t[0]) {
        int d; uint32_t c=((uint32_t)t[0]<<16)|((uint32_t)t[1]<<8)|t[2];
        if (cn_is_digit(t, &d)) { num=d; any=true; t+=3; continue; }
        if (c==0xE58D81||c==0xE68B // 十/拾
            )              { sec += (num?num:1)*10;  num=0; any=true; t+=3; continue; }
        else if (c==0xE799BE||c==0xE4BDB0) { sec += (num?num:1)*100; num=0; any=true; t+=3; continue; }
        else if (c==0xE58D83||c==0xE8B4AC) { sec += (num?num:1)*1000; num=0; any=true; t+=3; continue; }
        else if (c==0xE4B887) { total += (sec+num)*10000; sec=num=0; any=true; t+=3; continue; }
        else break;
    }
    if (!any) return -1;
    long v = total + sec + num;
    return v;
}
/* 目录显示标题: 数字章转 "第N章", 再拼接原章节名称 (省去长中文数字, 保留原名) */
static char s_toc_title_buf[64];
static const char *toc_display_title(uint32_t gi) {
    const uint8_t*raw = (const uint8_t*)s_chapters[gi].title;
    /* 后缀字 (章回节卷部话集篇) */
    static const uint32_t suf[] = {0xE7ABA0,0xE59B9E,0xE88A82,0xE58DB7,0xE983A8,0xE8AF9D,0xE99B86,0xE7AF87};
    long v = toc_chap_num(raw);
    int used = 0;
    if (v >= 0) {
        /* 找后缀字 */
        const uint8_t*p=raw+3;
        /* 跳过数字字符得到后缀位置 */
        while (p[0]) {
            int d; uint32_t c=((uint32_t)p[0]<<16)|((uint32_t)p[1]<<8)|p[2];
            if (cn_is_digit(p,&d)) { p+=3; continue; }
            if (c==0xE58D81||c==0xE68B||c==0xE799BE||c==0xE4BDB0||c==0xE58D83||c==0xE8B4AC||c==0xE4B887){ p+=3; continue; }
            break;
        }
        int sl=0;
        for (int i=0;i<8;i++){ uint32_t c=((uint32_t)p[0]<<16)|((uint32_t)p[1]<<8)|p[2];
            if (c==suf[i]){ sl=3; break; } }
        if (sl==3) {
            used=(int)(p+3-raw);
            int hn=snprintf(s_toc_title_buf,sizeof(s_toc_title_buf),"\xE7\xAC\xAC%ld",v);
            char sufs[4]={(char)p[0],(char)p[1],(char)p[2],0};
            hn+=snprintf(s_toc_title_buf+hn,sizeof(s_toc_title_buf)-hn,"%s",sufs);
            snprintf(s_toc_title_buf+hn,sizeof(s_toc_title_buf)-hn,"%.*s",(int)sizeof(s_toc_title_buf)-hn,(const char*)(raw+used));
            return s_toc_title_buf;
        }
    }
    snprintf(s_toc_title_buf,sizeof(s_toc_title_buf),"%.*s",(int)sizeof(s_toc_title_buf)-1,(const char*)raw);
    return s_toc_title_buf;
}

/* 目录页码条按钮布局 (绘制与触摸共用). hit: -1=上一页 <, -2=下一页 >, 0..=页码 */
#define TOC_PAGER_MAX 16
static int toc_pager_layout(int bx, int bw, int top, int out[][3]) {
    (void)top;
    uint32_t P = toc_pages_count(); if (P == 0) P = 1;   /* 目录未读完也先画 < 页 >, 中间随加载填充 */
    int asc = font_book_ascii_w();
    int gap = 4;
    int idx = 0;
    /* < 贴最左 */
    int left = bx + 1;
    out[idx][0] = -1; out[idx][1] = left; out[idx][2] = left + asc; idx++;
    /* > 贴最右 (内缩 2px 避免被边缘裁掉) */
    int asc2 = asc; if (asc2 > 14) asc2 = 14;   /* >/< 用较小宽度, 确保在窗口内 */
    int right = bx + bw - 2 - asc2;
    out[idx][0] = -2; out[idx][1] = right; out[idx][2] = right + asc2; idx++;
    /* 页码在中间均匀铺满 */
    int midL = left + asc + gap;
    int midR = right - gap;
    char buf[16]; snprintf(buf,sizeof(buf),"%u",(unsigned)(P-1)); int digits=(int)strlen(buf);
    int per = digits*asc + gap;
    int span = midR - midL;
    int cnt = span / per; if (cnt < 1) cnt = 1;
    uint32_t cur = s_toc_page;
    uint32_t first = (cur >= (uint32_t)(cnt/2)) ? (cur - cnt/2) : 0;
    uint32_t last = first + (uint32_t)cnt - 1;
    if (last >= P) { last = P - 1; first = (last >= (uint32_t)cnt-1) ? (last-(uint32_t)cnt+1) : 0; }
    int total_w = (int)(last-first+1) * per - gap;
    int xs = midL + (span - total_w)/2;           /* 居中铺满 */
    for (uint32_t p = first; p <= last && idx < TOC_PAGER_MAX; p++) {
        out[idx][0]=(int)p; out[idx][1]=xs; out[idx][2]=xs+digits*asc; xs+=per; idx++;
    }
    return idx;
}
static void toc_pager_draw(st7305_handle_t *lcd, int bx, int by, int bw, int top) {
    if (s_chapter_count == 0 && !s_open) return;   /* 无章节时也画 < 页 >, 仅不影响元素 */
    int b[TOC_PAGER_MAX][3]; int n = toc_pager_layout(bx, bw, top, b);
    int y = top + (TOC_PAGER_H - font_book_ascii_h()) / 2;
    for (int i = 0; i < n; i++) {
        bool cur_page = (b[i][0] == (int)s_toc_page);
        if (cur_page)
            for (int yy = top; yy < top + TOC_PAGER_H; yy++)
                for (int xx = b[i][1]; xx < b[i][2]; xx++)
                    menu_fb_px(lcd, xx, yy, true);
        if (b[i][0] == -1) {
            menu_draw_text(lcd, b[i][1], y, "<", cur_page);
        } else if (b[i][0] == -2) {
            menu_draw_text(lcd, b[i][1], y, ">", cur_page);
        } else {
            char txt[16]; snprintf(txt,sizeof(txt),"%u",(unsigned)b[i][0]+1);
            menu_draw_text(lcd, b[i][1], y, txt, cur_page);
        }
    }
    for (int x = by + 1; x <= by + bw - 2; x++) menu_fb_px(lcd, x, top - 1, true);
}

/* 标准列表弹窗外观: 固定宽 200, 高度自适应, 24px 字, 标题+列表+底部返回, 选中反色 */
static void draw_reader_menu(st7305_handle_t *lcd) {
    int count = menu_item_count();
    if (count <= 0) return;
    int bx, by, bw, bh, ty, content_bot, foot_y;
    book_menu_geom(&bx, &by, &bw, &bh, &ty, &content_bot, &foot_y);
    const int ROW_H = 32;
    const char *title = menu_title_text();
    int vis = (content_bot - ty) / ROW_H; if (vis < 1) vis = 1;
    if (s_menu_sel < s_menu_scroll) s_menu_scroll = s_menu_sel;
    if (s_menu_sel >= s_menu_scroll + (uint32_t)vis) s_menu_scroll = s_menu_sel - vis + 1;
    /* 像素偏移合法性: 上界 power */
    if ((int)s_menu_pix > ROW_H - 1) s_menu_pix = 0;
    if ((int)s_menu_pix < 0) s_menu_pix = 0;
    int shown = count - (int)s_menu_scroll;
    if (shown > vis) shown = vis;
    if ((int)s_menu_scroll + shown >= count && shown > 0) shown = count - (int)s_menu_scroll;
    if (shown < 0) shown = 0;

    font_book_select(s_fontstyle, 24);  /* 列表标准 24px */
    /* 实心白底 + 2px 粗黑边框 */
    for (int y = by; y <= by + bh - 1; y++)
        for (int x = bx; x <= bx + bw - 1; x++)
            menu_fb_px(lcd, x, y, false);
    for (int k = 0; k < 2; k++) {
        for (int x = bx + k; x <= bx + bw - 1 - k; x++) { menu_fb_px(lcd, x, by + k, true); menu_fb_px(lcd, x, by + bh - 1 - k, true); }
        for (int y = by + k; y <= by + bh - 1 - k; y++) { menu_fb_px(lcd, bx + k, y, true); menu_fb_px(lcd, bx + bw - 1 - k, y, true); }
    }
    /* 标题 (居中, 下方分隔线) */
    if (title[0]) {
        int tw = menu_text_width(title);
        int tx = bx + (bw - tw) / 2;
        if (tx < bx + 2) tx = bx + 2;
        menu_draw_text(lcd, tx, by + 2, title, false);
        for (int x = bx + 2; x <= bx + bw - 3; x++) menu_fb_px(lcd, x, by + 24, true);
    }
    /* 列表项 (像素级跟手: 整体上移 s_menu_pix 像素) */
    {
        int pix = s_menu_pix;
        /* 需多画一行以覆盖像素偏移在底部露出的内容 */
        int rows = shown + (pix > 0 ? 1 : 0);
        for (int k = 0; k < rows; k++) {
            int idx = (int)s_menu_scroll + k;
            if (idx < 0 || idx >= count) continue;
            bool sel = (idx == (int)s_menu_sel);
            int row_y = ty + k * ROW_H - pix;
            if (row_y + ROW_H <= ty || row_y >= content_bot) continue;   /* 完全在窗口外 */
            /* 可见裁剪 */
            int y0 = row_y < ty ? ty : row_y;
            int y1 = row_y + ROW_H > content_bot ? content_bot : row_y + ROW_H;
            if (sel)
                for (int yy = y0; yy < y1; yy++)
                    for (int xx = bx + 2; xx < bx + bw - 2; xx++)
                        menu_fb_px(lcd, xx, yy, true);
            const char *text = menu_item_text(idx);
            int tw = menu_text_width(text);
            int tx = (s_menu == BM_TOC) ? (bx + 3) : (bx + (bw - tw) / 2);
            if (tx < bx + 2) tx = bx + 2;
            int texty = row_y + (ROW_H - 24) / 2;
            if (texty < ty) texty = ty;
            if (texty < content_bot) menu_draw_text(lcd, tx, texty, text, sel);
        }
    }
    /* 右侧 Windows 式滚动条 (目录/书签): 20px 宽轨道 + 可拖滑块 */
    if ((s_menu == BM_TOC || s_menu == BM_BMKS) && count > 0) {
        int track_h = content_bot - ty;
        int sb_x = bx + bw - SCROLLBAR_W;              /* 滚动条左端 */
        /* 轨道: 浅色凹槽 + 左右边线 */
        for (int yy = ty; yy <= content_bot; yy++)
            for (int xx = sb_x; xx <= bx + bw - 2; xx++)
                menu_fb_px(lcd, xx, yy, false);
        for (int yy = ty; yy <= content_bot; yy++) {
            menu_fb_px(lcd, sb_x, yy, true);
            menu_fb_px(lcd, sb_x + SCROLLBAR_W - 1, yy, true);
        }
        /* 滑块: 实心方块, 比例=可视/总数, 位置=滚动比例 */
        int thumb_h = track_h * vis / count;
        if (thumb_h < 10) thumb_h = 10;
        if (thumb_h > track_h) thumb_h = track_h;
        int span_max = count - vis;
        int pos = 0;
        if (span_max > 0)
            pos = (int)s_menu_scroll * (track_h - thumb_h) / span_max;
        if (pos < 0) pos = 0;
        if (pos > track_h - thumb_h) pos = track_h - thumb_h;
        for (int yy = ty + pos; yy < ty + pos + thumb_h && yy < content_bot; yy++)
            for (int xx = sb_x; xx <= sb_x + SCROLLBAR_W - 1; xx++)
                menu_fb_px(lcd, xx, yy, true);
    }
    /* 目录: 底部页码条 (位于列表与"返回"之间) */
    if (s_menu == BM_TOC) toc_pager_draw(lcd, bx, by, bw, content_bot);
    /* 底部返回行: 单独一行, 用大字号 (2号) 居中显示 */
    for (int x = bx + 2; x <= bx + bw - 3; x++) menu_fb_px(lcd, x, foot_y, true);
    font_book_select(s_fontstyle, 32);  /* 底部返回: 大字号 32px */
    const char *back = M_BACK;
    int bw2 = menu_text_width(back);
    int by2 = foot_y + (44 - font_book_cell_h()) / 2;
    int bx2 = bx + (bw - bw2) / 2;
    if (bx2 < bx + 2) bx2 = bx + 2;
    menu_draw_text(lcd, bx2, by2, back, false);
    font_book_select(s_fontstyle, 24);  /* 恢复列表字号 24px */

    font_book_select(s_fontstyle, book_reader_size_px(s_fontsize));   /* 恢复阅读字号 */
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
        /* 覆盖层画进竖屏画布, 随正文一起旋转 */
        if (s_exit_confirm) draw_exit_confirm(lcd);
        if (s_menu != BM_READ) draw_reader_menu(lcd);
        /* 旋转全部交驱动底层统一处理 (与书架同套 set_rotation+flush), 消除两套逻辑 */
        st7305_set_rotation(lcd, s_rot, s_pfb);
    } else {
        render_landscape(lcd);
        if (s_exit_confirm) draw_exit_confirm(lcd);
        if (s_menu != BM_READ) draw_reader_menu(lcd);
        if (s_rot == 1 && !s_rot_buf) {
            s_rot_buf = heap_caps_malloc(15000, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }
        st7305_set_rotation(lcd, s_rot, (s_rot == 1) ? s_rot_buf : NULL);
    }
    if (s_night && !s_inverted) {
        st7305_set_inversion(lcd, true);
        s_inverted = true;
    } else if (!s_night && s_inverted) {
        st7305_set_inversion(lcd, false);
        s_inverted = false;
    }
    st7305_flush(lcd);   /* 驱动按当前旋转 (0横 / 1内存180 / 2,3竖屏) 送屏 */
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
        return;   /* 书签已满 (不弹提示) */
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
}

static void book_menu_select(void) {
    int idx = (int)s_menu_sel;
    int count = menu_item_count();
    if (idx < 0 || idx >= count) return;
    switch (s_menu) {
        case BM_MENU:
            switch (idx) {
                case 0: s_menu = BM_TOC; s_menu_sel = 0; s_menu_scroll = 0; break;   /* 章节目录 */
                case 1: book_add_bookmark(); break;                                  /* 添加书签 */
                case 2: s_menu = BM_BMKS; s_menu_sel = 0; s_menu_scroll = 0; break; /* 书签列表 */
                case 3: book_reader_close(); break;                                  /* 返回书库 */
            }
            return;
        case BM_FONT: {
            bool font_changed = false;
            if (idx < s_sd_font_count) {
                /* TF 卡字体: 构造完整路径并加载 */
                char path[128];
                snprintf(path, sizeof(path), "/sdcard/fonts/%s", s_sd_fonts[idx]);
                if (font_book_select_file(path)) {
                    font_changed = true;
                }
            } else if (idx == s_sd_font_count) {
                /* 内嵌仿宋 */
                font_book_select(0, book_reader_size_px(s_fontsize));
                font_changed = true;
            } else if (idx == s_sd_font_count + 1) {
                /* 内嵌黑体 */
                font_book_select(1, book_reader_size_px(s_fontsize));
                font_changed = true;
            }
            if (font_changed) {
                /* 字体变了, 重新分页 */
                book_restart_index();
            }
            s_menu = BM_MENU; s_menu_sel = 0; s_menu_scroll = 0;
            return;
        }
        case BM_TOC:
            if (s_chapter_count == 0) return;
            { uint32_t gi = toc_page_base() + (uint32_t)idx;
              if (gi < s_chapter_count) {
                  uint32_t off = s_chapters[gi].off;
                  /* 等索引扫过该章节偏移, 确保能正确映射到具体页 */
                  if (s_indexed_bytes < off && !s_index_done) {
                      uint32_t t0 = xTaskGetTickCount();
                      while (!s_index_done && !s_index_error && s_indexed_bytes < off &&
                             (uint32_t)(xTaskGetTickCount() - t0) < pdMS_TO_TICKS(5000))
                          vTaskDelay(pdMS_TO_TICKS(10));
                  }
                  uint32_t p = book_page_for_offset(off);
                  book_wait_indexed_page(p);
                  if (!s_index_error && s_page_count > p) { s_page = p; s_menu = BM_READ; }
              } }
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
    font_book_select(s_fontstyle, book_reader_size_px(s_fontsize));   /* 按设置切换字号 */

    /* V1.0.88: 扫描 TF 卡字体 (仅在阅读器内扫描, 不影响开机/菜单) */
    s_sd_font_count = font_book_scan_sd(s_sd_fonts, 16);
    if (s_sd_font_count > 0) {
        ESP_LOGI(TAG, "发现 %d 个 TF 卡字体", s_sd_font_count);
    }

    struct stat st;
    if (stat(path, &st) != 0) {
        ESP_LOGE(TAG, "stat 失败: %s (errno=%d)", path, errno);
        return false;
    }
    uint32_t fsz = (uint32_t)st.st_size;
    if (fsz == 0 || fsz > BOOK_MAX_FILE) {
        ESP_LOGE(TAG, "文件大小非法: %s size=%ld (上限 %u) mode=%o", path,
                 (long)st.st_size, BOOK_MAX_FILE, (unsigned)st.st_mode);
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
    s_toc_prescanned = false;   /* 每次打开都重建完整目录 */
    s_menu = BM_READ;
    s_exit_confirm = false;
    s_menu_msg[0] = 0;

    bool sidecar_loaded = book_load_sidecar();   /* 先载页表, 供偏移恢复 */
    /* 侧车已含完整目录 → 直接用缓存, 不再每次进入重新全本扫描 */
    if (sidecar_loaded && s_chapter_count >= BOOK_TOC_REBUILD_MIN) {
        s_toc_prescanned = true;
        ESP_LOGI(TAG, "目录命中缓存: %lu 章", (unsigned long)s_chapter_count);
    }
    book_load_progress();                          /* 再载进度 */
    if (s_pending_last_off > 0 && s_page_off && s_page_count) {
        /* 始终按字节偏移换算回最近页 (页号在重开后页数不同时会失效) */
        s_loaded_page = book_page_for_offset(s_pending_last_off);
        if (s_loaded_page >= s_page_count) s_loaded_page = s_page_count - 1;
        ESP_LOGI(TAG, "按偏移 %lu 恢复至页 %lu/%lu",
                 (unsigned long)s_pending_last_off, (unsigned long)s_loaded_page, (unsigned long)s_page_count);
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
    } else if (s_chapter_count < BOOK_TOC_REBUILD_MIN) {
        /* 页表完备但目录几乎为空(旧 .idx 未存目录) → 后台重建完整目录, 不阻塞阅读 */
        ESP_LOGI(TAG, "侧边目录过少(%lu 章), 后台重建", (unsigned long)s_chapter_count);
        book_toc_rebuild_start();
    }

    if (s_loaded_page < s_page_count) s_page = s_loaded_page;
    s_last_saved_page = s_page;

    /* 立即进入阅读: 不做任何阻塞等待. 页表未就绪时后台分页填充, 渲染到可用即显示 */

    const char *bn = base_name(path);
    snprintf(s_title, sizeof(s_title), "%.63s", bn);
    char *dot = strrchr(s_title, '.');
    if (dot && strcasecmp(dot, ".txt") == 0) *dot = '\0';

    s_open = true;
    s_open_tick_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);   /* 记录本次阅读起点 */
    input_set_screen_rotation(s_rot);   /* V1.0.68: 触摸跟随当前旋转方向 */
    input_set_swipe_turn(true);          /* 阅读器内横滑翻页 */
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
    input_set_swipe_turn(false);   /* 关闭阅读器横滑翻页 (恢复其它界面拖动) */
    ESP_LOGI(TAG, "阅读器已关闭, 内存已释放");
}

const char *book_reader_title(void) {
    return s_title;
}

bool book_reader_knock_active(void) {
    return false;   /* 敲击翻页功能已删除 (V1.0.64) */
}

void book_reader_set_settings(bool knock, int sens, bool night, bool pagenum, int rot,
                              int fontstyle, int fontsize, int margin, int lineh, int gap,
                              int indent) {
    bool was_portrait = book_is_portrait();
    (void)knock;
    (void)sens;
    s_night = night;
    s_pagenum = pagenum;
    bool layout_changed = (s_open &&
        (fontsize != (int)s_fontsize || margin != (int)s_margin_id ||
         lineh != (int)s_lineh_id || gap != (int)s_gap_id ||
         (indent >= 0 && indent != (int)s_indent)));
    bool style_changed = (s_open && fontstyle >= 0 && fontstyle != (int)s_fontstyle);
    s_fontstyle = (uint8_t)((fontstyle < 0) ? 0 : (fontstyle > 1) ? 1 : fontstyle);
    s_fontsize = (uint8_t)((fontsize < 0) ? 1 : (fontsize > 3) ? 3 : fontsize);   /* 档 0..3 = 20/24/28/32px */
    s_margin_id = (uint8_t)((margin < 0) ? 1 : (margin > 2) ? 2 : margin);
    s_lineh_id = (uint8_t)((lineh < 0) ? 1 : (lineh > 2) ? 2 : lineh);
    s_gap_id = (uint8_t)((gap < 0) ? 0 : (gap > 2) ? 2 : gap);
    if (indent >= 0) s_indent = (indent != 0);
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
            if (layout_changed || style_changed) font_book_select(s_fontstyle, book_reader_size_px(s_fontsize));
            book_restart_index();
        } else {
            s_page = 0;
        }
        /* 旋转后一律关闭菜单回阅读页, 否则菜单挡住正文, 视觉上"没旋转" */
        s_menu = BM_READ;
        s_exit_confirm = false;
        s_menu_msg[0] = 0;
    } else {
        s_rot = new_rot;
        if (layout_changed || style_changed) font_book_select(s_fontstyle, book_reader_size_px(s_fontsize));
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
    /* 刚拖动过菜单列表(含滚动条): 松手产生的首个动作一律吞掉, 保证拖动不触发选中/打开 */
    if (s_menu != BM_READ && s_menu_dragged) {
        s_menu_dragged = false;
        return true;
    }
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
                if (s_menu_sel > 0) { s_menu_sel--; s_menu_pix = 0; }
                return true;
            case BOOK_ACTION_DOWN:
                if (s_menu_sel + 1 < (uint32_t)menu_item_count()) { s_menu_sel++; s_menu_pix = 0; }
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

/* 阅读器二级菜单 (设置/目录/书签) 触摸命中.
 * 与 draw_reader_menu 共用 book_menu_geom, 点击与绘制逐像素对齐 (零偏移). */
static bool book_menu_handle_touch(int x, int y) {
    if (s_menu_dragged) {
        /* 刚拖动过列表 → 这次松手视为拖动结束, 不当作点击跳转 */
        s_menu_dragged = false;
        return true;
    }
    int count = menu_item_count();
    if (count <= 0) return false;
    int bx, by, bw, bh, ty, content_bot, foot_y;
    book_menu_geom(&bx, &by, &bw, &bh, &ty, &content_bot, &foot_y);
    if (x < bx || x > bx + bw - 1 || y < by || y > by + bh - 1) {
        /* 点弹窗外 = 取消/关闭菜单, 回到阅读页 */
        s_menu = BM_READ;
        s_menu_sel = 0;
        s_menu_scroll = 0;
        return true;
    }
    const int ROW_H = 32;
    if (s_menu == BM_TOC && y >= content_bot && y < foot_y) {
        /* 目录底部页码条点按 */
        int b[TOC_PAGER_MAX][3]; int n = toc_pager_layout(bx, bw, content_bot, b);
        uint32_t P = toc_pages_count();
        for (int i = 0; i < n; i++) {
            if (x >= b[i][1] && x < b[i][2]) {
                int hit = b[i][0];
                uint32_t np = s_toc_page;
                if (hit == -1)      np = (np == 0) ? 0 : np - 1;
                else if (hit == -2) np = (np + 1 >= P) ? np : np + 1;
                else if (hit >= 0 && (uint32_t)hit < P) np = (uint32_t)hit;
                if (np != s_toc_page) {
                    s_toc_page = np; s_menu_sel = 0; s_menu_scroll = 0; s_menu_pix = 0;
                }
                return true;
            }
        }
        return true;   /* 页码条空白同样消费, 不落到列表 */
    }
    if (y >= foot_y) {
        /* 底部返回: 子菜单回主菜单, 主菜单回阅读 */
        s_menu = (s_menu == BM_MENU) ? BM_READ : BM_MENU;
        s_menu_sel = 0;
        s_menu_scroll = 0;
        return true;
    }
    int vis = (content_bot - ty) / ROW_H; if (vis < 1) vis = 1;
    if (s_menu_sel < s_menu_scroll) s_menu_scroll = s_menu_sel;
    if (s_menu_sel >= s_menu_scroll + (uint32_t)vis) s_menu_scroll = s_menu_sel - vis + 1;
    int k = (y - ty + s_menu_pix) / ROW_H;
    if (k < 0 || k >= vis) return false;
    int idx = (int)s_menu_scroll + k;
    if (idx < 0 || idx >= count) return false;
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
        /* 上半页 -> 上一页 (触摸 y 与屏幕 y 同向, 上半=回上一页) */
        book_reader_prev_page();
    } else if (y > lh * 2 / 3) {
        /* 下半页 -> 下一页 */
        book_reader_next_page();
        if ((s_page % 5) == 0 && s_page != s_last_saved_page) {
            s_last_saved_page = s_page;
            book_save_progress();
        }
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
    static int s_drag_y0 = -1;        /* 按下时的逻辑 y */
    static int s_drag_sel0 = 0;       /* 按下时的选中项 */
    static int s_drag_scroll0 = 0;    /* 按下时的滚动偏移 */
    static int s_drag_px = 0;         /* 拖动累计像素 */
    static bool s_drag_scrollbar = false; /* 正在拖滚动条 */
    /* 仅阅读器菜单启用拖动 */
    if (!s_open || s_menu == BM_READ) {
        s_drag_y0 = -1;
        s_menu_dragged = false;
        return false;
    }
    int tx, ty;
    bool down = input_get_touch_pos(&tx, &ty);   /* 已旋转到逻辑坐标 */
    int bx, by, bw, bh, ty0, content_bot, foot_y;
    book_menu_geom(&bx, &by, &bw, &bh, &ty0, &content_bot, &foot_y);
    (void)foot_y;
    const int ROW_H = 32;
    int count = menu_item_count();
    if (count <= 0) { s_drag_y0 = -1; s_menu_dragged = false; return false; }
    /* 滚动条区域 (右侧 20px 宽条) */
    int sb_x = bx + bw - SCROLLBAR_W;
    int vis = (content_bot - ty0) / ROW_H; if (vis < 1) vis = 1;
    int max_scroll = (count > vis) ? (count - vis) : 0;

    if (down) {
        if (s_drag_y0 < 0) {
            /* 首次按下: 记录锚点 */
            s_drag_y0 = ty;
            s_drag_sel0 = (int)s_menu_sel;
            s_drag_scroll0 = (int)s_menu_scroll;
            s_drag_px = 0;
            s_menu_dragged = false;
            s_drag_scrollbar = (tx >= sb_x && tx <= bx + bw - 1 && ty >= ty0 && ty < content_bot);
            return false;
        }
        if (s_drag_scrollbar) {
            /* 拖滚动条: 按比例换算成选中项 */
            int track_h = content_bot - ty0;
            if (track_h > 0 && max_scroll > 0) {
                int ratio = ty - ty0;
                if (ratio < 0) ratio = 0;
                if (ratio > track_h) ratio = track_h;
                int ns = ratio * max_scroll / track_h;
                if (ns < 0) ns = 0;
                if (ns >= count) ns = count - 1;
                s_menu_scroll = (uint32_t)ns;
                s_menu_sel = (uint32_t)ns;
                s_menu_dragged = true;
                return true;
            }
            return false;
        }
        /* 像素级跟手: 内容 1:1 随手指平滑移动 */
        int ab = (s_drag_y0 - ty) < 0 ? (ty - s_drag_y0) : (s_drag_y0 - ty);
        if (ab > 1) s_menu_dragged = true;   /* 有位移=拖动, 松手一律不选章 */
        /* 绝对坐标: 顶端行像素位置 = 起始行*行高 + (按下-y当前) */
        int top_pix = s_drag_scroll0 * ROW_H + (s_drag_y0 - ty);
        int new_scroll = top_pix / ROW_H;
        int new_pix   = top_pix % ROW_H;
        if (new_pix < 0) { new_pix += ROW_H; new_scroll--; }
        if (new_scroll < 0) { new_scroll = 0; new_pix = 0; }
        if (new_scroll > max_scroll) { new_scroll = max_scroll; new_pix = 0; }
        int ns = new_scroll;                  /* 高亮跟随顶端可见行 */
        if (ns >= count) ns = count - 1;
        if (ns < 0) ns = 0;
        s_menu_scroll = (uint32_t)new_scroll;
        s_menu_pix    = new_pix;
        if ((int)s_menu_sel != ns) {
            s_menu_sel = (uint32_t)ns;
            return true;
        }
        /* 像素偏移可能变了但行列没变 → 也要重绘 */
        return true;
    }
    /* 松手 */
    s_drag_y0 = -1;
    return false;
}
