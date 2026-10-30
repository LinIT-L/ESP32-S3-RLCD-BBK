#include "lbf.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "lbf";

/* 2 倍放大的输出缓冲：32px 原生 → 64px，行宽 8 字节 × 64 行 = 512 */
#define LBF_SCALE_BUF_BYTES (LBF_MAX_GLYPH_BYTES * 4)
static uint8_t s_scale_buf[LBF_SCALE_BUF_BYTES];

typedef struct {
    uint32_t cp;
    uint8_t  used;
    uint8_t  w, h;
    int8_t   xoff, yoff;
    uint32_t tick;
    uint8_t  bmp[LBF_MAX_GLYPH_BYTES];
} lbf_slot_t;

struct lbf_font {
    FILE       *fp;          /* 未预加载时的按需读取句柄；预加载成功后置 NULL */
    uint8_t    *img;         /* ★ 2026-10-31: 整个 LBF 文件映像 (PSRAM 一整块) */
    size_t      img_sz;
    lbf_header_t hdr;
    uint32_t   *cmap;        /* 预加载时指向 img 内部；否则独立 malloc */
    lbf_idx_t  *idx;         /* 同上 */
    lbf_slot_t *cache;       /* 仅在**未预加载**时分配（按需读取的 LRU 槽） */
    uint32_t    tick;
    lbf_stats_t st;
    bool        ok;
    bool        preloaded;
};

static const char *src_name(uint8_t s) {
    switch (s) {
    case LBF_SRC_BDF: return "bdf";
    case LBF_SRC_TTF: return "ttf";
    case LBF_SRC_OTF: return "otf";
    case LBF_SRC_PCF: return "pcf";
    case LBF_SRC_HZK: return "hzk";
    case LBF_SRC_FNT: return "fnt";
    default:          return "?";
    }
}

static void set_err(char *err, size_t len, const char *msg) {
    if (err && len) {
        strncpy(err, msg, len - 1);
        err[len - 1] = '\0';
    }
}

static inline size_t glyph_bytes(uint8_t w, uint8_t h) {
    return (size_t)((w + 7) / 8) * (size_t)h;
}

lbf_font_t *lbf_open(const char *path, char *err, size_t errlen) {
    if (!path || !path[0]) { set_err(err, errlen, "路径为空"); return NULL; }

    FILE *fp = fopen(path, "rb");
    if (!fp) { set_err(err, errlen, "打不开文件"); return NULL; }

    lbf_header_t h;
    if (fread(&h, 1, sizeof(h), fp) != sizeof(h)) {
        fclose(fp); set_err(err, errlen, "文件短于文件头"); return NULL;
    }
    if (h.magic != LBF_MAGIC) {
        fclose(fp); set_err(err, errlen, "魔数不是 LBF1"); return NULL;
    }
    if (h.ver_major != LBF_VER_MAJOR) {
        fclose(fp); set_err(err, errlen, "主版本不兼容"); return NULL;
    }
    if (h.header_size != LBF_HEADER_SIZE) {
        fclose(fp); set_err(err, errlen, "头长度不符"); return NULL;
    }
    /* 结构合法性：所有段必须落在文件内且不重叠 */
    if (h.glyph_count == 0 || h.native_w == 0 || h.native_h == 0) {
        fclose(fp); set_err(err, errlen, "字形数为 0 或尺寸为 0"); return NULL;
    }
    uint64_t cmap_end = (uint64_t)h.cmap_off + (uint64_t)h.glyph_count * 4u;
    uint64_t idx_end  = (uint64_t)h.idx_off  + (uint64_t)h.glyph_count * 8u;
    uint64_t data_end = (uint64_t)h.data_off + (uint64_t)h.data_len;
    if (h.cmap_off != LBF_HEADER_SIZE || idx_end != h.data_off ||
        cmap_end > h.idx_off || data_end > h.file_size) {
        fclose(fp); set_err(err, errlen, "段偏移不自洽"); return NULL;
    }

    fseek(fp, 0, SEEK_END);
    long real = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (real < (long)h.file_size) {
        fclose(fp); set_err(err, errlen, "文件被截断"); return NULL;
    }

    lbf_font_t *f = (lbf_font_t *)heap_caps_calloc(1, sizeof(lbf_font_t),
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!f) { fclose(fp); set_err(err, errlen, "PSRAM 不足(控制块)"); return NULL; }
    f->fp = fp;
    f->hdr = h;

    size_t cmap_sz = (size_t)h.glyph_count * 4u;
    size_t idx_sz  = (size_t)h.glyph_count * 8u;

    /* ★★★ 2026-10-31: 整个字体一次性读进 PSRAM（见 lbf.h 里 LBF_PRELOAD_MAX 的说明）。
     *   原来只有 cmap+idx 常驻（约 300KB），**字形**走 64 槽 LRU；而一页 24px 字
     *   约 192 个不同字形 —— 64 槽装不下，等于每字一次 fseek+fread 打 TF 卡。
     *   翻页慢、切字体后更慢，根子都在这里。
     *   改成整块映像后 cmap / idx / glyph 全在同一块内存，取字形退化为一次指针加法。 */
    size_t free_ps = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (h.file_size <= LBF_PRELOAD_MAX && free_ps >= (size_t)h.file_size + (1u << 20)) {
        f->img = (uint8_t *)heap_caps_malloc((size_t)h.file_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (f->img) {
            if (fseek(fp, 0, SEEK_SET) == 0 &&
                fread(f->img, 1, (size_t)h.file_size, fp) == (size_t)h.file_size) {
                f->img_sz    = (size_t)h.file_size;
                f->preloaded = true;
                f->cmap = (uint32_t *)(f->img + h.cmap_off);
                f->idx  = (lbf_idx_t *)(f->img + h.idx_off);
                fclose(fp);            /* 句柄也放掉：不占 FAT 资源，睡眠/拔卡都不受影响 */
                f->fp = NULL;
            } else {
                heap_caps_free(f->img);
                f->img = NULL;
            }
        }
    }

    if (!f->preloaded) {
        /* 降级路径：按需读取 + 有界 LRU（内存不足或文件超大时走这里，行为与原实现一致） */
        f->cmap = (uint32_t *)heap_caps_malloc(cmap_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        f->idx  = (lbf_idx_t *)heap_caps_malloc(idx_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        f->cache = (lbf_slot_t *)heap_caps_calloc(LBF_CACHE_SLOTS, sizeof(lbf_slot_t),
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!f->cmap || !f->idx || !f->cache) {
            lbf_close(f);
            set_err(err, errlen, "PSRAM 不足(索引)");
            return NULL;
        }
        if (fseek(fp, (long)h.cmap_off, SEEK_SET) != 0 ||
            fread(f->cmap, 1, cmap_sz, fp) != cmap_sz ||
            fread(f->idx, 1, idx_sz, fp) != idx_sz) {
            lbf_close(f); set_err(err, errlen, "索引读取不完整"); return NULL;
        }
    }

    /* cmap 必须严格升序，否则二分失效 —— 宁可拒绝也不给错字 */
    for (uint32_t i = 1; i < h.glyph_count; i++) {
        if (f->cmap[i] <= f->cmap[i - 1]) {
            lbf_close(f); set_err(err, errlen, "码点表非升序"); return NULL;
        }
    }
    /* 索引项边界自检 */
    for (uint32_t i = 0; i < h.glyph_count; i++) {
        const lbf_idx_t *e = &f->idx[i];
        if (e->w == 0 || e->h == 0) { continue; }
        if ((uint64_t)e->off + glyph_bytes(e->w, e->h) > h.data_len) {
            lbf_close(f); set_err(err, errlen, "字形越界"); return NULL;
        }
    }

    f->ok = true;
    ESP_LOGI(TAG, "LBF 已加载: %s (%ux%u, %u 字, %u B, 来源 %s, %s)",
             h.name, h.native_w, h.native_h, (unsigned)h.glyph_count,
             (unsigned)h.file_size, src_name(h.src_format),
             f->preloaded ? "整块 PSRAM·零IO" : "按需读取·64槽LRU");
    return f;
}

void lbf_close(lbf_font_t *f) {
    if (!f) return;
    if (f->fp) fclose(f->fp);
    if (f->img) {
        /* ★ 预加载时 cmap / idx 是指向 img 内部的指针，**不能**单独 free */
        heap_caps_free(f->img);
    } else {
        if (f->cmap) heap_caps_free(f->cmap);
        if (f->idx) heap_caps_free(f->idx);
    }
    if (f->cache) heap_caps_free(f->cache);
    heap_caps_free(f);
}

static int cmap_find(lbf_font_t *f, uint32_t cp) {
    int lo = 0, hi = (int)f->hdr.glyph_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        uint32_t v = f->cmap[mid];
        if (v == cp) return mid;
        if (v < cp) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

bool lbf_has(lbf_font_t *f, uint32_t cp) {
    return f && f->ok && cmap_find(f, cp) >= 0;
}

bool lbf_glyph(lbf_font_t *f, uint32_t cp, lbf_glyph_t *out) {
    if (!f || !f->ok || !out) return false;

    /* ★★★ 2026-10-31: 预加载路径 —— 零 IO、零拷贝，位图直接指向 PSRAM 映像内部。
     *   这是"翻页不再被字体拖慢"的关键：一次指针加法代替一次 fseek+fread。
     *   也因为不用往缓存槽拷，cache 在预加载模式下根本不分配（省 64×~90B）。 */
    if (f->preloaded) {
        int gi = cmap_find(f, cp);
        if (gi < 0) { f->st.misses++; return false; }
        const lbf_idx_t *e = &f->idx[gi];
        if (e->w == 0 || e->h == 0 ||
            (uint64_t)e->off + glyph_bytes(e->w, e->h) > f->hdr.data_len) {
            f->st.misses++;
            return false;
        }
        f->st.hits++;
        out->cp   = cp;
        out->w    = e->w;
        out->h    = e->h;
        out->xoff = e->xoff;
        out->yoff = e->yoff;
        out->bitmap = f->img + f->hdr.data_off + e->off;
        return true;
    }

    for (int i = 0; i < LBF_CACHE_SLOTS; i++) {
        lbf_slot_t *s = &f->cache[i];
        if (s->used && s->cp == cp) {
            s->tick = ++f->tick;
            f->st.hits++;
            out->cp = cp; out->w = s->w; out->h = s->h;
            out->xoff = s->xoff; out->yoff = s->yoff;
            out->bitmap = s->bmp;
            return true;
        }
    }
    f->st.misses++;

    int gi = cmap_find(f, cp);
    if (gi < 0) return false;

    const lbf_idx_t *e = &f->idx[gi];
    size_t nbytes = glyph_bytes(e->w, e->h);
    if (nbytes == 0 || nbytes > LBF_MAX_GLYPH_BYTES) return false;

    /* 选槽：空槽优先，否则淘汰 tick 最小（最久未用） */
    lbf_slot_t *victim = NULL;
    for (int i = 0; i < LBF_CACHE_SLOTS; i++) {
        if (!f->cache[i].used) { victim = &f->cache[i]; break; }
    }
    if (!victim) {
        uint32_t oldest = 0xFFFFFFFFu;
        for (int i = 0; i < LBF_CACHE_SLOTS; i++) {
            if (f->cache[i].tick < oldest) { oldest = f->cache[i].tick; victim = &f->cache[i]; }
        }
    }
    if (!victim) return false;

    if (fseek(f->fp, (long)(f->hdr.data_off + e->off), SEEK_SET) != 0) return false;
    if (fread(victim->bmp, 1, nbytes, f->fp) != nbytes) return false;

    f->st.io_reads++;
    f->st.io_bytes += (uint32_t)nbytes;

    victim->used = 1;
    victim->cp = cp;
    victim->w = e->w;
    victim->h = e->h;
    victim->xoff = e->xoff;
    victim->yoff = e->yoff;
    victim->tick = ++f->tick;

    out->cp = cp; out->w = e->w; out->h = e->h;
    out->xoff = e->xoff; out->yoff = e->yoff;
    out->bitmap = victim->bmp;
    return true;
}

bool lbf_glyph_scaled(lbf_font_t *f, uint32_t cp, int scale, lbf_glyph_t *out) {
    if (scale != 1 && scale != 2) return false;
    lbf_glyph_t g;
    if (!lbf_glyph(f, cp, &g)) return false;
    if (scale == 1) { *out = g; return true; }

    int ow = g.w * 2, oh = g.h * 2;
    int dbpr = (ow + 7) / 8;
    size_t need = (size_t)dbpr * oh;
    if (need > sizeof(s_scale_buf)) return false;
    memset(s_scale_buf, 0, need);

    int sbpr = (g.w + 7) / 8;
    for (int y = 0; y < g.h; y++) {
        for (int x = 0; x < g.w; x++) {
            if (!(g.bitmap[y * sbpr + (x >> 3)] & (0x80 >> (x & 7)))) continue;
            int tx = x * 2, ty = y * 2;
            for (int dy = 0; dy < 2; dy++) {
                uint8_t *row = s_scale_buf + (size_t)(ty + dy) * dbpr;
                row[(tx) >> 3]      |= (uint8_t)(0x80 >> (tx & 7));
                row[(tx + 1) >> 3]  |= (uint8_t)(0x80 >> ((tx + 1) & 7));
            }
        }
    }
    out->cp = cp;
    out->w = (uint8_t)ow;
    out->h = (uint8_t)oh;
    out->xoff = (int8_t)(g.xoff * 2);
    out->yoff = (int8_t)(g.yoff * 2);
    out->bitmap = s_scale_buf;
    return true;
}

int      lbf_native_w(const lbf_font_t *f)    { return f ? f->hdr.native_w : 0; }
int      lbf_native_h(const lbf_font_t *f)    { return f ? f->hdr.native_h : 0; }
int      lbf_baseline(const lbf_font_t *f)    { return f ? f->hdr.baseline : 0; }
int      lbf_line_height(const lbf_font_t *f) { return f ? f->hdr.line_height : 0; }
uint32_t lbf_glyph_count(const lbf_font_t *f) { return f ? f->hdr.glyph_count : 0; }
const char *lbf_name(const lbf_font_t *f)     { return f ? f->hdr.name : ""; }
const char *lbf_src_format(const lbf_font_t *f) { return f ? src_name(f->hdr.src_format) : ""; }

void lbf_get_stats(const lbf_font_t *f, lbf_stats_t *st) {
    if (!f || !st) return;
    *st = f->st;
    st->cached = LBF_CACHE_SLOTS;
}

/* ===== 2026-10-30: 只读文件头 (字体选择列表用) ==========================
 * 为什么单独做: 列表要显示每个字体的名字, 但 lbf_open() 会分配 cmap/idx/缓存
 * (约 40KB PSRAM/字)。一次进书列 10+ 个字体就不划算了 —— 这里只读 96 字节头。 */
bool lbf_peek(const char *path, char *name, size_t namelen,
              int *native_w, int *native_h)
{
    if (!path || !path[0]) return false;
    FILE *fp = fopen(path, "rb");
    if (!fp) return false;

    lbf_header_t h;
    bool ok = (fread(&h, 1, sizeof(h), fp) == sizeof(h)) &&
              h.magic == LBF_MAGIC && h.ver_major == LBF_VER_MAJOR &&
              h.header_size == LBF_HEADER_SIZE &&
              h.glyph_count > 0 && h.native_w > 0 && h.native_h > 0;
    fclose(fp);
    if (!ok) return false;

    if (name && namelen) {
        /* 头里 name[32], 不保证以 0 结尾 */
        size_t n = 0;
        while (n < 32 && h.name[n]) n++;
        if (n) {
            if (n >= namelen) n = namelen - 1;
            memcpy(name, h.name, n);
            name[n] = 0;
        } else {
            /* 没写名字 ⇒ 用文件名主干, 不擅自造名字 */
            const char *base = strrchr(path, '/');
            base = base ? base + 1 : path;
            size_t k = 0;
            while (base[k] && base[k] != '.' && k + 1 < namelen && k < 63) { name[k] = base[k]; k++; }
            name[k] = 0;
        }
    }
    if (native_w) *native_w = h.native_w;
    if (native_h) *native_h = h.native_h;
    return true;
}

/* 2026-10-31: 预加载状态查询（见 lbf.h 说明） */
bool lbf_is_preloaded(const lbf_font_t *f) { return f && f->preloaded; }
uint32_t lbf_img_bytes(const lbf_font_t *f) { return f && f->preloaded ? (uint32_t)f->img_sz : 0; }
