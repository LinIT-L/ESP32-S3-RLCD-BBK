/**
 * am_store_state.c — 商店条目状态判定（纯 C 实现，见头文件的设计说明）
 *
 * 本文件不得 include 任何 ESP-IDF / FreeRTOS / os.h 头，也不得做文件 IO ——
 * 目的是让 tests/store_state/ 能直接用宿主机 clang 编译同一份代码跑断言，
 * 避免"测试副本漂移"（LPK 读取器当初就是这么要求的，这里沿用同一条规矩）。
 */
#include "am_store_state.h"

#include <string.h>

/* ---------------- 版本比较 ---------------- */

int am_st_ver_newer(const char *lbuild, unsigned lver,
                    const char *rbuild, unsigned rver)
{
    const bool has_lb = (lbuild && lbuild[0]);
    const bool has_rb = (rbuild && rbuild[0]);

    if (has_rb) {
        if (!has_lb) return 1;                       /* ② 老包 → 提示一次迁移更新 */
        int c = strcmp(rbuild, lbuild);
        return (c > 0) ? 1 : (c < 0) ? -1 : 0;       /* ① 定长数字串，字典序==时间序 */
    }
    /* ③ 老索引没有 build：退回数字版本比较。未知版本(0)按最低算。 */
    unsigned lv = lver ? lver : 1u;
    if (rver > lv) return 1;
    if (rver && rver < lv) return -1;
    return 0;
}

am_st_t am_st_decide(bool installed,
                     const char *lbuild, unsigned lver,
                     const char *rbuild, unsigned rver)
{
    if (!installed) return AM_ST_NONE;
    /* 商店侧没有可用版本信息（例如 zip 资源包没有 ver/build）→ 视为已是最新，
     * 保持"已安装 = 打开"，不凭空空造一个"更新"。 */
    if ((!rbuild || !rbuild[0]) && rver == 0) return AM_ST_INSTALLED;
    return (am_st_ver_newer(lbuild, lver, rbuild, rver) > 0) ? AM_ST_UPDATE
                                                             : AM_ST_INSTALLED;
}

/* ---------------- 唯一更新判据：服务器文件修改时间 ---------------- */

/* 两份"文件身份"是否相同。crc 为 0 视为"不知道"，此时不算相同 —— 
 * 宁可多提示一次更新，也不要因为缺字段把真正的新版本吞掉。 */
static bool same_content(uint32_t crc_a, uint32_t size_a,
                         uint32_t crc_b, uint32_t size_b)
{
    if (crc_a == 0 || crc_b == 0) return false;
    if (crc_a != crc_b) return false;
    if (size_a && size_b && size_a != size_b) return false;
    return true;
}

am_st_t am_st_decide_meta(const am_st_meta_t *m, bool *backfill)
{
    if (backfill) *backfill = false;
    if (!m || !m->installed) return AM_ST_NONE;

    /* ★ 2026-11-03: **本地独有** —— 装着、但服务器索引里确实没有这一条
     *   （应用已下架/被删除）。规矩：商店仍要保留该条目，按钮给「卸载」，
     *   绝不显示「获取」（那会让用户以为没装）。
     *   两个门必须同时成立，否则会误判：
     *     · orphan_ok —— 只有"本地行"才代表真实安装；云端行解析失败不算；
     *     · idx_ready —— 索引必须已成功拉到过；离线时 has_idx=false 只表示"不知道"。 */
    if (m->orphan_ok && m->idx_ready && !m->has_idx) return AM_ST_ORPHAN;

    /* 索引里没有这条应用的资料（例如刚下载完还没刷新索引）→ 不造"更新" */
    if (!m->has_idx) return AM_ST_INSTALLED;

    const bool idx_has_time = (m->idx_mtime != 0);

    /* ① 有安装记录且记录了服务器 mtime —— 正常路径，只比时间 */
    if (m->has_rec && m->rec_mtime != 0) {
        if (idx_has_time && m->idx_mtime > m->rec_mtime) {
            /* 时间前移了，但内容完全一致（重新生成索引/重新打包没改内容）
             * → 不提示更新，并把记录推进到新 mtime，避免以后每次都提示。 */
            if (same_content(m->rec_crc, m->rec_size, m->idx_crc, m->idx_size)) {
                if (backfill) *backfill = true;
                return AM_ST_INSTALLED;
            }
            return AM_ST_UPDATE;
        }
        return AM_ST_INSTALLED;
    }

    /* ② 有记录但没 mtime（早期记录/老索引）→ 比内容 */
    if (m->has_rec && m->rec_crc != 0) {
        if (same_content(m->rec_crc, m->rec_size, m->idx_crc, m->idx_size)) {
            if (backfill && idx_has_time) *backfill = true;
            return AM_ST_INSTALLED;
        }
        return AM_ST_UPDATE;
    }

    /* ③ 老应用（升级前装的，没有任何记录）→ 用本地实测 size+crc32 与索引核对。
     *    一致 = 就是商店现在这一份 ⇒ 认定已是最新，并补记当前 mtime（一次迁移，之后走 ①）。
     *    不一致 = 本地确实是旧版本 ⇒ 提示更新。
     *    本地文件读不到（无存储/权限）⇒ 不敢下结论，保持"已安装"，不造更新。 */
    if (m->loc_crc_ok) {
        if (same_content(m->loc_crc, m->loc_size, m->idx_crc, m->idx_size)) {
            if (backfill) *backfill = true;
            return AM_ST_INSTALLED;
        }
        return AM_ST_UPDATE;
    }

    /* ④ 索引连内容标识都没有（极老索引）→ 退回版本号比较，仍不参与 mtime 逻辑 */
    return AM_ST_INSTALLED;
}

am_st_t am_st_with_task(am_st_t base, am_st_task_t task)
{
    switch (task) {
    case AM_TASK_DL:        return AM_ST_DL;
    case AM_TASK_INSTALL:   return (base == AM_ST_UPDATE) ? AM_ST_UPDATING : AM_ST_INSTALLING;
    case AM_TASK_UPDATE:    return AM_ST_UPDATING;
    case AM_TASK_UNINSTALL: return AM_ST_UNINSTALLING;
    case AM_TASK_NONE:
    default:                return base;
    }
}

const char *am_st_btn(am_st_t st)
{
    /* ★ 2026-11-03 linit 定稿：商店只允许五种按钮文字
     *   **下载 / 安装 / 下载中 / 更新 / 卸载**。
     *   「获取」「打开」「已安装」都不再出现：
     *   · 未装 = 下载（LPK 本地有包时由 am_core_btn_of 改写成「安装」/「更新」）；
     *   · 已装且最新 = 卸载（打开应用走详情弹窗，不占按钮）。 */
    switch (st) {
    case AM_ST_INSTALLED:    return "卸载";
    case AM_ST_UPDATE:       return "更新";
    case AM_ST_ORPHAN:       return "卸载";   /* 服务器已无此条: 只能卸, 不给"下载" */
    case AM_ST_DL:           return "下载中";
    case AM_ST_INSTALLING:   return "安装中";
    case AM_ST_UPDATING:     return "更新中";
    case AM_ST_UNINSTALLING: return "卸载中";
    case AM_ST_NONE:
    default:                 return "下载";
    }
}

bool am_st_btn_busy(am_st_t st)
{
    return (st == AM_ST_DL || st == AM_ST_INSTALLING ||
            st == AM_ST_UPDATING || st == AM_ST_UNINSTALLING);
}

/* ---------------- 身份归一 ----------------
 * ★ 2026-11-03: am_st_alias（历史别名白名单）与 am_st_same_app（按声明名合并）
 *   已**删除**。身份现在只有一个来源：am_app_core 的 key
 *   （manifest.app_id / 规范化文件名，带类型前缀）。
 *   ⛔ 不再保留"码表 → 自行车码表"这类白名单，也不再按显示名做近似匹配 ——
 *      那两套规则本身就是"同一个应用两条记录"和"两个应用被并成一条"的来源。
 */

const char *am_st_display_name(const char *meta_zh, const char *meta_en,
                               const char *fname)
{
    if (meta_zh && meta_zh[0]) return meta_zh;
    if (meta_en && meta_en[0]) return meta_en;
    if (fname && fname[0])     return fname;
    return "";
}

/* ---------------- registry 解析 ---------------- */

/* 取一行的 `键=值`：键必须在行首（不跳过空白，registry 是本工程自己写的，
 * 格式固定；宁可严格也不接受带前导空格的行，免得把正文里的 "x=y" 误当字段）。 */
static bool line_kv(const char *line, size_t len, size_t *klen, size_t *voff, size_t *vlen)
{
    size_t i = 0;
    while (i < len && line[i] != '=') i++;
    if (i >= len || i == 0) return false;
    *klen = i;
    *voff = i + 1;
    *vlen = len - i - 1;
    return true;
}

static void copy_field(char *dst, size_t cap, const char *src, size_t n)
{
    if (!dst || !cap) return;
    if (dst[0]) return;                 /* 第一次为准，后续补丁行不覆盖 */
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

bool am_st_reg_parse(const char *txt, size_t len, am_st_reg_t *out)
{
    if (!txt || !out) return false;
    memset(out, 0, sizeof(*out));
    if (len == 0) len = strlen(txt);

    size_t pos = 0;
    while (pos < len) {
        size_t eol = pos;
        while (eol < len && txt[eol] != '\n') eol++;
        size_t llen = eol - pos;
        if (llen && txt[pos + llen - 1] == '\r') llen--;   /* CRLF 容忍 */

        size_t klen = 0, voff = 0, vlen = 0;
        if (line_kv(txt + pos, llen, &klen, &voff, &vlen)) {
            const char *k = txt + pos;
            const char *v = txt + pos + voff;
            if (klen == 6 && memcmp(k, "app_id", 6) == 0)
                copy_field(out->app_id, sizeof(out->app_id), v, vlen);
            else if (klen == 5 && memcmp(k, "label", 5) == 0)
                copy_field(out->label, sizeof(out->label), v, vlen);
            else if (klen == 7 && memcmp(k, "version", 7) == 0)
                copy_field(out->ver, sizeof(out->ver), v, vlen);
            else if (klen == 5 && memcmp(k, "build", 5) == 0)
                copy_field(out->build, sizeof(out->build), v, vlen);
            else if (klen == 9 && memcmp(k, "store_ver", 9) == 0)
                copy_field(out->store_ver, sizeof(out->store_ver), v, vlen);
            else if (klen == 11 && memcmp(k, "store_build", 11) == 0)
                copy_field(out->store_build, sizeof(out->store_build), v, vlen);
            else if (klen == 11 && memcmp(k, "store_mtime", 11) == 0)
                copy_field(out->store_mtime, sizeof(out->store_mtime), v, vlen);
            else if (klen == 9 && memcmp(k, "store_crc", 9) == 0)
                copy_field(out->store_crc, sizeof(out->store_crc), v, vlen);
            else if (klen == 10 && memcmp(k, "store_size", 10) == 0)
                copy_field(out->store_size, sizeof(out->store_size), v, vlen);
            else if (klen == 12 && memcmp(k, "package_type", 12) == 0)
                copy_field(out->pkg_type, sizeof(out->pkg_type), v, vlen);
            else if (klen == 7 && memcmp(k, "runtime", 7) == 0)
                copy_field(out->runtime, sizeof(out->runtime), v, vlen);
            else if (klen == 5 && memcmp(k, "entry", 5) == 0)
                copy_field(out->entry, sizeof(out->entry), v, vlen);
            else if (klen == 6 && memcmp(k, "source", 6) == 0)
                copy_field(out->source, sizeof(out->source), v, vlen);
        }
        pos = (eol < len) ? eol + 1 : len;
    }
    return out->app_id[0] != 0;
}

/* ---------------- 代次守卫 ---------------- */

uint32_t am_st_cell_invalidate(am_st_cell_t *c)
{
    if (!c) return 0;
    c->gen++;
    c->valid = false;
    return c->gen;
}

bool am_st_cell_put(am_st_cell_t *c, uint32_t gen, int32_t val)
{
    if (!c) return false;
    /* 无符号回绕比较：gen 是"越新越大"的单调计数，用有符号差值判断新旧，
     * 这样跨过 2^32 也不会因为 0xFFFFFFFF → 0 而把新结果判成旧的。 */
    if (c->valid && (int32_t)(gen - c->gen) < 0) return false;   /* 旧结果，丢弃 */
    c->gen   = gen;
    c->val   = val;
    c->valid = true;
    return true;
}
