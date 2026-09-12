/* macoui.c — IEEE OUI 字典查询, 数据在 appdata os_db 槽. */
#include "macoui.h"
#include "font_part.h"
#include <string.h>

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

/* 打开 + mmap + 解析头 (首次调用触发). */
bool macoui_open(void) {
    const uint8_t *p = font_part_map(MACOUI_DB_OFF, MACOUI_DB_MAXLEN);
    if (!p || memcmp(p, "OUID", 4) != 0) return false;
    uint32_t count = (uint32_t)p[4] | ((uint32_t)p[5] << 8) | ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
    uint32_t nsz   = (uint32_t)p[8] | ((uint32_t)p[9] << 8) | ((uint32_t)p[10] << 16) | ((uint32_t)p[11] << 24);
    if (count == 0 || count > 30000) return false;
    size_t need = 12 + (size_t)count * sizeof(rec_t) + nsz + 4;
    if (need > MACOUI_DB_MAXLEN) return false;
    s_base  = p + 12;
    s_count = (int)count;
    s_nb    = (const char *)(p + 12 + count * sizeof(rec_t));
    return true;
}