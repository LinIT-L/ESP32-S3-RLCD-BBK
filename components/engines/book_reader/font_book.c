#include "font_book.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "font_part.h"
#include "book_flow.h"   /* bf_cp_to_utf8: cp → UTF-8 */
#include "font_zh.h"     /* font_zh_find_utf8/get_bitmap_by_index, FONT8X12 */
#include "font_zh16.h"   /* font_zh16_find_utf8/get_bitmap_by_index */
#include "gb2312_uni.h"  /* gb2312_to_uni: GBK 双字节 → Unicode */

static const char *TAG = "font_book";

/* V1.0.9x: 电子书正文改用固件 UI 字体 (font_zh16=16px宋体 / font_zh=24px粗宋),
 * 不再依赖 appdata 里的 .fnt 槽位. .fnt 解析器原样保留, 仅供可选 TF 卡 /sdcard/fonts/ 覆盖.
 */

#define FONT_MAGIC "BK16FNT1"
#define GB_GRID    94          /* GB2312 码区 94x94 */

typedef struct {
    char     magic[8];
    uint16_t cell_w;
    uint16_t cell_h;
    uint16_t ascii_w;
    uint16_t ascii_h;
    uint16_t ascii_count;
    uint16_t gb_count;
    uint32_t gb_bytes_per_char;
    uint32_t ascii_bytes_per_char;
    uint32_t flags;
} book_font_header_t;

static const book_font_header_t *s_hdr;
static const uint8_t *s_ascii;
static const uint8_t *s_gb_off;     /* u16 小端数组 (94*94) */
static const uint8_t *s_gb_glyphs;
static const uint8_t *s_glyph_off;  /* 压缩格式: u32[gb_count+1] 字形偏移 */
static const uint8_t *s_uni;        /* u16 cp, u16 idx 交替, 按 cp 升序 */
static const char *s_error = "未初始化";
static bool s_compressed = false;        /* 字形是否 RLE 压缩 (flags bit0) */
static uint8_t s_glyph_scratch[160];     /* 压缩字形解码缓冲 (32px 最大 128B) */

/* V1.0.88: TF 卡字库支持 */
static bool s_sd_active = false;         /* 当前是否使用 TF 卡字库 */
static uint8_t *s_sd_buf = NULL;         /* TF 卡字库 PSRAM 缓冲 */
static size_t s_sd_buf_size = 0;

/* V1.0.9x: UI 字体模式(默认开启). true=用 font_zh/font_zh16; false=用 .fnt(SD卡覆盖). */
static bool  s_using_ui = true;
static bool  s_ui16     = false;         /* true=基准字库 font_zh16(16px); false=font_zh(24px) */
static uint8_t s_ui_px  = 24;            /* 目标渲染字号(px): 16/20/24/28/32, UI 兜底按此缩放 */
static uint8_t s_ui_cell_w = 24, s_ui_cell_h = 24;
static uint8_t s_ui_ascii_w = 8, s_ui_ascii_h = 16;   /* UI 模式 ASCII 恒 8x16 (FONT8X12) */
/* 24px 源字形缩放到目标字号的行缓冲 (最大 32px: 32×4=128 字节) */
static uint8_t s_ui_scale_buf[128];

#define FONT_FLAG_RLE 1

static uint16_t rd_u16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* RLE 解码: 0..127 = 1..128 个 0 字节; 128..255 = (v-127) 个字面字节 */
static int rle_decode(const uint8_t *src, size_t n, uint8_t *dst, size_t cap) {
    size_t si = 0, di = 0;
    while (si < n && di < cap) {
        uint8_t v = src[si++];
        if (v < 128) {
            int c = (int)v + 1;
            if ((size_t)c > cap - di) c = (int)(cap - di);
            memset(dst + di, 0, (size_t)c);
            di += (size_t)c;
        } else {
            int c = (int)v - 127;
            if (si + (size_t)c > n) c = (int)(n - si);
            if ((size_t)c > cap - di) c = (int)(cap - di);
            memcpy(dst + di, src + si, (size_t)c);
            si += (size_t)c;
            di += (size_t)c;
        }
    }
    return (int)di;
}

static bool font_bind(const uint8_t *d, size_t len) {
    if (!d) {
        s_error = "字库分区未映射";
        return false;
    }
    if (len < sizeof(book_font_header_t)) {
        s_error = "字库文件过小";
        return false;
    }
    if (memcmp(d, FONT_MAGIC, 8) != 0) {
        s_error = "字库魔数错误";
        return false;
    }
    s_hdr = (const book_font_header_t *)d;
    s_compressed = (s_hdr->flags & FONT_FLAG_RLE) != 0;
    size_t off = sizeof(book_font_header_t);
    s_ascii = d + off;
    off += (size_t)s_hdr->ascii_count * s_hdr->ascii_bytes_per_char;
    s_gb_off = d + off;
    off += GB_GRID * GB_GRID * 2;
    if (s_compressed) {
        s_glyph_off = d + off;
        off += ((size_t)s_hdr->gb_count + 1) * 4;
        s_gb_glyphs = d + off;
        /* 字形 blob 之后是 Unicode 表 (gb_count * 4) */
        off = len - (size_t)s_hdr->gb_count * 4;
    } else {
        s_gb_glyphs = d + off;
        off += (size_t)s_hdr->gb_count * s_hdr->gb_bytes_per_char;
    }
    s_uni = d + off;

    if (off > len) {
        s_error = "字库数据不完整";
        return false;
    }
    s_error = NULL;
    return true;
}

/* ---- V1.0.9x: UI 字体模式 —— 用 font_zh(24px)/font_zh16(16px) 取字形 ---- */
/* 把 1bpp 位图 src(w×h) 线性缩放到 dst(ow×oh), 存入 s_ui_scale_buf.
 * srcBitsPerRow = 每行字节数; dstBitsPerRow = (ow+7)/8.
 * 返回 dst 指针 (即 s_ui_scale_buf). */
static uint8_t *font_book_ui_scale(const uint8_t *src, int w, int h,
                                   int ow, int oh,
                                   int sbpr, int dbpr) {
    for (int dy = 0; dy < oh; dy++) {
        int sy = dy * h / oh;
        for (int dx = 0; dx < ow; dx++) {
            int sx = dx * w / ow;
            bool on = src[sy * sbpr + (sx >> 3)] & (0x80 >> (sx & 7));
            int by = dy * dbpr + (dx >> 3);
            if (on) s_ui_scale_buf[by] |= (uint8_t)(0x80 >> (dx & 7));
            else    s_ui_scale_buf[by] &= (uint8_t)~(0x80 >> (dx & 7));  /* 先清再置, 复用缓冲 */
        }
    }
    return s_ui_scale_buf;
}

/* 把 Unicode 码点换算成 UTF-8 并用当前档位在 UI 字体中查字形 */
static bool font_book_ui_lookup(uint32_t cp, book_glyph_t *out) {
    /* V1.0.9x: 字库新增了 “”‘’・; ·U+00B7 是2字节无法入3字节槽故缺.
     * 先查原始码点, 查不到(字库仍缺)再用映射兜底: ·→・ (形状相同), “”→「」等.
     * 仅当对应字形在字库中确实缺失时映射, 避免覆盖已补符号. */
    uint32_t map = 0;
    if      (cp == 0x201C || cp == 0x201E) map = 0x300C;
    else if (cp == 0x201D)                 map = 0x300D;
    else if (cp == 0x2018)                 map = 0x3008;
    else if (cp == 0x2019)                 map = 0x3009;
    else if (cp == 0x00B7)                 map = 0x30FB;

    uint8_t utf8[4];
    int n = bf_cp_to_utf8(cp, utf8);
    if (n <= 0) return false;
    bool have = false;
    const uint8_t *b = NULL;
    if (s_ui16) {
        int idx = font_zh16_find_utf8((const char *)utf8);
        if (idx >= 0) { b = font_zh16_get_bitmap_by_index(idx); out->w = out->h = 16; }
    } else {
        int idx = font_zh_find_utf8((const char *)utf8);
        if (idx >= 0) { b = font_zh_get_bitmap_by_index(idx); out->w = out->h = 24; }
    }
    /* 原始码点缺字形 → 用映射码点再查一次 (兜底) */
    if (!b && map) {
        n = bf_cp_to_utf8(map, utf8);
        if (n > 0) {
            if (s_ui16) {
                int idx = font_zh16_find_utf8((const char *)utf8);
                if (idx >= 0) { b = font_zh16_get_bitmap_by_index(idx); out->w = out->h = 16; }
            } else {
                int idx = font_zh_find_utf8((const char *)utf8);
                if (idx >= 0) { b = font_zh_get_bitmap_by_index(idx); out->w = out->h = 24; }
            }
        }
    }
    if (b) {
        /* 需要缩放: 基准字号(font_zh16=16 / font_zh=24) != 目标 s_ui_px */
        int src_sz = s_ui16 ? 16 : 24;
        int out_sz = s_ui_px;
        if (out_sz != src_sz) {
            int dbpr = (out_sz + 7) / 8;
            memset(s_ui_scale_buf, 0, (size_t)out_sz * dbpr);
            out->bitmap = font_book_ui_scale(b, src_sz, src_sz,
                                             out_sz, out_sz,
                                             src_sz / 8, dbpr);
            out->w = out->h = (uint8_t)out_sz;
        } else {
            out->bitmap = b;
            out->w = out->h = (uint8_t)src_sz;
        }
        have = true;
    }
    return have;
}

bool font_book_init(void) {
    /* 不再映射 appdata 的 .fnt 槽: 默认切到 UI 字体模式 */
    s_using_ui = true;
    s_ui16 = false;
    s_ui_cell_w = s_ui_cell_h = 24;
    return true;
}

/* 样式: 0=仿宋 1=黑体; 字号: 目标像素(16~32)
 * [V1.0.9x: 全部改复用固件 UI 字体, 不再区分仿宋/黑体.
 *  字号 16 → font_zh16(16px); 20/24/28/32 → font_zh(24px 粗宋) 并按需缩放.
 *  TF 卡 .fnt 激活时(s_sd_active)此函数切回 UI 模式; 卡上字体优先见书内字符菜单. */
void font_book_select(int style, int size_px) {
    (void)style;                       /* 仿宋/黑体不再区分, 统一 UI 字体 */
    /* 释放旧 SD 缓冲 (切回 UI 模式) */
    if (s_sd_buf) {
        heap_caps_free(s_sd_buf);
        s_sd_buf = NULL;
        s_sd_buf_size = 0;
    }
    s_sd_active = false;
    s_using_ui = true;
    /* 字号收敛到 16~32 */
    int px = size_px;
    if (px < 16) px = 16;
    if (px > 32) px = 32;
    s_ui_px = (uint8_t)px;
    s_ui16 = (px == 16);               /* 16 直接用 font_zh16, 其余用 font_zh 缩放 */
    s_ui_cell_w = s_ui_cell_h = (uint8_t)px;
    s_ui_ascii_w = (uint8_t)(px * 8 / 16); if (s_ui_ascii_w < 4) s_ui_ascii_w = 4;
    s_ui_ascii_h = (uint8_t)px;
}

/* V1.0.88: 切回内嵌字库 (退出 SD 字库模式) */
void font_book_select_embedded(int style, int size_id) {
    font_book_select(style, size_id);
}

const char *font_book_error(void) {
    return s_error ? s_error : "";
}

int font_book_cell_w(void)  { return s_using_ui ? s_ui_cell_w  : (s_hdr ? s_hdr->cell_w : 16); }
int font_book_cell_h(void)  { return s_using_ui ? s_ui_cell_h  : (s_hdr ? s_hdr->cell_h : 16); }
int font_book_ascii_w(void) { return s_using_ui ? s_ui_ascii_w : (s_hdr ? s_hdr->ascii_w : 8); }
int font_book_ascii_h(void) { return s_using_ui ? s_ui_ascii_h : (s_hdr ? s_hdr->ascii_h : 16); }

bool font_book_glyph_ascii(uint8_t c, book_glyph_t *out) {
    if (s_using_ui) {
        if (c < 0x20 || c > 0x7E) return false;
        /* ASCII 8×16 源; 字号非 16px 时按目标高/宽缩放 (半宽: 宽=px*8/16) */
        int px = s_ui_px;
        if (px == 16) {
            out->w = 8; out->h = 16;
            out->bitmap = FONT8X12[c - 0x20];
        } else {
            int ow = px * 8 / 16; if (ow < 4) ow = 4;
            int dbpr = (ow + 7) / 8;
            memset(s_ui_scale_buf, 0, (size_t)px * dbpr);
            out->w = (uint8_t)ow; out->h = (uint8_t)px;
            out->bitmap = font_book_ui_scale(FONT8X12[c - 0x20], 8, 16, ow, px, 1, dbpr);
        }
        return true;
    }
    if (!s_hdr || c < 0x20 || c >= 0x20 + s_hdr->ascii_count) return false;
    out->w = s_hdr->ascii_w;
    out->h = s_hdr->ascii_h;
    out->bitmap = s_ascii + (size_t)(c - 0x20) * s_hdr->ascii_bytes_per_char;
    return true;
}

bool font_book_glyph_gb(uint8_t hi, uint8_t lo, book_glyph_t *out) {
    if (s_using_ui) {
        if (hi < 0xA1 || hi > 0xFE || lo < 0xA1 || lo > 0xFE) return false;
        uint32_t cp = gb2312_to_uni[hi - 0xA1][lo - 0xA1];
        if (cp == 0) return false;
        return font_book_ui_lookup(cp, out);
    }
    if (!s_hdr || hi < 0xA1 || hi > 0xF7 || lo < 0xA1 || lo > 0xFE) return false;
    uint16_t idx = rd_u16(s_gb_off + ((size_t)(hi - 0xA1) * GB_GRID + (lo - 0xA1)) * 2);
    if (idx == 0xFFFF) return false;
    out->w = s_hdr->cell_w;
    out->h = s_hdr->cell_h;
    if (s_compressed) {
        uint32_t a = rd_u32(s_glyph_off + (size_t)idx * 4);
        uint32_t b = rd_u32(s_glyph_off + ((size_t)idx + 1) * 4);
        if (rle_decode(s_gb_glyphs + a, (size_t)(b - a), s_glyph_scratch,
                       sizeof(s_glyph_scratch)) <= 0) return false;
        out->bitmap = s_glyph_scratch;
    } else {
        out->bitmap = s_gb_glyphs + (size_t)idx * s_hdr->gb_bytes_per_char;
    }
    return true;
}

bool font_book_glyph_unicode(uint32_t cp, book_glyph_t *out) {
    if (s_using_ui) return font_book_ui_lookup(cp, out);
    if (!s_hdr || s_hdr->gb_count == 0) return false;
    /* 二分查找升序 cp 表 */
    int lo = 0, hi = (int)s_hdr->gb_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint16_t c = rd_u16(s_uni + (size_t)mid * 4);
        if (c == cp) {
            uint16_t idx = rd_u16(s_uni + (size_t)mid * 4 + 2);
            out->w = s_hdr->cell_w;
            out->h = s_hdr->cell_h;
            if (s_compressed) {
                uint32_t a = rd_u32(s_glyph_off + (size_t)idx * 4);
                uint32_t b = rd_u32(s_glyph_off + ((size_t)idx + 1) * 4);
                if (rle_decode(s_gb_glyphs + a, (size_t)(b - a), s_glyph_scratch,
                               sizeof(s_glyph_scratch)) <= 0) return false;
                out->bitmap = s_glyph_scratch;
            } else {
                out->bitmap = s_gb_glyphs + (size_t)idx * s_hdr->gb_bytes_per_char;
            }
            return true;
        }
        if (c < cp) lo = mid + 1;
        else hi = mid - 1;
    }
    return false;
}

/* ============ V1.0.88: TF 卡字库加载 ============ */

#define SD_FONT_DIR "/sdcard/fonts"

bool font_book_select_file(const char *path) {
    if (!path || !path[0]) return false;

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "无法打开字库: %s", path);
        s_error = "无法打开字库文件";
        return false;
    }

    /* 获取文件大小 */
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize < (long)sizeof(book_font_header_t) || fsize > 4 * 1024 * 1024) {
        ESP_LOGW(TAG, "字库文件大小异常: %ld", fsize);
        fclose(f);
        s_error = "字库文件大小异常";
        return false;
    }

    /* 分配 PSRAM 缓冲 */
    uint8_t *buf = heap_caps_malloc((size_t)fsize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        ESP_LOGE(TAG, "PSRAM 分配失败 (%ld bytes)", fsize);
        fclose(f);
        s_error = "内存不足";
        return false;
    }

    size_t got = fread(buf, 1, (size_t)fsize, f);
    fclose(f);
    if ((long)got != fsize) {
        ESP_LOGW(TAG, "读取不完整: %zu/%ld", got, fsize);
        free(buf);
        s_error = "读取不完整";
        return false;
    }

    /* 校验 magic */
    if (memcmp(buf, FONT_MAGIC, 8) != 0) {
        ESP_LOGW(TAG, "字库魔数错误: %s", path);
        free(buf);
        s_error = "字库魔数错误";
        return false;
    }

    /* 释放旧缓冲 */
    if (s_sd_buf) {
        free(s_sd_buf);
    }

    s_sd_buf = buf;
    s_sd_buf_size = (size_t)fsize;
    s_sd_active = true;

    if (!font_bind(buf, s_sd_buf_size)) {
        ESP_LOGW(TAG, "字库绑定失败: %s", path);
        s_sd_active = false;
        /* 不立即释放, 让错误信息可查; 下次加载会替换 */
        s_error = "字库绑定失败";
        return false;
    }

    /* SD 卡 .fnt 成功加载, 切到 .fnt 模式(覆盖 UI 字体) */
    s_using_ui = false;
    ESP_LOGI(TAG, "TF 卡字库加载成功: %s (%ld bytes, cell=%dx%d)",
             path, fsize, s_hdr ? s_hdr->cell_w : 0, s_hdr ? s_hdr->cell_h : 0);
    return true;
}

int font_book_scan_sd(char names[][64], int max) {
    if (!names || max <= 0) return 0;

    DIR *dir = opendir(SD_FONT_DIR);
    if (!dir) {
        ESP_LOGI(TAG, "字体目录不存在: %s", SD_FONT_DIR);
        return 0;
    }

    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && count < max) {
        /* 只收集 .fnt 文件 */
        const char *dot = strrchr(ent->d_name, '.');
        if (dot && strcasecmp(dot, ".fnt") == 0) {
            memcpy(names[count], ent->d_name, 63);
            names[count][63] = '\0';
            count++;
        }
    }
    closedir(dir);
    ESP_LOGI(TAG, "扫描到 %d 个 TF 卡字体", count);
    return count;
}

bool font_book_is_sd_active(void) {
    return s_sd_active;
}
