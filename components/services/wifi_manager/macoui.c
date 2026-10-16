/* macoui.c — IEEE OUI 字典查询, 数据在 appdata os_db 槽. */
#include "macoui.h"
#include <string.h>
#include <stdio.h>
#include <esp_heap_caps.h>

typedef struct {
    uint8_t  oui[3];
    uint32_t nameoff;   /* 相对 nameblob 起始 */
} __attribute__((packed)) rec_t;

static const uint8_t *s_base = NULL;   /* 指向 records 起始 */
static int          s_count = 0;
static const char   *s_nb   = NULL;    /* name blob 起始 */

static bool macoui_open(void);

bool macoui_lookup(const uint8_t oui[3], const char **vendor) {
    if (!s_base && !macoui_open()) return false;
    if (!vendor || !s_base || s_count <= 0) return false;
    int lo = 0, hi = s_count - 1;
    const uint8_t *rec = s_base;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        const rec_t *r = (const rec_t *)(rec + mid * sizeof(rec_t));
        int c = (int)(r->oui[0] == oui[0] ? (r->oui[1] == oui[1] ? r->oui[2] - oui[2] : r->oui[1] - oui[1]) : r->oui[0] - oui[0]);
        if (c == 0) { *vendor = s_nb + r->nameoff; return true; }
        if (c < 0) lo = mid + 1; else hi = mid - 1;
    }
    return false;
}

/* ★ 2026-10-13 (linit 要求优化体积): oui_db.bin **不再 EMBED 进固件**(原 294 KB),
 *   改为**首次查询时从 TF 卡读进 PSRAM** —— 与 fonts/font_part.c 同一套路。
 *   卡上没有该文件时 macoui_open() 失败 ⇒ macoui_lookup() 返回 false,
 *   上层显示"未知厂商" ⇒ 优雅降级, 不影响任何其它功能。
 *   文件位置(二选一, 存在即可): /sdcard/oui_db.bin 或 /sdcard/wifi/oui_db.bin */
#define MACOUI_PATH_A "/sdcard/oui_db.bin"
#define MACOUI_PATH_B "/sdcard/wifi/oui_db.bin"
#define MACOUI_MAX_SZ (512 * 1024)

/* 打开 + 解析 TF 卡上的数据 (首次调用触发). */
bool macoui_open(void) {
    static uint8_t *s_buf  = NULL;      /* 常驻 PSRAM, 不释放(记录指针指向它) */
    static size_t   s_blen = 0;
    if (!s_buf) {
        FILE *f = fopen(MACOUI_PATH_A, "rb");
        if (!f) f = fopen(MACOUI_PATH_B, "rb");
        if (!f) return false;
        if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
        long sz = ftell(f);
        if (sz < 12 || sz > MACOUI_MAX_SZ || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return false; }
        s_buf = (uint8_t *)heap_caps_malloc((size_t)sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_buf) { fclose(f); return false; }
        s_blen = fread(s_buf, 1, (size_t)sz, f);
        fclose(f);
        if (s_blen != (size_t)sz) return false;
    }
    const uint8_t *p = s_buf;
    size_t avail = s_blen;
    if (avail < 12 || memcmp(p, "OUID", 4) != 0) return false;
    uint32_t count = (uint32_t)p[4] | ((uint32_t)p[5] << 8) | ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
    uint32_t nsz   = (uint32_t)p[8] | ((uint32_t)p[9] << 8) | ((uint32_t)p[10] << 16) | ((uint32_t)p[11] << 24);
    if (count == 0 || count > 30000) return false;
    size_t need = 12 + (size_t)count * sizeof(rec_t) + nsz + 4;
    if (need > avail) return false;
    s_base  = p + 12;
    s_count = (int)count;
    s_nb    = (const char *)(p + 12 + count * sizeof(rec_t));
    return true;
}