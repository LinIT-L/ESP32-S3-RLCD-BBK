#include "font_book.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "book_flow.h"    /* bf_cp_to_utf8 */
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "font_zh.h"      /* 内置 24px 兜底 */
#include "font_zh16.h"    /* 内置 16px 兜底 */
#include "gb2312_uni.h"   /* GBK 双字节 → Unicode */
#include "lbf.h"

static const char *TAG = "font_book";

/* ==========================================================================
 * 统一取字后端
 *
 *   正式运行格式: LBF（TF 卡 /sdcard/fonts 下的 .lbf，按需读取 + 有界 LRU）
 *   内置兜底:     font_zh16(16px) / font_zh(24px)，来自 appdata 分区 mmap
 *
 * 两条路径共用 font_book_glyph_unicode() 这一个取字入口，不存在第二套
 * 字体解析器（旧 .fnt 的 BK16FNT1 解析器已整体移除）。
 * ========================================================================== */

#define SD_FONT_DIR "/sdcard/fonts"

typedef enum {
    BACKEND_UI16 = 0,   /* 内置 16px 宋体 */
    BACKEND_UI24 = 1,   /* 内置 24px 宋体 */
    BACKEND_LBF  = 2,   /* TF 卡 LBF */
} backend_t;

static backend_t    s_backend = BACKEND_UI24;
/* 字体样式档。当前固件内置只有一套字库，只有 0 档可用 —— 见 font_book_select() 注释。
 * 保留这个状态是为了诊断/将来接第二套字库时不用再改调用链。 */
static uint8_t      s_font_style = 0;
static lbf_font_t  *s_lbf = NULL;
/* 2026-10-30: 当前正在使用的 LBF 绝对路径（内置字库时为空）。
 * 用途: 商店卸载前判断"这个字体是不是正在被阅读器用着"，用着就不许删 ——
 * 否则删完当前页立刻变成一排缺字框。只在 select_file/select_embedded 里改。 */
static char         s_cur_path[128];

/* ★★★ 2026-10-31: 上一个字体实例（"2 槽 PSRAM 缓存"的第二槽）。
 *   用户切 A→B→A 时，若 B 加载后把 A 释放了，切回 A 又得重读 TF 卡。
 *   这里保留最近一个，切回时**直接交换指针**，零 IO 零分配。
 *   内存：最多 2 个整块实例（实测最大 1.23MB/个），PSRAM 余量不足时自动放弃这一槽。 */
static lbf_font_t  *s_prev_lbf = NULL;
static char         s_prev_path[128];
static int          s_px = 24;        /* 目标渲染像素 */
static int          s_scale = 1;      /* 只允许 1 或 2 */
static int          s_native = 24;    /* 当前后端原生像素 */
static bool         s_inited = false;
static const char  *s_error = "未初始化";
static const char  *s_default_err = "";

/* 内置字库 2 倍放大缓冲：48px × 6 字节/行 = 288 */
static uint8_t s_scale_buf[384];
/* LBF 字形回填到统一网格用的缓冲：32px × 4 字节/行 = 128（2 倍时 64×8=512） */
static uint8_t s_cell_buf[512];
static uint8_t s_cell2_buf[1024];
/* LBF ASCII 前进宽度（半角），select_file 时按 'A' 实测；0=用 FONT8X12 */
static int s_lbf_ascii_w = 0;
/* FONT8X12 放大缓冲 */
static uint8_t s_ascii_buf[384];

/* 把 LBF 的紧凑字形放回 native_w × native_h 的统一网格。
 * 位图按行 MSB 左、1=墨点，与 UI 字库同构，渲染层无需知道字形偏移。 */
static bool lbf_fetch_cell(uint32_t cp, book_glyph_t *out) {
    lbf_glyph_t g;
    if (!lbf_glyph(s_lbf, cp, &g)) return false;

    int cw = s_native;
    int ch = s_native;
    int dbpr = (cw + 7) / 8;
    size_t need = (size_t)dbpr * ch;
    if (need > sizeof(s_cell_buf)) return false;

    memset(s_cell_buf, 0, need);
    int dx = (cw - g.w) / 2;
    int dy = lbf_baseline(s_lbf) - (g.yoff + g.h);
    int sbpr = (g.w + 7) / 8;
    for (int y = 0; y < g.h; y++) {
        int ty = y + dy;
        if (ty < 0 || ty >= ch) continue;
        for (int x = 0; x < g.w; x++) {
            if (!(g.bitmap[y * sbpr + (x >> 3)] & (0x80 >> (x & 7)))) continue;
            int tx = x + dx;
            if (tx < 0 || tx >= cw) continue;
            s_cell_buf[(size_t)ty * dbpr + (tx >> 3)] |= (uint8_t)(0x80 >> (tx & 7));
        }
    }

    if (s_scale == 2) {
        int obpr = (cw * 2 + 7) / 8;
        size_t oneed = (size_t)obpr * (ch * 2);
        if (oneed > sizeof(s_cell2_buf)) return false;
        memset(s_cell2_buf, 0, oneed);
        for (int y = 0; y < ch; y++) {
            for (int x = 0; x < cw; x++) {
                if (!(s_cell_buf[(size_t)y * dbpr + (x >> 3)] & (0x80 >> (x & 7)))) continue;
                int tx = x * 2, ty = y * 2;
                for (int d = 0; d < 2; d++) {
                    uint8_t *row = s_cell2_buf + (size_t)(ty + d) * obpr;
                    row[tx >> 3] |= (uint8_t)(0x80 >> (tx & 7));
                    row[(tx + 1) >> 3] |= (uint8_t)(0x80 >> ((tx + 1) & 7));
                }
            }
        }
        out->w = (uint8_t)(cw * 2);
        out->h = (uint8_t)(ch * 2);
        out->bitmap = s_cell2_buf;
    } else {
        out->w = (uint8_t)cw;
        out->h = (uint8_t)ch;
        out->bitmap = s_cell_buf;
    }
    return true;
}

/* -------------------------------------------------------------------------- */

static int snap_px(int req, int native) {
    /* 只允许 原生 或 严格 2 倍；其它请求就近收敛 */
    int a = native;
    int b = native * 2;
    if (b > 64) b = 64;               /* 超过 64px 没有实际意义 */
    if (req <= a) return a;
    if (req >= b) return b;
    return ((req - a) < (b - req)) ? a : b;
}

bool font_book_init(void) {
    /* ★ 幂等，且**绝不改动**已选择的字体。
     * 旧实现在这里把 s_using_ui 强制置真，导致 page_book 预选的 TF 字库
     * 在 book_reader_open() 里被静默丢弃（随后 font_book_select 又释放 SD 缓冲）。 */
    if (s_inited) return true;
    s_inited = true;
    if (s_backend != BACKEND_LBF) {
        s_backend = BACKEND_UI24;
        s_native = 24;
        s_px = 24;
        s_scale = 1;
    }
    s_error = NULL;
    return true;
}

/* ---- 内置字库 ---- */

static bool ui_lookup(uint32_t cp, book_glyph_t *out) {
    /* 字库缺少数个弯引号/间隔号时的等价兜底（形状相同），只在这些码点确实缺失时生效 */
    uint32_t map = 0;
    if      (cp == 0x201C || cp == 0x201E) map = 0x300C;
    else if (cp == 0x201D)                 map = 0x300D;
    else if (cp == 0x2018)                 map = 0x3008;
    else if (cp == 0x2019)                 map = 0x3009;
    else if (cp == 0x00B7)                 map = 0x30FB;

    int src_sz = (s_backend == BACKEND_UI16) ? 16 : 24;
    uint8_t utf8[4];
    const uint8_t *bmp = NULL;

    int n = bf_cp_to_utf8(cp, utf8);
    if (n > 0) {
        if (src_sz == 16) {
            int idx = font_zh16_find_utf8((const char *)utf8);
            if (idx >= 0) bmp = font_zh16_get_bitmap_by_index(idx);
        } else {
            int idx = font_zh_find_utf8((const char *)utf8);
            if (idx >= 0) bmp = font_zh_get_bitmap_by_index(idx);
        }
    }
    if (!bmp && map) {
        n = bf_cp_to_utf8(map, utf8);
        if (n > 0) {
            if (src_sz == 16) {
                int idx = font_zh16_find_utf8((const char *)utf8);
                if (idx >= 0) bmp = font_zh16_get_bitmap_by_index(idx);
            } else {
                int idx = font_zh_find_utf8((const char *)utf8);
                if (idx >= 0) bmp = font_zh_get_bitmap_by_index(idx);
            }
        }
    }
    if (!bmp) return false;

    int out_sz = s_px;
    if (out_sz == src_sz) {
        out->w = out->h = (uint8_t)src_sz;
        out->bitmap = bmp;
        return true;
    }
    /* 严格 2 倍最近邻（s_scale 只会是 2；非整数倍已由 snap_px 收敛掉） */
    int dbpr = (out_sz + 7) / 8;
    if ((size_t)dbpr * out_sz > sizeof(s_scale_buf)) return false;
    memset(s_scale_buf, 0, (size_t)dbpr * out_sz);
    int sbpr = src_sz / 8;
    for (int y = 0; y < src_sz; y++) {
        for (int x = 0; x < src_sz; x++) {
            if (!(bmp[y * sbpr + (x >> 3)] & (0x80 >> (x & 7)))) continue;
            int tx = x * 2, ty = y * 2;
            for (int dy = 0; dy < 2; dy++) {
                uint8_t *row = s_scale_buf + (size_t)(ty + dy) * dbpr;
                row[tx >> 3]     |= (uint8_t)(0x80 >> (tx & 7));
                row[(tx + 1) >> 3] |= (uint8_t)(0x80 >> ((tx + 1) & 7));
            }
        }
    }
    out->w = out->h = (uint8_t)out_sz;
    out->bitmap = s_scale_buf;
    return true;
}

/* ---- 对外接口 ---- */

void font_book_select(int style, int size_px) {
    /* ★★★ 2026-10-30 核实结论（原实现这里是 `(void)style;`，看着像 bug）★★★
     *   style 的原始语义是 0=仿宋 / 1=黑体，但**固件里从来没有两套内置字库**：
     *   内置只有 appdata 固定槽那两档 —— 16px 文泉驿点阵宋、24px HZK24S，
     *   是按**字号**分的，不是按**风格**分的。所以 style 没有任何可选目标，
     *   硬把它接上去只会造成"菜单显示黑体、实际还是宋体"的假生效。
     *
     *   真正的字体选择走 font_book_select_file()（TF 卡 LBF），与 style 无关；
     *   那个入口已经做到"选了就真的换字"。本函数只负责按字号选内置档位。
     *
     *   这里做两件事：① 把越界的 style 规范回 0；② 保留形参不删，避免改动
     *   既有调用点与 ABI。要真的支持"仿宋/黑体"切换，前提是先加第二套内置字库
     *   （appdata 里还有 ALT 槽可用），属于字体资源变动，不在本次范围。 */
    if (style < 0) style = 0;
    if (style > 1) style = 0;
    s_font_style = (uint8_t)style;   /* 仅记录，供诊断输出 */

    /* TF LBF 已激活时，本函数只调整倍率，**不释放、不丢弃** LBF */
    if (s_backend == BACKEND_LBF && s_lbf) {
        s_px = snap_px(size_px, s_native);
        s_scale = (s_px >= s_native * 2) ? 2 : 1;
        return;
    }

    /* 内置模式：在 16 / 24 / 32 三档里就近选（32 是 16 的严格 2 倍） */
    /* 内置可选档：16(原生) / 24(原生) / 32(16px×2) / 48(24px×2)。
     * 都是"原生尺寸或严格 2 倍"，不做非整数倍缩放。 */
    struct { int px; backend_t b; } cand[4] = {
        {16, BACKEND_UI16}, {24, BACKEND_UI24},
        {32, BACKEND_UI16}, {48, BACKEND_UI24},
    };
    int req = size_px;
    if (req < 8) req = 8;
    if (req > 48) req = 48;   /* 48 是最大档 */

    int best = 0, bestd = 0x7FFFFFFF;
    for (int i = 0; i < 4; i++) {
        int d = req > cand[i].px ? req - cand[i].px : cand[i].px - req;
        if (d < bestd) { bestd = d; best = i; }
    }
    s_backend = cand[best].b;
    s_native = (cand[best].b == BACKEND_UI16) ? 16 : 24;
    s_px = cand[best].px;
    s_scale = (s_px > s_native) ? 2 : 1;
    s_error = NULL;
}

void font_book_select_embedded(int style, int size_px) {
    if (s_lbf) {
        /* ★ 2026-10-31: 退到 prev 槽而不是就地释放 —— 用户常常"内置↔TF"来回切，
         *   释放了就得重读整卡。内存不够时才真释放（下面 prev 满时自然顶掉）。 */
        if (s_prev_lbf) lbf_close(s_prev_lbf);
        s_prev_lbf = s_lbf;
        snprintf(s_prev_path, sizeof(s_prev_path), "%s", s_cur_path);
        s_lbf = NULL;
        ESP_LOGI(TAG, "TF 字库转入缓存槽, 切回内置字体");
    }
    s_cur_path[0] = 0;                       /* 回内置字库 = 没有"正在使用的文件" */
    s_backend = BACKEND_UI24;
    s_native = 24;
    s_lbf_ascii_w = 0;
    font_book_select(style, size_px);
}

/* 2026-10-31: 退出阅读器时释放**全部**字体实例（当前槽 + 缓存槽）。
 * 阅读器关书必须调用：否则两个 1MB+ 的映像会一直压在 PSRAM 里，
 * 挤占游戏引擎/其他模块。再次进入时重新预加载即可（30 万~70 万字节，<0.5s）。 */
void font_book_release_all(void) {
    if (s_lbf)      { lbf_close(s_lbf);      s_lbf = NULL; }
    if (s_prev_lbf) { lbf_close(s_prev_lbf); s_prev_lbf = NULL; }
    s_prev_path[0] = 0;
    s_cur_path[0]  = 0;
    ESP_LOGI(TAG, "字体实例已全部释放 (当前槽 + 缓存槽)");
}

const char *font_book_error(void) {
    return s_error ? s_error : s_default_err;
}

const char *font_book_current_name(void) {
    if (s_backend == BACKEND_LBF && s_lbf) return lbf_name(s_lbf);
    return (s_native == 16) ? "内置宋体16" : "内置宋体24";
}

int font_book_native_px(void) { return s_native; }
int font_book_scale(void)     { return s_scale; }

int font_book_size_options(int *out, int max) {
    if (!out || max <= 0) return 0;
    int n = 0;
    out[n++] = s_native;
    if (n < max) out[n++] = s_native * 2;
    return n;
}

int font_book_cell_w(void)  { return s_px; }
int font_book_cell_h(void)  { return s_px; }
int font_book_ascii_w(void) {
    if (s_backend == BACKEND_LBF && s_lbf && s_lbf_ascii_w > 0)
        return s_lbf_ascii_w * s_scale;
    return (s_px * 8 + 8) / 16;
}
int font_book_ascii_h(void) { return s_px; }

bool font_book_glyph_ascii(uint8_t c, book_glyph_t *out) {
    if (c < 0x20 || c > 0x7E) return false;

    /* LBF 自带 ASCII 时优先用，中英文风格才统一 */
    if (s_backend == BACKEND_LBF && s_lbf) {
        lbf_glyph_t g;
        if (lbf_glyph_scaled(s_lbf, c, s_scale, &g)) {
            out->w = g.w;
            out->h = g.h;
            out->bitmap = g.bitmap;
            return true;
        }
    }

    /* ASCII 是半角：宽度为同档中文的一半，画布高度与中文一致（= 字号 px）。
     * 四档都用 8×16 源做**整数倍最近邻**放大，不插值、不产生灰阶：
     *   16 → 8×16(1×) ｜ 24 → 12×24(1.5×) ｜ 32 → 16×32(2×) ｜ 48 → 24×48(3×)
     *
     * ★★★ 2026-10-30 基线对齐（本轮核心修复）★★★
     *   实测 VGA 8×16 的基线在**第 11 行**（大写字母底），小写 x-height 在第 5~11 行，
     *   降部 g/p/q/y 到第 14 行。而中文点阵字库的基线在 14 / 20 / 28 / 40 行
     *   （16px 档对齐第 14 行；24px 档 HZK24S 头里 baseline=20；32/48 是它们的 2 倍）。
     *
     *   原实现把 8×16 整块等比放大 ⇒ 英文大写底部比中文基线**高 3~7 行**：
     *   16px 时差 3 行还看得过去，48px 时差 7 行 —— 中文画满 48 行、英文只占 33 行且贴顶，
     *   看起来完全是两套字号，这就是"48px 中英文尺寸不一致"的真正来源（不是放大倍数错）。
     *
     *   修法：每档把 ASCII 整体下移 oy 行，使大写底部落在中文基线上方 2 行内（视觉齐脚）。
     *   oy 是按"降部底 ≤ 画布底"反算出来的，保证 g/p/q/y 的尾巴不被裁：
     *     16: 降部 14 + 1 = 15 ≤ 15 ✅   24: 21 + 2 = 23 ≤ 23 ✅
     *     32: 28 + 3 = 31 ≤ 31 ✅       48: 42 + 5 = 47 ≤ 47 ✅
     *   字宽/字形比例完全不动（不是拉伸），只做竖直位移。 */
    int px = s_px;
    static const struct { int px, ow, oh, oy; } AT[] = {
        {16,  8, 16, 1},
        {24, 12, 24, 2},
        {32, 16, 32, 3},
        {48, 24, 48, 5},
    };
    int ow = 8, oh = 16, oy = 1;
    for (size_t i = 0; i < sizeof(AT) / sizeof(AT[0]); i++)
        if (AT[i].px == px) { ow = AT[i].ow; oh = AT[i].oh; oy = AT[i].oy; break; }
    {
        int dbpr = (ow + 7) / 8;
        if ((size_t)dbpr * oh > sizeof(s_ascii_buf)) return false;
        memset(s_ascii_buf, 0, (size_t)dbpr * oh);
        for (int y = 0; y < 16; y++) {
            int ty = oy + y * oh / 16;       /* 源行 → 目标行（含基线偏移） */
            if (ty >= oh) break;             /* 超出画布的行丢弃（都是空白/降部末梢） */
            for (int x = 0; x < 8; x++) {
                if (!(FONT8X12[c - 0x20][y] & (0x80 >> x))) continue;
                for (int tx = x * ow / 8; tx < (x + 1) * ow / 8 && tx < ow; tx++)
                    s_ascii_buf[(size_t)ty * dbpr + (tx >> 3)] |= (uint8_t)(0x80 >> (tx & 7));
            }
        }
        out->w = (uint8_t)ow;
        out->h = (uint8_t)oh;
        out->bitmap = s_ascii_buf;
        return true;
    }
}

bool font_book_glyph_gb(uint8_t hi, uint8_t lo, book_glyph_t *out) {
    if (hi < 0xA1 || hi > 0xFE || lo < 0xA1 || lo > 0xFE) return false;
    uint32_t cp = gb2312_to_uni[hi - 0xA1][lo - 0xA1];
    if (cp == 0) return false;
    return font_book_glyph_unicode(cp, out);
}

bool font_book_glyph_unicode(uint32_t cp, book_glyph_t *out) {
    if (s_backend == BACKEND_LBF && s_lbf) {
        /* 缺字不回落到内置字库整套替换：由调用方画缺字框（严格模式），
         * 用户能一眼看出这本字体缺哪个字，而不是被骗成"都有"。 */
        return lbf_fetch_cell(cp, out);
    }
    return ui_lookup(cp, out);
}

/* ---- TF 卡 LBF 加载 ---- */

bool font_book_select_file(const char *path) {
    if (!path || !path[0]) {
        s_error = "路径为空";
        return false;
    }

    /* ① 已经是当前字体 → 直接成功，不做任何 IO */
    if (s_lbf && s_cur_path[0] && strcmp(s_cur_path, path) == 0) return true;

    char err[64] = {0};
    lbf_font_t *nf = NULL;
    bool from_prev = false;

    /* ② 目标就在"上一个"槽里 → 交换引用，不读卡、不分配 */
    if (s_prev_lbf && strcmp(s_prev_path, path) == 0) {
        nf = s_prev_lbf;
        s_prev_lbf = NULL;
        s_prev_path[0] = 0;
        from_prev = true;
    } else {
        /* 内存紧就先丢掉上一槽，保证当前字体能整块进 PSRAM（预加载才是快的根本） */
        if (s_prev_lbf && heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < (3u << 20)) {
            lbf_close(s_prev_lbf);
            s_prev_lbf = NULL;
            s_prev_path[0] = 0;
        }
        nf = lbf_open(path, err, sizeof(err));
        if (!nf) {
            /* 明确失败：保留当前字体不变，绝不静默切换 */
            ESP_LOGW(TAG, "LBF 加载失败 %s: %s", path, err);
            s_error = err[0] ? err : "LBF 加载失败";
            return false;
        }
    }

    /* ③ 当前字体退到 prev 槽（顶掉更老的那个） */
    if (s_prev_lbf) lbf_close(s_prev_lbf);
    s_prev_lbf = s_lbf;
    if (s_lbf) snprintf(s_prev_path, sizeof(s_prev_path), "%s", s_cur_path);
    else       s_prev_path[0] = 0;

    s_lbf = nf;
    s_backend = BACKEND_LBF;
    s_native = lbf_native_w(nf);
    s_px = snap_px(s_px, s_native);
    s_scale = (s_px >= s_native * 2) ? 2 : 1;
    s_error = NULL;

    /* 实测半角前进宽度：直接取 'A' 的字形宽度，缺 ASCII 则回落到 FONT8X12 规则 */
    s_lbf_ascii_w = 0;
    {
        lbf_glyph_t g;
        if (lbf_glyph(nf, 'A', &g)) s_lbf_ascii_w = g.w;
    }

    snprintf(s_cur_path, sizeof(s_cur_path), "%s", path);   /* 记住正在用哪个文件 */
    ESP_LOGI(TAG, "切换 LBF: %s (%dpx 原生, 显示 %dpx, %s, %s, %s)",
             lbf_name(nf), s_native, s_px, s_scale == 2 ? "2x" : "1x",
             from_prev ? "命中缓存" : "新加载",
             lbf_is_preloaded(nf) ? "整块PSRAM" : "降级按需读");
    return true;
}

int font_book_scan_sd(char names[][64], int max) {
    if (!names || max <= 0) return 0;

    DIR *dir = opendir(SD_FONT_DIR);
    if (!dir) {
        /* ★ 2026-10-30 linit 要求: 进阅读器就把字体目录准备好。
         *   原来这目录**只在商店装字体包时**由 am_asset_install_cb 里的 mkdirs 顺手建出来 ——
         *   没通过商店装过字体的机器上它根本不存在，用户想自己拷个 .lbf 进去都不知道放哪，
         *   而且"商店下载失败"时目录也建不出来（正是这次故障的现场）。
         *   现在扫描前先 mkdir：卡在就建，卡不在 mkdir 失败也无所谓（下次再试）。
         *   幂等 —— 已存在时 mkdir 返回 EEXIST，忽略即可。 */
        mkdir(SD_FONT_DIR, 0777);
        dir = opendir(SD_FONT_DIR);
    }
    if (!dir) {
        ESP_LOGI(TAG, "字体目录不存在且创建失败: %s (TF 卡未挂载?)", SD_FONT_DIR);
        return 0;
    }

    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && count < max) {
        const char *dot = strrchr(ent->d_name, '.');
        if (dot && strcasecmp(dot, ".lbf") == 0) {
            snprintf(names[count], 64, "%.63s", ent->d_name);
            count++;
        }
    }
    closedir(dir);
    ESP_LOGI(TAG, "扫描到 %d 个 LBF 字体", count);
    return count;
}

bool font_book_is_sd_active(void) {
    return s_backend == BACKEND_LBF && s_lbf != NULL;
}

/* 当前正在使用的 LBF 路径；内置字库时返回空串。见 s_cur_path 说明。 */
const char *font_book_current_file(void) { return s_cur_path; }

/* ===== 2026-10-31: 字体显示名中文化 (唯一实现) ============================
 * 为什么要一个公用函数: 字体列表有**两个入口** —— 阅读器菜单(BM_FONT)和
 *   书架「阅读优化 → 字体」滚轮。原来各写一套名字, 书架那边直接显示
 *   "fusion8.lbf" 这种文件名, 与"名称统一为 6 字以内中文"的要求不符。
 *
 * 规则: 中文原名前缀 与 文件名主干前缀 都认; 数字后缀(字号)保留, 因为
 *   同一字体不同尺寸必须能区分; 括号/变体后缀一律去掉。
 * 只影响**显示**, 不改 LBF 内部名字、不改持久化用的路径。
 */
const char *font_book_short_name(const char *raw, char *buf, size_t cap)
{
    static const struct { const char *from, *to; } MAP[] = {
        /* 中文原名 (LBF 头里写的) */
        { "缝合像素",      "融合像素" },   /* Fusion Pixel */
        { "融合像素",      "融合像素" },
        { "GNU Unifont",   "通用点阵" },
        { "Unifont",       "通用点阵" },
        { "文泉驿点阵宋",  "文泉驿"   },   /* WenQuanYi */
        { "文泉驿",        "文泉驿"   },
        { "寒蝉点阵体",    "寒蝉点阵" },   /* ChillBitmap */
        { "寒蝉",          "寒蝉点阵" },
        /* 文件名主干 (书架滚轮列的是文件名) */
        { "fusion",        "融合像素" },
        { "ark",           "方舟像素" },
        { "wqy",           "文泉驿"   },
        { "unifont",       "通用点阵" },
        { "chill",         "寒蝉点阵" },
        { "qiuye",         "秋叶圆体" },
        { "zhengge",       "正格点黑" },
        { "changban",      "长坂点宋" },
        /* 方舟像素 / 秋叶圆体 / 正格点黑 / 长坂点宋 的中文原名本身已够短, 原样保留 */
    };
    if (!buf || cap == 0) return raw;
    if (!raw || !raw[0]) { snprintf(buf, cap, "未知字体"); return buf; }

    /* 文件名主干: 去目录、去扩展名 */
    const char *base = strrchr(raw, '/');
    base = base ? base + 1 : raw;
    char stem[64];
    size_t k = 0;
    while (base[k] && base[k] != '.' && k + 1 < sizeof(stem)) { stem[k] = base[k]; k++; }
    stem[k] = 0;
    if (!stem[0]) { snprintf(buf, cap, "未知字体"); return buf; }

    for (size_t i = 0; i < sizeof(MAP) / sizeof(MAP[0]); i++) {
        size_t n = strlen(MAP[i].from);
        if (strncmp(stem, MAP[i].from, n) == 0) {
            snprintf(buf, cap, "%s%s", MAP[i].to, stem + n);
            goto strip;
        }
        if (strncmp(raw, MAP[i].from, n) == 0) {
            snprintf(buf, cap, "%s%s", MAP[i].to, raw + n);
            goto strip;
        }
    }
    snprintf(buf, cap, "%s", stem);       /* 认不出就退回主干, 不编造名字 */

strip:
    /* 括号后缀: "方舟像素12(OTF)" → "方舟像素12"
     * ⚠ 全角"（"是 3 字节 UTF-8, 不能当 char 常量比 */
    {
        char *h = strchr(buf, '(');
        char *f = strstr(buf, "\xef\xbc\x88");
        char *cut = (h && f) ? (h < f ? h : f) : (h ? h : f);
        if (cut) *cut = 0;
    }
    /* 文件变体后缀 "_otf" / "-otf" (大小写都认) */
    {
        size_t n = strlen(buf);
        if (n > 4) {
            const char *t = buf + n - 4;
            if ((t[0] == '_' || t[0] == '-') &&
                (t[1] == 'o' || t[1] == 'O') && (t[2] == 't' || t[2] == 'T') &&
                (t[3] == 'f' || t[3] == 'F')) buf[n - 4] = 0;
        }
    }
    while (cap > 1 && buf[0] && buf[strlen(buf) - 1] == ' ') buf[strlen(buf) - 1] = 0;
    return buf;
}
