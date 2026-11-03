/* lpk_mode.c — 资源模式判定（纯 C，宿主机可直接单测；规则见 lpk_mode.h 文件头） */
#include "lpk_mode.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 必备资源：每条覆盖一类子系统，缺任何一条都不该启动 ——
 *   ABC.MKF 战斗动作 / FBP.MKF 过场 / GOP.MKF 角色贴图 / MAP.MKF 地图 /
 *   RNG.MKF 场景脚本 / M.MSG 剧情文本 / WORD.DAT 词库 / FONTGLYPH.BIN 汉字字模。
 * 只挑小文件做探针（GOP 11 MB 那种不在启动时读）。 */
static const char *const REQUIRED[] = {
    "ABC.MKF", "FBP.MKF", "GOP.MKF", "MAP.MKF",
    "MGO.MKF", "RNG.MKF", "M.MSG", "WORD.DAT", "FONTGLYPH.BIN",
};

const char *const *lpk_mode_required(int *n) {
    if (n) *n = (int)(sizeof(REQUIRED) / sizeof(REQUIRED[0]));
    return REQUIRED;
}

static void set_why(char *why, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void set_why(char *why, size_t cap, const char *fmt, ...) {
    if (!why || cap == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, cap, fmt, ap);
    va_end(ap);
}

/* ---------------- 安装记录 ---------------- */

int lpk_mode_record_read(const char *dir) {
    if (!dir || !dir[0]) return -1;
    char p[200];
    snprintf(p, sizeof(p), "%s/%s", dir, LPK_MODE_RECORD);
    FILE *f = fopen(p, "rb");
    if (!f) return -1;
    char buf[16] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) return -1;
    for (size_t i = 0; i < n; i++) {                 /* 去空白/换行 */
        if (buf[i] == '\r' || buf[i] == '\n' || buf[i] == ' ' || buf[i] == '\t') buf[i] = 0;
    }
    if (strcmp(buf, "lpk") == 0) return 1;
    if (strcmp(buf, "zip") == 0 || strcmp(buf, "dir") == 0) return 0;
    return -1;
}

int lpk_mode_record_write(const char *dir, int mode) {
    if (!dir || !dir[0]) return -1;
    char p[200];
    snprintf(p, sizeof(p), "%s/%s", dir, LPK_MODE_RECORD);
    FILE *f = fopen(p, "wb");
    if (!f) return -1;
    const char *s = mode ? "lpk\n" : "zip\n";
    size_t n = strlen(s);
    bool ok = fwrite(s, 1, n, f) == n;
    ok = (fflush(f) == 0) && ok;
    fclose(f);
    return ok ? 0 : -1;
}

/* ---------------- 目录模式自检 ---------------- */

/* 大小写两种写法都试：FAT 本身不区分大小写，但这里可能跑在宿主机上做回归 */
static bool probe_file(const char *dir, const char *name) {
    char p[240];
    snprintf(p, sizeof(p), "%s/%s", dir, name);
    FILE *f = fopen(p, "rb");
    if (f) { fclose(f); return true; }
    /* 转小写再试一次（宿主机是大小写敏感的文件系统） */
    char low[64];
    size_t n = strlen(name);
    if (n >= sizeof(low)) return false;
    for (size_t i = 0; i < n; i++)
        low[i] = (name[i] >= 'A' && name[i] <= 'Z') ? (char)(name[i] - 'A' + 'a') : name[i];
    low[n] = 0;
    snprintf(p, sizeof(p), "%s/%s", dir, low);
    f = fopen(p, "rb");
    if (f) { fclose(f); return true; }
    return false;
}

bool lpk_mode_dir_ready(const char *dir, char *why, size_t cap) {
    if (!dir || !dir[0]) { set_why(why, cap, "数据目录为空"); return false; }
    int n = 0;
    const char *const *req = lpk_mode_required(&n);
    for (int i = 0; i < n; i++) {
        if (!probe_file(dir, req[i])) {
            set_why(why, cap, "目录模式缺少 %s", req[i]);
            return false;
        }
    }
    return true;
}

/* ---------------- LPK 自检 ---------------- */

bool lpk_mode_lpk_ready(const char *lpk_path, lpk_entry_t *tab, int maxtab,
                        char *why, size_t cap) {
    if (!lpk_path || !lpk_path[0]) { set_why(why, cap, "未给 LPK 路径"); return false; }

    lpk_t h;
    int rc = lpk_open(&h, lpk_path, NULL, 0);        /* 先只看头，拿到条数 */
    if (rc != LPK_OK) { set_why(why, cap, "LPK 头部校验失败 rc=%d", rc); return false; }
    uint32_t cnt = h.count;
    lpk_close(&h);
    if (cnt == 0) { set_why(why, cap, "LPK 里没有资源条目"); return false; }

    lpk_entry_t *own = NULL;
    lpk_entry_t *t = tab;
    int max = maxtab;
    if (!t || max < (int)cnt) {
        own = (lpk_entry_t *)malloc((size_t)cnt * sizeof(lpk_entry_t));
        if (!own) { set_why(why, cap, "LPK 索引表内存不足 (%d 条)", (int)cnt); return false; }
        t = own; max = (int)cnt;
    }

    rc = lpk_open(&h, lpk_path, t, max);             /* 再读索引：越界/路径/重名都在这里查 */
    if (rc != LPK_OK) {
        free(own);
        set_why(why, cap, "LPK 索引校验失败 rc=%d", rc);
        return false;
    }

    bool ok = true;
    int n = 0;
    const char *const *req = lpk_mode_required(&n);
    for (int i = 0; i < n && ok; i++) {
        if (lpk_find_ci(&h, t, req[i]) < 0) {
            set_why(why, cap, "LPK 缺少 %s", req[i]);
            ok = false;
        }
    }
    lpk_close(&h);
    free(own);
    return ok;
}

/* ---------------- 综合判定 ---------------- */

lpk_res_mode_t lpk_mode_decide(const char *dir, lpk_entry_t *tab, int maxtab,
                               char *why, size_t cap) {
    if (!dir || !dir[0]) { set_why(why, cap, "数据目录为空"); return LPK_RES_NONE; }

    char lpk[240];
    snprintf(lpk, sizeof(lpk), "%s/pal.lpk", dir);

    const int rec = lpk_mode_record_read(dir);       /* 1=lpk 0=zip -1=无 */

    char w1[96] = {0}, w2[96] = {0};
    bool dir_ok = lpk_mode_dir_ready(dir, w1, sizeof(w1));
    bool lpk_ok = lpk_mode_lpk_ready(lpk, tab, maxtab, w2, sizeof(w2));

    if (rec == 1) {                                  /* 安装器明确说是 LPK */
        if (lpk_ok) { set_why(why, cap, "安装记录=lpk 且包自检通过"); return LPK_RES_LPK; }
        if (dir_ok) { set_why(why, cap, "安装记录=lpk 但包不可用, 回落到目录"); return LPK_RES_DIR; }
        set_why(why, cap, "安装记录=lpk 但两种资源都不可用");
        return LPK_RES_NONE;
    }
    if (rec == 0) {                                  /* 安装器明确说是 ZIP */
        if (dir_ok) { set_why(why, cap, "安装记录=zip 且目录自检通过"); return LPK_RES_DIR; }
        if (lpk_ok) { set_why(why, cap, "安装记录=zip 但目录残缺, 改用 LPK"); return LPK_RES_LPK; }
        set_why(why, cap, "安装记录=zip 且目录缺 %s", w1[0] ? w1 : "必备资源");
        return LPK_RES_NONE;
    }

    /* 没有任何记录（老版本装的）：目录优先 —— 历史行为不变 */
    if (dir_ok) { set_why(why, cap, "无安装记录, 目录完整 ⇒ 目录模式"); return LPK_RES_DIR; }
    if (lpk_ok) { set_why(why, cap, "无安装记录, 只有 LPK ⇒ LPK 模式"); return LPK_RES_LPK; }
    set_why(why, cap, "两种资源都不完整");
    return LPK_RES_NONE;
}

const char *lpk_mode_str(lpk_res_mode_t m) {
    switch (m) {
    case LPK_RES_LPK: return "LPK";
    case LPK_RES_DIR: return "ZIP-DIR";
    default:          return "NONE";
    }
}
