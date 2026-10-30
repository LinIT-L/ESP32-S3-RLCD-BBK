#ifndef LBF_H
#define LBF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
 * LBF —— LinTOS 阅读器统一字体格式（唯一正式运行格式）
 *
 * 文件布局（全部小端）：
 *   [0,96)            lbf_header_t
 *   [cmap_off, +4*n)  u32 码点数组，**升序**（二分查找的前提）
 *   [idx_off, +8*n)   lbf_idx_t 数组，与 cmap 一一对应
 *   [data_off, +len)  字形数据区，1bpp 行优先，每行 (w+7)/8 字节，MSB 在左，1=墨点
 *
 * 设计约束：
 *   - 只存**原生尺寸**字形，2 倍放大由渲染层做严格最近邻扩展，不占文件体积
 *   - 字形按需读取（fseek + fread 单字），不整库载入内部 RAM
 *   - cmap + idx 载入 PSRAM 常驻（26k 字约 310KB），字形走有界 LRU
 *   - magic / 版本 / CRC 三重校验，损坏文件明确报错，不静默降级
 * ========================================================================== */

#define LBF_MAGIC        0x3146424Cu   /* 'L','B','F','1' 小端 */
#define LBF_VER_MAJOR    1
#define LBF_VER_MINOR    0
#define LBF_HEADER_SIZE  96

/* flags */
#define LBF_FLAG_PROPORTIONAL (1u << 0)
#define LBF_FLAG_HAS_ASCII    (1u << 1)
#define LBF_FLAG_CJK          (1u << 2)

/* src_format */
#define LBF_SRC_BDF   0
#define LBF_SRC_TTF   1
#define LBF_SRC_OTF   2
#define LBF_SRC_PCF   3
#define LBF_SRC_HZK   4
#define LBF_SRC_FNT   5

/* 单个字形位图字节上限：32px × 4 字节/行 = 128，留余量到 160 */
#define LBF_MAX_GLYPH_BYTES 160
/* 字形缓存槽数（**仅降级路径**使用；预加载模式下不分配） */
#define LBF_CACHE_SLOTS 64

/* ★★★ 2026-10-31: 整块预加载上限。
 *   打开时若文件 ≤ 该值且 PSRAM 余量够，就把**整个 LBF**读进一块 PSRAM，
 *   cmap / idx / 字形全部落在同一块内存里 ⇒ 取字形是一次指针加法，零 IO 零拷贝。
 *   为什么必须这么做：原来字形只有 64 槽 LRU，而一页 24px 字约 192 个不同字形，
 *   64 槽装不下 ⇒ 几乎每字一次 fseek+fread 打 TF 卡，翻页慢、
 *   切换字体（缓存清空）后更慢。
 *   实测本工程最大字体 1.23 MB（fusion12 37852 字），2 MB 上限留足余量；
 *   超限或 PSRAM 不足时**自动降级**回按需读取，不影响可用性。 */
#define LBF_PRELOAD_MAX (2u * 1024u * 1024u)

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t ver_major;
    uint16_t ver_minor;
    uint32_t header_size;
    uint32_t flags;
    uint8_t  native_w;
    uint8_t  native_h;
    int8_t   baseline;
    uint8_t  line_height;
    uint32_t glyph_count;
    uint32_t cmap_off;
    uint32_t idx_off;
    uint32_t data_off;
    uint32_t data_len;
    uint32_t file_size;
    uint32_t crc32;          /* 本字段按 0 计算 */
    char     name[32];       /* UTF-8，null 补齐 */
    uint8_t  src_format;
    uint8_t  reserved;
    uint8_t  pad[14];
} lbf_header_t;

typedef struct __attribute__((packed)) {
    uint32_t off;   /* 相对 data_off */
    uint8_t  w;
    uint8_t  h;
    int8_t   xoff;
    int8_t   yoff;
} lbf_idx_t;

typedef struct {
    uint32_t       cp;
    uint8_t        w;
    uint8_t        h;
    int8_t         xoff;
    int8_t         yoff;
    const uint8_t *bitmap;   /* 指向字体内部缓存槽，**下次取字即失效** */
} lbf_glyph_t;

typedef struct lbf_font lbf_font_t;

/* 打开并校验 LBF。失败返回 NULL，err 里是中文原因（可为 NULL）。 */
lbf_font_t *lbf_open(const char *path, char *err, size_t errlen);
void        lbf_close(lbf_font_t *f);

/* 2026-10-30: 只读文件头取字体名/原生字号，**不建索引、不分配缓存、不占文件句柄**。
 * 用于字体选择列表 —— 一次进书要列 10+ 个字体，用 lbf_open() 每个都 mmap 索引+缓存
 * 太浪费（每个约 40KB PSRAM）。返回 false = 文件打不开 / 魔数或主版本不符。
 * name 为 UTF-8，取自 LBF 头的 name 字段；该字段为空时回填文件名主干。 */
bool lbf_peek(const char *path, char *name, size_t namelen,
              int *native_w, int *native_h);

/* 取单个字形（原生尺寸）。返回 false = 缺字或读失败。 */
bool lbf_glyph(lbf_font_t *f, uint32_t cp, lbf_glyph_t *out);

/* 取字形并按整数倍率放大：scale 只接受 1 或 2，2 倍为严格最近邻（源像素→2×2）。
 * scale==2 时 bitmap 指向内部共享缓冲，下次调用即失效。 */
bool lbf_glyph_scaled(lbf_font_t *f, uint32_t cp, int scale, lbf_glyph_t *out);

/* 是否包含该码点（只查 cmap，不读字形数据） */
bool lbf_has(lbf_font_t *f, uint32_t cp);

int         lbf_native_w(const lbf_font_t *f);
int         lbf_native_h(const lbf_font_t *f);
int         lbf_baseline(const lbf_font_t *f);
int         lbf_line_height(const lbf_font_t *f);
uint32_t    lbf_glyph_count(const lbf_font_t *f);
const char *lbf_name(const lbf_font_t *f);
const char *lbf_src_format(const lbf_font_t *f);

/* 2026-10-31: 是否走了整块 PSRAM 预加载（true = 取字形零 IO）。
 * 阅读器用它判断"切换字体时是否还要等 IO"，也用于日志核对。 */
bool lbf_is_preloaded(const lbf_font_t *f);
/* 预加载占用字节 / 字体文件字节（未预加载时返回 0） */
uint32_t lbf_img_bytes(const lbf_font_t *f);

/* 运行时统计（缓存命中率 / IO 次数），调试与报告用 */
typedef struct {
    uint32_t hits;
    uint32_t misses;
    uint32_t io_reads;
    uint32_t io_bytes;
    uint16_t cached;
} lbf_stats_t;
void lbf_get_stats(const lbf_font_t *f, lbf_stats_t *st);

#ifdef __cplusplus
}
#endif

#endif /* LBF_H */
