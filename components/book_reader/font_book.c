#include "font_book.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "font_book";

/* 由 CMake target_add_binary_data 生成:
 * assets/book16.fnt -> _binary_book16_fnt_start/_end */
/* 仿宋 (fs) / 黑体 (hz, 菜单字体款式) × 16/24/32, 全部原生 GB2312 简体点阵 */
extern const uint8_t book16_fs_fnt_start[] asm("_binary_book16_fs_fnt_start");
extern const uint8_t book16_fs_fnt_end[]   asm("_binary_book16_fs_fnt_end");
extern const uint8_t book24_fs_fnt_start[] asm("_binary_book24_fs_fnt_start");
extern const uint8_t book24_fs_fnt_end[]   asm("_binary_book24_fs_fnt_end");
extern const uint8_t book32_fs_fnt_start[] asm("_binary_book32_fs_fnt_start");
extern const uint8_t book32_fs_fnt_end[]   asm("_binary_book32_fs_fnt_end");
extern const uint8_t book16_hz_fnt_start[] asm("_binary_book16_hz_fnt_start");
extern const uint8_t book16_hz_fnt_end[]   asm("_binary_book16_hz_fnt_end");
extern const uint8_t book24_hz_fnt_start[] asm("_binary_book24_hz_fnt_start");
extern const uint8_t book24_hz_fnt_end[]   asm("_binary_book24_hz_fnt_end");
extern const uint8_t book32_hz_fnt_start[] asm("_binary_book32_hz_fnt_start");
extern const uint8_t book32_hz_fnt_end[]   asm("_binary_book32_hz_fnt_end");

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
    return font_bind(book24_fs_fnt_start, (size_t)(book24_fs_fnt_end - book24_fs_fnt_start));
}

/* 样式: 0=仿宋 1=黑体; 字号: 0=16 1=24 2=32 */
void font_book_select(int style, int size_id) {
    const uint8_t *d;
    size_t len;
    int st = (style < 0) ? 0 : (style > 1 ? 1 : style);
    int sz = (size_id < 0) ? 1 : (size_id > 2 ? 2 : size_id);
    if (st == 1) {   /* 黑体 (菜单字体款式) */
        if (sz == 0)      { d = book16_hz_fnt_start; len = (size_t)(book16_hz_fnt_end - book16_hz_fnt_start); }
        else if (sz == 2) { d = book32_hz_fnt_start; len = (size_t)(book32_hz_fnt_end - book32_hz_fnt_start); }
        else              { d = book24_hz_fnt_start; len = (size_t)(book24_hz_fnt_end - book24_hz_fnt_start); }
    } else {           /* 仿宋 */
        if (sz == 0)      { d = book16_fs_fnt_start; len = (size_t)(book16_fs_fnt_end - book16_fs_fnt_start); }
        else if (sz == 2) { d = book32_fs_fnt_start; len = (size_t)(book32_fs_fnt_end - book32_fs_fnt_start); }
        else              { d = book24_fs_fnt_start; len = (size_t)(book24_fs_fnt_end - book24_fs_fnt_start); }
    }
    /* 释放旧 SD 缓冲 (切回内嵌) */
    if (s_sd_buf) {
        free(s_sd_buf);
        s_sd_buf = NULL;
        s_sd_buf_size = 0;
    }
    s_sd_active = false;
    font_bind(d, len);
}

/* V1.0.88: 切回内嵌字库 (退出 SD 字库模式) */
void font_book_select_embedded(int style, int size_id) {
    font_book_select(style, size_id);
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
            strncpy(names[count], ent->d_name, 63);
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
