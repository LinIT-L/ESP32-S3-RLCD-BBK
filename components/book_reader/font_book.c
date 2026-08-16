#include "font_book.h"

#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"

/* 由 CMake target_add_binary_data 生成:
 * assets/book16.fnt -> _binary_book16_fnt_start/_end */
extern const uint8_t book20_fnt_start[] asm("_binary_book20_fnt_start");
extern const uint8_t book20_fnt_end[]   asm("_binary_book20_fnt_end");
extern const uint8_t book24_fnt_start[] asm("_binary_book24_fnt_start");
extern const uint8_t book24_fnt_end[]   asm("_binary_book24_fnt_end");
extern const uint8_t book28_fnt_start[] asm("_binary_book28_fnt_start");
extern const uint8_t book28_fnt_end[]   asm("_binary_book28_fnt_end");
extern const uint8_t book32_fnt_start[] asm("_binary_book32_fnt_start");
extern const uint8_t book32_fnt_end[]   asm("_binary_book32_fnt_end");

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

bool font_book_init(void) {
    return font_bind(book24_fnt_start, (size_t)(book24_fnt_end - book24_fnt_start));
}

void font_book_select(int size_id) {
    const uint8_t *d;
    size_t len;
    if (size_id == 0) { d = book20_fnt_start; len = (size_t)(book20_fnt_end - book20_fnt_start); }
    else if (size_id == 2) { d = book28_fnt_start; len = (size_t)(book28_fnt_end - book28_fnt_start); }
    else if (size_id == 3) { d = book32_fnt_start; len = (size_t)(book32_fnt_end - book32_fnt_start); }
    else { d = book24_fnt_start; len = (size_t)(book24_fnt_end - book24_fnt_start); }
    font_bind(d, len);
}

const char *font_book_error(void) {
    return s_error ? s_error : "";
}

int font_book_cell_w(void) { return s_hdr ? s_hdr->cell_w : 16; }
int font_book_cell_h(void) { return s_hdr ? s_hdr->cell_h : 16; }
int font_book_ascii_w(void) { return s_hdr ? s_hdr->ascii_w : 8; }
int font_book_ascii_h(void) { return s_hdr ? s_hdr->ascii_h : 16; }

bool font_book_glyph_ascii(uint8_t c, book_glyph_t *out) {
    if (!s_hdr || c < 0x20 || c >= 0x20 + s_hdr->ascii_count) return false;
    out->w = s_hdr->ascii_w;
    out->h = s_hdr->ascii_h;
    out->bitmap = s_ascii + (size_t)(c - 0x20) * s_hdr->ascii_bytes_per_char;
    return true;
}

bool font_book_glyph_gb(uint8_t hi, uint8_t lo, book_glyph_t *out) {
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
