/**
 * lpk_manifest.c — manifest.json 读取 / 解析 / 校验（纯 C，可宿主单测）
 * 设计说明见 include/lpk_manifest.h
 */
#include "lpk_manifest.h"
#include "lpk1.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- 小工具 ---------------- */

static const char *skip_ws(const char *p, const char *e) {
    while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    return p;
}

/* 读一个 JSON 字符串（含转义），写入 out（超长截断但**照常消耗**输入）。
 * 成功返回 0，遇到未闭合返回 -1。 */
static int read_str(const char **pp, const char *e, char *out, size_t cap) {
    const char *p = *pp;
    if (p >= e || *p != '"') return -1;
    p++;
    size_t o = 0;
    while (p < e && *p != '"') {
        unsigned char c = (unsigned char)*p;
        if (c == '\\') {
            p++;
            if (p >= e) return -1;
            switch (*p) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case '"': c = '"';  break;
            case '\\': c = '\\'; break;
            case '/': c = '/';  break;
            default:  c = (unsigned char)*p; break;
            }
        }
        if (o + 1 < cap) { out[o] = (char)c; o++; }
        p++;
    }
    if (p >= e) return -1;          /* 未闭合 */
    out[o] = 0;
    *pp = p + 1;
    return 0;
}

/* 定长拷贝（显式截断）。用 memcpy 而不是 snprintf，
 * 是因为 -Werror=format-truncation 会把"可能截断"直接判成编译错误。 */
static void set_str(char *dst, size_t cap, const char *src) {
    if (!cap || !src) return;
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

/* 顶层 key → 字段。只认**扁平字符串**字段；数组/数字单独处理。 */
static void mf_set(lpk_mf_t *m, const char *k, const char *v) {
    if (!strcmp(k, "app_id"))            set_str(m->app_id, sizeof(m->app_id), v);
    else if (!strcmp(k, "name"))         set_str(m->name, sizeof(m->name), v);
    else if (!strcmp(k, "label"))        set_str(m->label, sizeof(m->label), v);
    else if (!strcmp(k, "description"))  set_str(m->description, sizeof(m->description), v);
    else if (!strcmp(k, "category"))     set_str(m->category, sizeof(m->category), v);
    else if (!strcmp(k, "package_type")) set_str(m->package_type, sizeof(m->package_type), v);
    else if (!strcmp(k, "runtime"))      set_str(m->runtime, sizeof(m->runtime), v);
    else if (!strcmp(k, "entry"))        set_str(m->entry, sizeof(m->entry), v);
    else if (!strcmp(k, "version"))      set_str(m->version, sizeof(m->version), v);
    else if (!strcmp(k, "build"))        set_str(m->build, sizeof(m->build), v);
    else if (!strcmp(k, "author"))       set_str(m->author, sizeof(m->author), v);
    else if (!strcmp(k, "icon"))         set_str(m->icon, sizeof(m->icon), v);
    else if (!strcmp(k, "resource_type")) set_str(m->resource_type, sizeof(m->resource_type), v);
    else if (!strcmp(k, "install_target")) set_str(m->install_target, sizeof(m->install_target), v);
    else if (!strcmp(k, "min_firmware")) set_str(m->min_firmware, sizeof(m->min_firmware), v);
    /* 其余 key（schema_version 是数字、dependencies/permissions 是数组）在扫描处处理 */
}

/* ---------------- 扁平 JSON 解析 ----------------
 * 只支持"顶层对象 + 字符串/数字/数组"这一种形态 —— manifest 本来就该长这样。
 * 数组/对象整段跳过，仅记录"是否为空"，从而能判 dependencies/permissions 有没有。
 * 这样实现的最大好处：不引入 JSON 库、不 malloc、不递归，宿主/设备同一份行为。 */
static int mf_parse(const char *js, size_t n, lpk_mf_t *m) {
    const char *p = js, *e = js + n;
    memset(m, 0, sizeof(*m));

    p = skip_ws(p, e);
    if (p >= e || *p != '{') return LPK_MF_E_JSON;
    p++;

    int depth = 1;
    while (p < e) {
        p = skip_ws(p, e);
        if (p >= e) break;
        if (*p == '}') { if (--depth <= 0) return LPK_MF_OK; p++; continue; }
        if (*p == ',') { p++; continue; }

        if (*p != '"') {                       /* 结构外的杂字符：跳过 */
            p++;
            continue;
        }

        char key[48];
        const char *key_start = p;
        if (read_str(&p, e, key, sizeof(key)) != 0) return LPK_MF_E_JSON;

        const char *q = skip_ws(p, e);
        if (q >= e || *q != ':') {             /* 不是 key，当作字符串值跳过 */
            p = key_start + 1;
            continue;
        }
        q = skip_ws(q + 1, e);

        if (q < e && *q == '"') {              /* 字符串值 */
            char val[160];
            if (read_str(&q, e, val, sizeof(val)) != 0) return LPK_MF_E_JSON;
            mf_set(m, key, val);
            p = q;
        } else if (q < e && (*q == '[' || *q == '{')) {  /* 数组 / 对象：整段跳过 */
            int d2 = 0;
            bool empty = true;
            while (q < e) {
                if (*q == '[' || *q == '{') { d2++; q++; continue; }
                if (*q == ']' || *q == '}') { d2--; q++; if (d2 == 0) break; continue; }
                if (*q == '"') {
                    char tmp[4];
                    if (read_str(&q, e, tmp, sizeof(tmp)) != 0) return LPK_MF_E_JSON;
                    empty = false;
                    continue;
                }
                if (!(*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n' || *q == ',')) empty = false;
                q++;
            }
            if (!strcmp(key, "dependencies")) m->has_dependencies = !empty;
            if (!strcmp(key, "permissions"))  m->has_permissions  = !empty;
            p = q;
        } else {                                /* 数字 / true / false / null */
            const char *s2 = q;
            while (q < e && *q != ',' && *q != '}' && *q != ']') q++;
            char num[24];
            size_t L = (size_t)(q - s2);
            if (L >= sizeof(num)) L = sizeof(num) - 1;
            memcpy(num, s2, L);
            num[L] = 0;
            if (!strcmp(key, "schema_version")) m->schema_version = atoi(num);
            p = q;
        }
    }
    return (depth == 0) ? LPK_MF_OK : LPK_MF_E_JSON;
}

/* ---------------- 规则 ---------------- */

/* 包内相对路径：禁止绝对 / .. / 反斜杠 / 控制字符 / 空段 */
static bool mf_path_ok(const char *rel) {
    if (!rel || !rel[0]) return false;
    if (rel[0] == '/') return false;
    if (strlen(rel) >= LPK_MAX_NAME) return false;
    for (const char *p = rel; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == '\\') return false;
    }
    if (strstr(rel, "..")) return false;
    if (strstr(rel, "//")) return false;
    size_t n = strlen(rel);
    if (rel[n - 1] == '/') return false;
    return true;
}

/* app_id: 小写字母开头，允许 小写/数字/-/_，点分段，至少两段，无空段 */
static bool mf_id_ok(const char *id) {
    if (!id || !id[0]) return false;
    size_t n = strlen(id);
    if (n < 3 || n >= 64) return false;
    if (!(id[0] >= 'a' && id[0] <= 'z')) return false;
    int segs = 1;
    for (size_t i = 0; i < n; i++) {
        char c = id[i];
        if (c == '.') {
            if (i == 0 || id[i - 1] == '.') return false;
            segs++;
            continue;
        }
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) return false;
    }
    if (id[n - 1] == '.') return false;
    return segs >= 2;
}

const char *lpk_mf_cat_id(const char *cat) {
    if (!cat || !cat[0]) return "software";
    if (!strcmp(cat, "软件") || !strcmp(cat, "程序") || !strcmp(cat, "工具")) return "software";
    if (!strcmp(cat, "游戏"))     return "games";
    if (!strcmp(cat, "学习"))     return "learning";
    if (!strcmp(cat, "运维"))     return "operations";
    if (!strcmp(cat, "系统"))     return "system";
    if (!strcmp(cat, "壁纸"))     return "wallpaper";
    /* 已经是稳定 ID 的直接放行 */
    if (!strcmp(cat, "software") || !strcmp(cat, "games") || !strcmp(cat, "learning") ||
        !strcmp(cat, "operations") || !strcmp(cat, "system") || !strcmp(cat, "wallpaper") ||
        !strcmp(cat, "tools")) return cat;
    return "software";
}

bool lpk_mf_launchable(const lpk_mf_t *m) {
    if (!m) return false;
    if (strcmp(m->package_type, "app") != 0) return false;
    if (!m->runtime[0] || !m->entry[0]) return false;
    return true;
}

static void why_set(char *why, size_t cap, const char *fmt, const char *a) {
    if (!why || !cap) return;
    if (a) snprintf(why, cap, fmt, a);
    else   snprintf(why, cap, "%s", fmt);
}

int lpk_mf_validate(const lpk_mf_t *m, char *why, size_t why_cap) {
    if (!m) return LPK_MF_E_ARG;

    if (m->schema_version != 1) { why_set(why, why_cap, "schema_version 必须为 1", NULL); return LPK_MF_E_FIELD; }
    if (!mf_id_ok(m->app_id))   { why_set(why, why_cap, "app_id 非法：%s", m->app_id); return LPK_MF_E_FIELD; }
    if (!m->label[0])           { why_set(why, why_cap, "label 为空", NULL); return LPK_MF_E_FIELD; }
    if (!m->version[0])         { why_set(why, why_cap, "version 为空", NULL); return LPK_MF_E_FIELD; }

    const bool is_app = !strcmp(m->package_type, "app");
    const bool is_res = !strcmp(m->package_type, "resource");
    if (!is_app && !is_res) {
        why_set(why, why_cap, "package_type 必须是 app 或 resource（实际 %s）", m->package_type);
        return LPK_MF_E_FIELD;
    }

    if (is_app) {
        if (!m->runtime[0]) { why_set(why, why_cap, "app 包必须有 runtime", NULL); return LPK_MF_E_FIELD; }
        if (strcmp(m->runtime, "lua") && strcmp(m->runtime, "native") && strcmp(m->runtime, "engine")) {
            why_set(why, why_cap, "runtime 不支持：%s", m->runtime);
            return LPK_MF_E_FIELD;
        }
        if (!m->entry[0]) { why_set(why, why_cap, "app 包必须有 entry", NULL); return LPK_MF_E_FIELD; }
        if (!mf_path_ok(m->entry)) { why_set(why, why_cap, "entry 路径非法：%s", m->entry); return LPK_MF_E_PATH; }
        if (m->icon[0] && !mf_path_ok(m->icon)) { why_set(why, why_cap, "icon 路径非法：%s", m->icon); return LPK_MF_E_PATH; }
    } else {
        if (!m->resource_type[0]) { why_set(why, why_cap, "resource 包必须有 resource_type", NULL); return LPK_MF_E_FIELD; }
        if (m->entry[0]) { why_set(why, why_cap, "resource 包不应有 entry", NULL); return LPK_MF_E_FIELD; }
    }
    return LPK_MF_OK;
}

/* ---------------- 从包里取 ---------------- */

int lpk_mf_parse(const char *json, size_t len, lpk_mf_t *out, char *why, size_t why_cap) {
    if (!json || !out) return LPK_MF_E_ARG;
    if (why && why_cap) why[0] = 0;
    if (len == 0 || len > LPK_MF_MAX) return LPK_MF_E_BIG;
    int rc = mf_parse(json, len, out);
    if (rc != LPK_MF_OK) { why_set(why, why_cap, "manifest.json 不是合法 JSON 对象", NULL); return rc; }
    return lpk_mf_validate(out, why, why_cap);
}

int lpk_mf_load(const char *lpk_path, lpk_mf_t *out, char *why, size_t why_cap) {
    if (!lpk_path || !out) return LPK_MF_E_ARG;
    if (why && why_cap) why[0] = 0;
    memset(out, 0, sizeof(*out));

    lpk_t h;
    memset(&h, 0, sizeof(h));

    /* ① 先只读头拿条数（entries 传 NULL：不分配表） */
    int rc = lpk_open(&h, lpk_path, NULL, 0);
    if (rc != LPK_OK) {
        why_set(why, why_cap, "不是有效的 LPK：%s", lpk_path);
        return LPK_MF_E_OPEN;
    }
    uint32_t cnt = h.count;
    lpk_close(&h);
    if (cnt == 0 || cnt > LPK_MAX_ENTRIES) {
        why_set(why, why_cap, "包内条目数异常：%s", NULL);
        return LPK_MF_E_OPEN;
    }

    /* ② 按真实条数分配索引表（109 条 ≈ 21 KB，装完即 free） */
    lpk_entry_t *en = (lpk_entry_t *)calloc(cnt, sizeof(lpk_entry_t));
    if (!en) return LPK_MF_E_ARG;

    int ret = LPK_MF_OK;
    char *js = NULL;

    rc = lpk_open(&h, lpk_path, en, (int)cnt);
    if (rc != LPK_OK) { ret = LPK_MF_E_OPEN; goto done; }

    int idx = lpk_find_ci(&h, en, LPK_MF_NAME);
    if (idx < 0) { why_set(why, why_cap, "包里没有 manifest.json", NULL); ret = LPK_MF_E_NOENT; goto done; }
    if (en[idx].size == 0 || en[idx].size > LPK_MF_MAX) {
        why_set(why, why_cap, "manifest.json 大小异常", NULL);
        ret = LPK_MF_E_BIG; goto done;
    }

    js = (char *)malloc(en[idx].size + 1);
    if (!js) { ret = LPK_MF_E_ARG; goto done; }
    if (lpk_read(&h, &en[idx], 0, js, en[idx].size) != LPK_OK) {
        why_set(why, why_cap, "manifest.json 读取失败", NULL);
        ret = LPK_MF_E_OPEN; goto done;
    }
    js[en[idx].size] = 0;

    ret = mf_parse(js, en[idx].size, out);
    if (ret != LPK_MF_OK) { why_set(why, why_cap, "manifest.json 不是合法 JSON 对象", NULL); goto done; }

    ret = lpk_mf_validate(out, why, why_cap);
    if (ret != LPK_MF_OK) goto done;

    /* ③ 声明的 entry / icon 必须**真的在包里** —— 这是"装完才发现跑不起来"的唯一防线 */
    if (lpk_mf_launchable(out)) {
        if (lpk_find_ci(&h, en, out->entry) < 0) {
            why_set(why, why_cap, "manifest 声明的 entry 不在包里：%s", out->entry);
            ret = LPK_MF_E_NOENTRY; goto done;
        }
    }
    if (out->icon[0]) {
        int ii = lpk_find_ci(&h, en, out->icon);
        if (ii < 0) {
            why_set(why, why_cap, "manifest 声明的 icon 不在包里：%s", out->icon);
            ret = LPK_MF_E_NOICON; goto done;
        }
        if (en[ii].size != LPK_MF_ICON_SZ) {
            why_set(why, why_cap, "icon 长度必须为 512（64×64 1bpp）", NULL);
            ret = LPK_MF_E_ICON; goto done;
        }
    }

done:
    if (js) free(js);
    free(en);
    lpk_close(&h);
    return ret;
}
