/**
 * am_lpkg.c — 第三方 LPK 应用包的安装 / 卸载 / 注册（商店范围）
 * 设计见 include/am_lpkg.h 与 docs/architecture/app_system.md
 */
#include "am_lpkg.h"

#include "am_store_state.h"   /* registry 解析 + 版本比较：与商店共用同一份实现 */
#include "str_trunc.h"       /* 短字段的有意截断拷贝(消除 -Wformat-truncation 噪音) */
#include "lpk1.h"
#include "lpk_manifest.h"
#include "ui_common.h"

#include "esp_log.h"
#include "esp_heap_caps.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "AMLPKG";

/* 安装根目录。生产恒为 /sdcard/apps —— 不传 LPKG_ROOT 时编译结果与以前完全一致。
 * 为什么留这个开关：安装/更新/回滚/掉电自愈这些"改磁盘状态"的行为，只有**真跑**才
 * 能证明；宿主机测试用 -DLPKG_ROOT='"/tmp/xxx"' 把根指到临时目录，就能用同一份生产代码
 * 跑 tests/installer/ 里的安装、失败回滚、重启恢复用例，而不是靠代码走查下结论。 */
#ifndef LPKG_ROOT
#define LPKG_ROOT      "/sdcard/apps"
#endif
#define LPKG_INSTALLED LPKG_ROOT "/installed"
#define LPKG_DATA      LPKG_ROOT "/data"
#define LPKG_CACHE     LPKG_ROOT "/cache"
#define LPKG_STAGE     LPKG_ROOT "/staging"
#define LPKG_REG       LPKG_ROOT "/registry"
#define LPKG_QUAR      LPKG_ROOT "/quarantine"
/* ⚠ 资源根**不**跟着 LPKG_ROOT 走：生产的资源包在 /sdcard/resources，
 *   与 /sdcard/apps 是两棵独立的树（字体还在 /sdcard/fonts，见 res_root_of）。
 *   改成 LPKG_ROOT "/resources" 会让现网资源包路径整体搬家 —— 不能动。 */
#define LPKG_RES       "/sdcard/resources"

#define LPKG_PATH      192
#define LPKG_IO_CHUNK  4096
#define LPKG_REG_TXT_MAX 512      /* registry 记录很小(几百字节), 读进栈上足够 */

/* 前向声明: 读取实现放在文件末尾(与 am_lpkg_info 同处), 安装收尾要回读校验 */
static bool read_registry(const char *app_id, char *buf, size_t cap, size_t *out_len);

/* ---------------- 注册表（静态，用于启动链路反查） ---------------- */
typedef struct {
    bool      used;
    char      app_id[56];
    char      label[48];
    char      ver[16];
    char      entry[LPKG_PATH];      /* 绝对路径：/sdcard/apps/installed/<id>/<entry> */
    bool      need_wifi;
    const char *cat;
    os_app_t  app;
} lpk_app_t;

static lpk_app_t s_lp[AM_LPKG_MAX];
static int       s_lp_n;

/* ---------------- 路径小工具（不用 snprintf，避免 format-truncation） ---------------- */

static bool cat_s(char *out, size_t cap, const char *s) {
    if (!s) return true;
    size_t o = strlen(out), n = strlen(s);
    if (o + n + 1 > cap) return false;
    memcpy(out + o, s, n + 1);
    return true;
}

/* 拼路径：各段之间补 '/'（段本身不带首尾斜杠）。 */
static bool pj(char *out, size_t cap, const char *a, const char *b, const char *c, const char *d) {
    if (!out || !cap) return false;
    out[0] = 0;
    const char *parts[4] = { a, b, c, d };
    for (int i = 0; i < 4; i++) {
        const char *s = parts[i];
        if (!s || !s[0]) continue;
        size_t o = strlen(out);
        if (o && out[o - 1] != '/') { if (!cat_s(out, cap, "/")) return false; }
        if (!cat_s(out, cap, s)) return false;
    }
    return true;
}

static bool exists(const char *p) {
    struct stat st;
    return p && stat(p, &st) == 0;
}

/* 取文件名主干：**只去掉最后一个扩展名**。
 * "packages/com.lintos.recorder.lpk" → "com.lintos.recorder"
 * ⚠ 历史教训：早先的实现遇到第一个 '.' 就停 ⇒ 得到 "com" ⇒ 身份全错。 */
static void stem_of(const char *path, char *buf, size_t cap)
{
    if (!buf || !cap) return;
    buf[0] = 0;
    if (!path || !path[0]) return;
    const char *b = strrchr(path, '/');
    b = b ? b + 1 : path;
    const char *dot = strrchr(b, '.');
    size_t n = (dot && dot != b) ? (size_t)(dot - b) : strlen(b);
    if (n >= cap) n = cap - 1;
    memcpy(buf, b, n);
    buf[n] = 0;
}

/* 逐级创建目录 */
static bool mkdirs(const char *path) {
    char buf[LPKG_PATH];
    size_t n = path ? strlen(path) : 0;
    if (n == 0 || n >= sizeof(buf)) return false;
    memcpy(buf, path, n + 1);
    for (char *p = buf + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        mkdir(buf, 0777);
        *p = '/';
    }
    mkdir(buf, 0777);
    return exists(buf);
}

/* 递归删除（只用于 staging / 卸载，绝不用于 /sdcard 根） */
static void rm_rf(const char *path) {
    if (!path || !path[0]) return;
    struct stat st;
    if (stat(path, &st) != 0) return;
    if (!S_ISDIR(st.st_mode)) { unlink(path); return; }

    DIR *d = opendir(path);
    if (d) {
        struct dirent *e;
        char child[LPKG_PATH];
        while ((e = readdir(d)) != NULL) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            if (!pj(child, sizeof(child), path, e->d_name, NULL, NULL)) continue;
            rm_rf(child);
        }
        closedir(d);
    }
    rmdir(path);
}

static void setwhy(char *why, size_t cap, const char *msg, const char *arg) {
    if (!why || !cap) return;
    if (arg) snprintf(why, cap, "%s: %s", msg, arg);
    else     snprintf(why, cap, "%s", msg);
}

/* ---------------- 解包一个条目 ---------------- */

static bool unpack_entry(lpk_t *h, const lpk_entry_t *e, const char *stage,
                         unsigned char *buf, size_t bufsz, unsigned long *bytes_out) {
    char dst[LPKG_PATH];
    if (!pj(dst, sizeof(dst), stage, e->name, NULL, NULL)) return false;

    /* 建父目录 */
    char parent[LPKG_PATH];
    size_t dn = strlen(dst);
    if (dn >= sizeof(parent)) return false;
    memcpy(parent, dst, dn + 1);
    char *slash = strrchr(parent, '/');
    if (slash) { *slash = 0; if (!mkdirs(parent)) return false; }

    FILE *f = fopen(dst, "wb");
    if (!f) { ESP_LOGE(TAG, "无法写入 %s", dst); return false; }

    uint32_t off = 0;
    bool ok = true;
    while (off < e->size) {
        uint32_t n = e->size - off;
        if (n > bufsz) n = (uint32_t)bufsz;
        if (lpk_read(h, e, off, buf, n) != LPK_OK) { ok = false; break; }
        if (fwrite(buf, 1, n, f) != n)             { ok = false; break; }
        off += n;
    }
    fclose(f);
    if (!ok) { unlink(dst); return false; }
    *bytes_out += e->size;
    return true;
}

/* ---------------- 安装 ---------------- */

/* 写安装记录（registry/<app_id>.txt）：先写 .tmp 再换名，写完回读校验。
 * 返回 false = 记录没落地 —— 调用方必须把刚切好的目录回滚掉，
 * 否则会出现"文件是新版、记录还是旧版（或不存在）"的半装状态：
 * 商店按记录判版本 ⇒ 永远提示更新；按记录判安装 ⇒ 新版反而显示"下载"。 */
static bool write_registry(const lpk_mf_t *mf, const char *src_path,
                           const char *src_ver, const char *src_build,
                           uint32_t src_mtime, uint32_t src_crc, uint32_t src_size)
{
    char rec[LPKG_PATH], tmp[LPKG_PATH + 8];
    if (!pj(rec, sizeof(rec), LPKG_REG, mf->app_id, NULL, NULL)) return false;
    if (!cat_s(rec, sizeof(rec), ".txt")) return false;
    snprintf(tmp, sizeof(tmp), "%s.tmp", rec);

    /* version/build   = 包内 manifest 自称的（应用自己的版本）
     * store_ver/build = 商店索引声明的（这次是从哪一版装进来的）
     * 两份不一致 ⇒ 服务器索引没跟着重打包，am_lpkg_info() 会打 WARN。
     * 更新判定必须用 store_*：否则"索引抬了版本但包没重打"会让用户永远收到更新。 */
    const char *sv = (src_ver   && src_ver[0])   ? src_ver   : mf->version;
    const char *sb = (src_build && src_build[0]) ? src_build : mf->build;

    FILE *f = fopen(tmp, "wb");
    if (!f) { ESP_LOGE(TAG, "写安装记录失败(打不开): %s", tmp); return false; }
    int n = fprintf(f, "app_id=%s\nlabel=%s\nversion=%s\nbuild=%s\n"
                       "store_ver=%s\nstore_build=%s\n"
                       "store_mtime=%lu\nstore_crc=%08lx\nstore_size=%lu\n"
                       "package_type=%s\nruntime=%s\nentry=%s\nsource=%s\n",
                    mf->app_id, mf->label, mf->version, mf->build,
                    sv, sb,
                    (unsigned long)src_mtime, (unsigned long)src_crc, (unsigned long)src_size,
                    mf->package_type, mf->runtime[0] ? mf->runtime : "-",
                    mf->entry[0] ? mf->entry : "-", src_path);
    bool wrote = (n > 0);
    if (fclose(f) != 0) wrote = false;          /* 收尾失败 = 没落盘 */
    if (!wrote) { unlink(tmp); ESP_LOGE(TAG, "写安装记录失败(写入不完整): %s", tmp); return false; }

    /* 换名分三步，保证任何一步失败都能回到"旧记录仍在"：
     *   ① 旧记录 → .bak（先不删）  ② .tmp → 正式名  ③ 回读校验过了才删 .bak
     * 为什么不能直接 remove(rec) 再 rename：那样 rename 一失败记录就**没了**，
     * 上一版信息彻底丢掉（只能靠 installed/ 目录兜底判"已安装、版本未知"）。 */
    char bak[LPKG_PATH + 8];
    snprintf(bak, sizeof(bak), "%s.bak", rec);
    const bool had_old = exists(rec);
    if (had_old) { remove(bak); if (rename(rec, bak) != 0) { unlink(tmp); return false; } }
    if (rename(tmp, rec) != 0) {
        unlink(tmp);
        if (had_old) rename(bak, rec);          /* 恢复旧记录 */
        ESP_LOGE(TAG, "安装记录替换失败: %s", rec);
        return false;
    }

    /* 回读校验：记录必须能被解析出 app_id，且包内版本与包一致。
     * 不校验的话，一次 SPIFFS 满导致的半截文件会被下一轮当成"已安装旧版"。
     * 校验不过 ⇒ 连新记录一起撤掉，恢复到 .bak（否则文件回滚了、记录却是新的）。 */
    char txt[LPKG_REG_TXT_MAX];
    size_t tlen = 0;
    am_st_reg_t r;
    bool ok = read_registry(mf->app_id, txt, sizeof(txt), &tlen) &&
              am_st_reg_parse(txt, tlen, &r) &&
              strcmp(r.app_id, mf->app_id) == 0 &&
              !(r.ver[0] && mf->version[0] && strcmp(r.ver, mf->version) != 0);
    if (!ok) {
        ESP_LOGE(TAG, "安装记录回读校验失败: %s", rec);
        unlink(rec);
        if (had_old) rename(bak, rec);
        return false;
    }
    if (had_old) remove(bak);
    return true;
}

static const char *res_root_of(const char *t) {
    if (!t || !t[0]) return NULL;
    /* ⚠ 字体保持 /sdcard/fonts：现有字体加载器只扫这个目录。
     *   统一目录不能以"改坏加载器"为代价 —— 这条已在 app_system.md 里注明。 */
    if (!strcmp(t, "font"))     return "/sdcard/fonts";
    if (!strcmp(t, "theme"))    return LPKG_RES "/themes";
    if (!strcmp(t, "rom"))      return LPKG_RES "/roms";
    if (!strcmp(t, "gamedata")) return LPKG_RES "/game";
    if (!strcmp(t, "shared"))   return LPKG_RES "/shared";
    return NULL;
}

esp_err_t am_lpkg_install(const char *lpk_path, char *why, size_t why_cap) {
    return am_lpkg_install_src2(lpk_path, NULL, NULL, 0, 0, 0, why, why_cap);
}

esp_err_t am_lpkg_install_src(const char *lpk_path, const char *src_ver,
                              const char *src_build, char *why, size_t why_cap) {
    return am_lpkg_install_src2(lpk_path, src_ver, src_build, 0, 0, 0, why, why_cap);
}

esp_err_t am_lpkg_install_src2(const char *lpk_path, const char *src_ver,
                               const char *src_build,
                               uint32_t src_mtime, uint32_t src_crc, uint32_t src_size,
                               char *why, size_t why_cap) {
    if (!lpk_path || !lpk_path[0]) return ESP_ERR_INVALID_ARG;
    if (why && why_cap) why[0] = 0;

    lpk_mf_t mf;
    int mrc = lpk_mf_load(lpk_path, &mf, why, why_cap);
    if (mrc != LPK_MF_OK) {
        ESP_LOGE(TAG, "manifest 校验失败(%d): %s", mrc, why ? why : "");
        return ESP_ERR_INVALID_STATE;
    }

    const bool is_app = (strcmp(mf.package_type, "app") == 0);
    if (is_app) {
        if (!lpk_mf_launchable(&mf)) { setwhy(why, why_cap, "包不可启动", NULL); return ESP_ERR_INVALID_STATE; }
        if (strcmp(mf.runtime, "lua") != 0) {
            setwhy(why, why_cap, "暂不支持该 runtime（当前仅 lua）", mf.runtime);
            return ESP_ERR_NOT_SUPPORTED;
        }
    }

    /* 目标根：应用 → installed/；资源 → resources/<type>/ */
    const char *root = is_app ? LPKG_INSTALLED : res_root_of(mf.resource_type);
    if (!root) { setwhy(why, why_cap, "未知 resource_type", mf.resource_type); return ESP_ERR_INVALID_ARG; }

    char final[LPKG_PATH], stage[LPKG_PATH], old[LPKG_PATH];
    if (!pj(final, sizeof(final), root, mf.app_id, NULL, NULL) ||
        !pj(stage, sizeof(stage), LPKG_STAGE, mf.app_id, NULL, NULL) ||
        !pj(old,   sizeof(old),   LPKG_STAGE, mf.app_id, NULL, NULL) ||
        !cat_s(stage, sizeof(stage), ".new") || !cat_s(old, sizeof(old), ".old")) {
        setwhy(why, why_cap, "路径过长", NULL);
        return ESP_ERR_INVALID_ARG;
    }

    mkdirs(LPKG_STAGE);
    mkdirs(LPKG_INSTALLED);
    mkdirs(LPKG_REG);

    /* ★ 2026-11-01 掉电自愈（更新链路第一次做"目录让位"，必须补这条）：
     *   提交顺序是 installed/<id> → staging/<id>.old → staging/<id>.new → installed/<id>。
     *   若恰好在"旧目录已让位、新目录还没就位"之间掉电，正式目录就是**不存在**的，
     *   而 .old 里躺着完整旧版本 —— 用户看到的就是"应用突然没了"。
     *   rename 是原子的 ⇒ 只可能出现 ①final 在 ②old 在 ③两者都在，这里补 ②。
     *   放在建完目录之后、动 stage 之前，且只在 final 缺失时才动手。 */
    if (!exists(final) && exists(old)) {
        if (rename(old, final) == 0)
            ESP_LOGW(TAG, "掉电自愈: 上次更新中断, 已恢复旧版本目录 %s", final);
        else
            ESP_LOGE(TAG, "掉电自愈失败: 无法把 %s 恢复为 %s", old, final);
    }

    rm_rf(stage);
    if (!mkdirs(stage)) { setwhy(why, why_cap, "建 staging 失败", stage); return ESP_FAIL; }

    /* 打开包（按真实条数分配索引表） */
    lpk_t h; memset(&h, 0, sizeof(h));
    if (lpk_open(&h, lpk_path, NULL, 0) != LPK_OK) {
        setwhy(why, why_cap, "不是有效 LPK", lpk_path); rm_rf(stage); return ESP_ERR_INVALID_STATE;
    }
    uint32_t cnt = h.count;
    lpk_close(&h);
    if (cnt == 0 || cnt > LPK_MAX_ENTRIES) { setwhy(why, why_cap, "条目数异常", NULL); rm_rf(stage); return ESP_ERR_INVALID_STATE; }

    lpk_entry_t *en = (lpk_entry_t *)calloc(cnt, sizeof(lpk_entry_t));
    unsigned char *buf = (unsigned char *)malloc(LPKG_IO_CHUNK);
    if (!en || !buf) { free(en); free(buf); rm_rf(stage); return ESP_ERR_NO_MEM; }

    esp_err_t ret = ESP_OK;
    if (lpk_open(&h, lpk_path, en, (int)cnt) != LPK_OK) {
        setwhy(why, why_cap, "索引读取失败", NULL); ret = ESP_ERR_INVALID_STATE; goto out;
    }

    unsigned long expect = 0, got = 0;
    for (uint32_t i = 0; i < cnt; i++) {
        expect += en[i].size;
        if (!unpack_entry(&h, &en[i], stage, buf, LPKG_IO_CHUNK, &got)) {
            setwhy(why, why_cap, "解包失败", en[i].name);
            ret = ESP_FAIL; goto out;
        }
    }
    if (got != expect) {                       /* 写盘字节数必须与索引完全一致 */
        setwhy(why, why_cap, "解包字节数不符", NULL);
        ret = ESP_ERR_INVALID_SIZE; goto out;
    }

    /* 原子切换：旧版先让路，新目录到位后才删旧的。
     * ★ 2026-11-01 「更新」的两阶段提交：
     *   ① 旧目录 → staging/<id>.old（**先不删**）
     *   ② staging/<id>.new → installed/<id>/（新版本就位）
     *   ③ 写 registry（写 .tmp + 回读校验）
     *   ④ ③ 成功才 rm_rf(.old)；③ 失败 ⇒ 删新目录 + 把 .old 改回来 ⇒ 回到旧版本
     *   这样"下载失败/校验失败/记录写不进去"都不会留下半装状态：要么是旧版本
     *   （商店显示「更新」，可以再试），要么是新版本（显示「卸载」），没有第三种。
     *   用户数据 data/<id>/ 全程不碰（data_policy=preserve）。 */
    bool had_old = exists(final);
    if (had_old) { rm_rf(old); if (rename(final, old) != 0) {
        setwhy(why, why_cap, "旧版本让位失败", final); ret = ESP_FAIL; goto out; } }
    if (rename(stage, final) != 0) {
        if (had_old) rename(old, final);        /* 回滚 */
        setwhy(why, why_cap, "切换安装目录失败", final);
        ret = ESP_FAIL; goto out;
    }

    if (is_app) {
        char d[LPKG_PATH], c[LPKG_PATH];
        if (pj(d, sizeof(d), LPKG_DATA,  mf.app_id, NULL, NULL)) mkdirs(d);   /* 升级不清数据 */
        if (pj(c, sizeof(c), LPKG_CACHE, mf.app_id, NULL, NULL)) mkdirs(c);
    }

    /* 安装记录：人可读，商店判"已安装/版本"、卸载、诊断都用它 */
    if (!write_registry(&mf, lpk_path, src_ver, src_build, src_mtime, src_crc, src_size)) {
        /* 记录没落地 ⇒ 整个切换回滚，绝不留"文件是新版、记录是旧版" */
        rm_rf(final);
        if (had_old) rename(old, final);
        setwhy(why, why_cap, "安装记录写入失败(已回滚)", mf.app_id);
        ret = ESP_FAIL; goto out;
    }
    if (had_old) rm_rf(old);          /* 到这里才真正丢掉旧版本 */
    if (is_app) am_lpkg_scan();       /* 注册到桌面/应用中心（按 id 覆盖，幂等） */

    ESP_LOGW(TAG, "%s %s（%s %s，%u 条，%lu B）→ %s",
             had_old ? "已更新" : "已安装",
             mf.app_id, mf.package_type, mf.version, (unsigned)cnt, got, final);

out:
    free(en);
    free(buf);
    lpk_close(&h);
    if (ret != ESP_OK) rm_rf(stage);
    return ret;
}

/* ---------------- 卸载 ---------------- */

esp_err_t am_lpkg_uninstall(const char *app_id, bool keep_data, char *why, size_t why_cap) {
    if (!app_id || !app_id[0]) return ESP_ERR_INVALID_ARG;
    if (why && why_cap) why[0] = 0;

    char p[LPKG_PATH];
    bool any = false;

    /* 应用目录 */
    if (pj(p, sizeof(p), LPKG_INSTALLED, app_id, NULL, NULL) && exists(p)) { rm_rf(p); any = true; }
    /* 资源目录（逐个 type 试；只删该 app_id 自己的子目录） */
    static const char *const RT[] = { "fonts", "themes", "roms", "game", "shared" };
    for (size_t i = 0; i < sizeof(RT) / sizeof(RT[0]); i++) {
        char r[LPKG_PATH];
        if (pj(r, sizeof(r), LPKG_RES, RT[i], app_id, NULL) && exists(r)) { rm_rf(r); any = true; }
    }
    /* 缓存永远删；数据按 keep_data */
    if (pj(p, sizeof(p), LPKG_CACHE, app_id, NULL, NULL) && exists(p)) rm_rf(p);
    if (!keep_data && pj(p, sizeof(p), LPKG_DATA, app_id, NULL, NULL) && exists(p)) rm_rf(p);

    /* 记录 */
    bool had_reg = false;
    if (pj(p, sizeof(p), LPKG_REG, app_id, NULL, NULL) && cat_s(p, sizeof(p), ".txt")) {
        had_reg = exists(p);
        unlink(p);
        if (had_reg) any = true;      /* 只剩记录(目录已被手删)也算"卸载掉了东西" */
        /* 上一次写记录时若在"删 .bak"之前掉电，会留下 <id>.txt.bak —— 一并清掉，
         * 否则卸载后卡上永远躺着一条旧版本记录（虽然不再被读取，但会误导排查）。 */
        char bak[LPKG_PATH + 8];
        snprintf(bak, sizeof(bak), "%s.bak", p);
        unlink(bak);
    }

    if (!any) { setwhy(why, why_cap, "未安装", app_id); return ESP_ERR_NOT_FOUND; }

    /* ★★★ 2026-11-01 linit: 卸载 = 这个应用在设备上**彻底归零**。
     *   桌面图标的两个持久化列表 (mainmenu=用户加过, mmhide=用户删过) 都要清掉。
     *   不清的后果：① 删过的应用重装后仍被 mmhide 挡着，用户"加了没反应"；
     *              ② 加过的应用重装后自动回到桌面，用户"没加它又出现了"。
     *   清零后语义唯一：装完默认不在桌面，要上桌面自己加。 */
    os_app_set_mainmenu(app_id, false);
    os_app_set_hidden(app_id, false);

    am_lpkg_scan();
    ESP_LOGW(TAG, "已卸载 %s（数据%s）", app_id, keep_data ? "保留" : "已删除");
    return ESP_OK;
}

/* ---------------- 扫描 + 注册 ---------------- */

static const char *cat_cn(const char *id) {
    if (!id) return "软件";
    if (!strcmp(id, "games"))      return "游戏";
    if (!strcmp(id, "learning"))   return "学习";
    if (!strcmp(id, "operations")) return "运维";
    if (!strcmp(id, "system"))     return "系统";
    if (!strcmp(id, "wallpaper"))  return "壁纸";
    return "软件";
}

static bool read_small(const char *path, char *buf, size_t cap, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = 0;
    if (out_len) *out_len = n;
    return true;
}

int am_lpkg_scan(void) {
    memset(s_lp, 0, sizeof(s_lp));
    s_lp_n = 0;

    DIR *d = opendir(LPKG_INSTALLED);
    if (!d) return 0;

    char *js = (char *)malloc(LPK_MF_MAX + 1);
    if (!js) { closedir(d); return 0; }

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        if (s_lp_n >= AM_LPKG_MAX) break;

        char dir[LPKG_PATH], mfp[LPKG_PATH];
        if (!pj(dir, sizeof(dir), LPKG_INSTALLED, e->d_name, NULL, NULL)) continue;
        if (!pj(mfp, sizeof(mfp), dir, "manifest.json", NULL, NULL)) continue;

        size_t len = 0;
        if (!read_small(mfp, js, LPK_MF_MAX + 1, &len) || len == 0) continue;

        lpk_mf_t mf;
        char why[96];
        if (lpk_mf_parse(js, len, &mf, why, sizeof(why)) != LPK_MF_OK) {
            ESP_LOGW(TAG, "%s 的 manifest 无效: %s", e->d_name, why);
            continue;
        }
        if (!lpk_mf_launchable(&mf)) continue;              /* 资源包不注册 */
        if (strcmp(mf.runtime, "lua") != 0) continue;       /* 暂只支持 lua 启动 */

        lpk_app_t *it = &s_lp[s_lp_n];
        /* 用 str_trunc 而不是 snprintf("%s")：app_id 字段 56B < manifest 的 64B 上限，
         * 这是**设计上的截断**（app_id 超 56 字符的包本来就不收），
         * 写清楚意图，别让 -Wformat-truncation 的噪音淹掉真正的告警。 */
        str_trunc(it->app_id, sizeof(it->app_id), mf.app_id);
        str_trunc(it->label,  sizeof(it->label),  mf.label);
        str_trunc(it->ver,    sizeof(it->ver),    mf.version);
        if (!pj(it->entry, sizeof(it->entry), dir, mf.entry, NULL, NULL)) continue;
        if (!exists(it->entry)) { ESP_LOGW(TAG, "%s 入口不存在: %s", mf.app_id, it->entry); continue; }

        it->need_wifi = mf.has_permissions && strstr(js, "network") != NULL;
        it->cat = cat_cn(lpk_mf_cat_id(mf.category));

        /* 图标：包内 icon.bin（512 B）→ 注册为外来位图；没有就用分类默认图 */
        int icon_idx = ICON_APP_MGR;
        char ip[LPKG_PATH];
        if (mf.icon[0] && pj(ip, sizeof(ip), dir, mf.icon, NULL, NULL)) {
            uint8_t bmp[LPK_MF_ICON_SZ];
            size_t ilen = 0;
            FILE *f = fopen(ip, "rb");
            if (f) {
                ilen = fread(bmp, 1, sizeof(bmp), f);
                fclose(f);
            }
            if (ilen == sizeof(bmp)) {
                int ex = ui_icon_register_extra(bmp);
                if (ex >= 0) icon_idx = ex;
            }
        }

        it->app.id        = it->app_id;
        it->app.label     = it->label;
        it->app.icon_idx  = icon_idx;
        it->app.page      = OS_PAGE_LUA;
        /* ★★★ 2026-11-01 linit 定稿：**商店装的应用不再自动往桌面上塞图标**。
         *   原来这里是 hidden=false ⇒ 装完就出现在主菜单/桌面，用户想清掉只能长按
         *   3 秒删除（写 NVS mmhide），而重新安装/重新扫描一遍又像是"又回来了"，
         *   观感就是"删不掉"。
         *   现在：装完**只进应用中心**，桌面图标由用户显式添加（os_app_set_mainmenu）。
         *   卸载时再把两个列表都清掉 ⇒ 重装后依然默认不在桌面，要上桌面得自己加。 */
        it->app.hidden    = true;                  /* 默认不在桌面：显式添加才显示 */
        it->app.touch_only= false;
        it->app.cat       = it->cat;
        it->app.reveal    = false;
        it->app.requires  = it->entry;             /* 入口没了 → 自动隐藏（拔卡/卸载） */

        it->used = true;
        s_lp_n++;
    }
    closedir(d);
    free(js);

    /* ★ 2026-11-03: **注册表写入点统一到 am_app_core**。
     *   原来这里自己 os_app_register(&s_lp[i].app)，而应用中心又由商店另建一套列表
     *   ⇒ 同一个 LPK 应用在桌面/应用中心/商店三处各有一条记录、各判一次状态。
     *   现在 am_lpkg_scan 只负责"把 registry 读成内存表"（供核心查询），
     *   注册给 os_app 的动作由 am_core_tick() 统一完成（同 id 覆盖，不会重复）。 */
    if (s_lp_n) ESP_LOGI(TAG, "已安装第三方应用: %d 条（注册由统一应用核心完成）", s_lp_n);
    return s_lp_n;
}

/* ---------------- 启动链路反查 ---------------- */

const char *am_lpkg_entry_of(const os_app_t *a) {
    if (!a) return NULL;
    for (int i = 0; i < s_lp_n; i++)
        if (&s_lp[i].app == a) return s_lp[i].entry;
    return NULL;
}

bool am_lpkg_entry_path(const char *app_id, char *out, size_t cap)
{
    if (!app_id || !app_id[0] || !out || !cap) return false;
    out[0] = 0;
    char txt[LPKG_REG_TXT_MAX];
    size_t tlen = 0;
    am_st_reg_t r;
    if (!read_registry(app_id, txt, sizeof(txt), &tlen)) return false;
    if (!am_st_reg_parse(txt, tlen, &r)) return false;
    if (!r.entry[0] || strcmp(r.entry, "-") == 0) return false;
    if (!pj(out, cap, LPKG_INSTALLED, app_id, r.entry, NULL)) return false;
    return exists(out);
}

bool am_lpkg_need_wifi_of(const os_app_t *a) {
    if (!a) return false;
    for (int i = 0; i < s_lp_n; i++)
        if (&s_lp[i].app == a) return s_lp[i].need_wifi;
    return false;
}

/* ---------------- 读取已安装状态（唯一判据实现） ---------------- */

/* 读 registry 文本；不存在/读失败返回 false。 */
static bool read_registry(const char *app_id, char *buf, size_t cap, size_t *out_len)
{
    char p[LPKG_PATH];
    if (!pj(p, sizeof(p), LPKG_REG, app_id, NULL, NULL)) return false;
    if (!cat_s(p, sizeof(p), ".txt")) return false;
    FILE *f = fopen(p, "rb");
    if (!f) return false;
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = 0;
    if (out_len) *out_len = n;
    return true;
}

bool am_lpkg_info(const char *app_id, am_lpkg_info_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!app_id || !app_id[0]) return false;
    snprintf(out->app_id, sizeof(out->app_id), "%s", app_id);

    /* ① registry —— 正常安装路径一定写它 */
    char txt[LPKG_REG_TXT_MAX];
    size_t tlen = 0;
    if (read_registry(app_id, txt, sizeof(txt), &tlen)) {
        am_st_reg_t r;
        if (am_st_reg_parse(txt, tlen, &r)) {
            out->registry_ok = true;
            /* 更新判定用**商店口径**（store_ver/store_build），没有这两行（早期版本装的）
             * 才退回包内 version/build。理由见 am_store_state.h 的格式说明。 */
            snprintf(out->mf_ver,   sizeof(out->mf_ver),   "%s", r.ver);
            snprintf(out->mf_build, sizeof(out->mf_build), "%s", r.build);
            snprintf(out->ver,   sizeof(out->ver),   "%s", r.store_ver[0]   ? r.store_ver   : r.ver);
            snprintf(out->build, sizeof(out->build), "%s", r.store_build[0] ? r.store_build : r.build);
            /* ★ 2026-11-02: 安装时服务器文件的 mtime/crc/size —— 更新判定的唯一依据。
             *   早期记录没有这三行 ⇒ 全 0，调用方退回 size+CRC32 核对（见 am_st_decide_meta）。 */
            out->rec_mtime = (uint32_t)strtoul(r.store_mtime, NULL, 10);
            out->rec_crc   = (uint32_t)strtoul(r.store_crc,   NULL, 16);
            out->rec_size  = (uint32_t)strtoul(r.store_size,  NULL, 10);
            /* 索引版本 ≠ 包内版本 ⇒ 服务器只抬了索引没重打包。明确报出来，
             * 不静默糊过去（否则以后每次发版都会踩同一个坑）。 */
            if ((r.store_ver[0] && r.ver[0] && strcmp(r.store_ver, r.ver) != 0) ||
                (r.store_build[0] && r.build[0] && strcmp(r.store_build, r.build) != 0)) {
                out->meta_mismatch = true;
                ESP_LOGW(TAG, "版本口径不一致 %s: 包内 %s/%s vs 商店 %s/%s（按商店口径判定；"
                              "服务器索引建议重新生成）",
                         app_id, r.ver, r.build, r.store_ver, r.store_build);
            }
            /* 记录里的 app_id 与文件名不一致 ⇒ 记录坏了（改名/手改/写一半掉电）。
             * 明确报出来，不静默当成"未安装" —— 那会让一个真装好的应用变回"下载"。 */
            if (strcmp(r.app_id, app_id) != 0)
                ESP_LOGW(TAG, "registry 记录不自洽: 文件名=%s 内容=%s（按已安装处理）",
                         app_id, r.app_id);
        } else {
            ESP_LOGW(TAG, "registry 记录损坏(解析不出 app_id): %s.txt", app_id);
        }
    }

    /* ② installed/<app_id>/ 目录兜底 —— registry 被删/写一半掉电时，
     *    只看文件树也要判"已安装"；版本从包内 manifest 补读。 */
    char dir[LPKG_PATH], mfp[LPKG_PATH];
    if (pj(dir, sizeof(dir), LPKG_INSTALLED, app_id, NULL, NULL) && exists(dir)) {
        out->dir_ok = true;
        if (!out->registry_ok) {
            if (pj(mfp, sizeof(mfp), dir, "manifest.json", NULL, NULL)) {
                /* ★ 2026-11-01 修复（串口铁证）：
                 *   原来是 char js[LPK_MF_MAX + 1] = 8193 B 的**栈**缓冲。
                 *   商店列表逐行渲染 → am_store_state_of → am_row_state →
                 *   am_row_calc → am_row_local_eval → am_lpkg_info()，
                 *   一进函数就要预留 8KB 栈帧，main 任务栈(20480)当场穿掉，
                 *   串口打出 "***ERROR*** A stack overflow in task main" 后
                 *   rst:0xc 重启（崩点固定在 STK: before_colrender / row_local_eval 之后）。
                 *   8KB 只是装一个真实约 600B 的 manifest，没必要吃内部 RAM 的栈：
                 *   改到 PSRAM。此分支仅在 registry 缺失时才走，不在正常渲染路径上。
                 *   分配失败按原逻辑退化为"已安装、版本未知"，不引入新行为。 */
                char *js = (char *)heap_caps_malloc(LPK_MF_MAX + 1,
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                size_t jlen = 0;
                if (!js) {
                    ESP_LOGW(TAG, "manifest 缓冲分配失败(PSRAM): %s（按已安装、版本未知处理）", app_id);
                } else if (read_small(mfp, js, LPK_MF_MAX + 1, &jlen) && jlen) {
                    lpk_mf_t mf;
                    char why[96];
                    if (lpk_mf_parse(js, jlen, &mf, why, sizeof(why)) == LPK_MF_OK) {
                        snprintf(out->ver,   sizeof(out->ver),   "%s", mf.version);
                        snprintf(out->build, sizeof(out->build), "%s", mf.build);
                        snprintf(out->mf_ver,   sizeof(out->mf_ver),   "%s", mf.version);
                        snprintf(out->mf_build, sizeof(out->mf_build), "%s", mf.build);
                        ESP_LOGW(TAG, "registry 缺失但已安装目录存在: %s（版本 %s, 按已安装处理）",
                                 app_id, mf.version);
                    }
                }
                if (js) heap_caps_free(js);
            } else {
                ESP_LOGW(TAG, "registry 缺失且无 manifest: %s（按已安装处理，版本未知）", app_id);
            }
        }
    }
    return out->registry_ok || out->dir_ok;
}

bool am_lpkg_is_installed(const char *app_id)
{
    if (!app_id || !app_id[0]) return false;
    am_lpkg_info_t info;
    return am_lpkg_info(app_id, &info);
}

const char *am_lpkg_stem_of(const char *path, char *buf, size_t cap)
{
    stem_of(path, buf, cap);
    return buf ? buf : "";
}

int am_lpkg_count(void) { return s_lp_n; }
const char *am_lpkg_id_at(int i)  { return (i >= 0 && i < s_lp_n) ? s_lp[i].app_id : NULL; }
const char *am_lpkg_ver_at(int i) { return (i >= 0 && i < s_lp_n) ? s_lp[i].ver : NULL; }
