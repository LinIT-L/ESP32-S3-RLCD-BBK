/* ============================================================
 * am_asset_multi.c — 资源包「多文件直接下载」安装器
 * 见 am_asset_multi.h 的契约说明。
 *
 * 为什么不再走 zip
 * ---------------
 *   真机实测(2026-10-31, 41.7 MB 仙剑): 下载 252 s + 解包 133 s = 385 s,
 *   而卡上总共写了 83 MB(41.7 zip + 41.7 文件)。系统 `cp` 同一个文件 200 s
 *   都拷不完(<208 KB/s) —— 说明 313 KB/s 已经是这张卡"边读边写"的上限,
 *   再优化缓冲区也没用, 唯一能省的就是"别写两遍"。
 *
 * 分工
 * ----
 *   · 本文件只负责"清单 → 逐个文件"的编排;
 *   · 传输/校验/临时文件与替换 全部复用 net_client(单请求槽);
 *   · 建目录复用 am_asset_mkdirs();
 *   · 卸载清单格式与 zip 路径完全一致 ⇒ 上层一行都不用改。
 * ============================================================ */
#include "am_asset_multi.h"
#include "am_manifest.h"
#include "am_asset.h"
#include "net_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "AM_MF"

#define MF_TEXT_MAX        (256 * 1024)   /* 清单正文上限(≈2000 个文件) */
#define MF_FETCH_MS        20000          /* 拉清单的等待上限 */
#define MF_FILE_MS         240000         /* 单个文件的等待上限(11 MB @180 KB/s ≈ 61 s) */
#define MF_TRY             3              /* 单文件尝试次数(首次 + 2 次重试) */

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* 当前这次安装的上下文。单请求槽 ⇒ 同时只可能有一次安装, 用文件级静态即可。 */
static struct {
    const char *dir;
    char        file_base[384];  /* mirror + 清单所在目录, 拼文件 URL 用 */
    uint32_t    total;
    uint32_t    base;          /* 已完成文件的字节之和 */
    int         done_files;
    int         total_files;
    int         requests;      /* 实际发出的文件请求数 */
    int         retries;       /* 重试次数 */
    int         skipped;       /* 因"已存在且校验通过"跳过的文件数 */
    uint32_t    wrote;         /* 真正写进卡里的字节数(跳过的不算) —— 与"包总大小"区分 */
    void      (*cb)(const am_mf_prog_t *);
} S;

static void report(uint32_t cur_got) {
    if (!S.cb) return;
    am_mf_prog_t p;
    uint32_t d = S.base + cur_got;
    if (d > S.total) d = S.total;
    p.done_bytes  = d;
    p.total_bytes = S.total;
    p.done_files  = S.done_files;
    p.total_files = S.total_files;
    S.cb(&p);
}

/* <dir>/<rel> 的父目录 (原地写 out) */
static void parent_of(const char *path, char *out, size_t cap) {
    out[0] = 0;
    const char *s = strrchr(path, '/');
    if (!s || s == path) return;
    size_t n = (size_t)(s - path);
    if (n >= cap) return;
    memcpy(out, path, n);
    out[n] = 0;
}

/* 拼 URL: base + "/" + 编码后的相对路径。返回 false = 太长。 */static bool build_url(char *out, size_t cap, const char *base, const char *rel) {
    char enc[AM_MF_PATH_MAX * 3 + 2];
    am_mf_url_encode(rel, enc, (int)sizeof(enc));
    size_t bl = strlen(base), el = strlen(enc);
    bool slash = (bl > 0 && base[bl - 1] != '/');
    if (bl + (slash ? 1u : 0u) + el + 1 > cap) return false;
    memcpy(out, base, bl);
    size_t o = bl;
    if (slash) out[o++] = '/';
    memcpy(out + o, enc, el);
    out[o + el] = 0;
    return true;
}

/* 下载一个文件到 full。net_client 保证：size/crc 不匹配时**不会**替换目标文件。 */
static bool fetch_one(const char *rel, const char *full, uint32_t size, uint32_t crc, int *err) {
    char url[384];
    if (!build_url(url, sizeof(url), S.file_base, rel)) {
        ESP_LOGE(TAG, "URL 过长: %s", rel);
        *err = 1;
        return false;
    }
    for (int t = 0; t < MF_TRY; t++) {
        if (t > 0) {
            S.retries++;
            vTaskDelay(pdMS_TO_TICKS(300));
            ESP_LOGW(TAG, "重试 %d/%d: %s", t, MF_TRY - 1, rel);
        }
        netc_req_t r = { 0 };
        r.url         = url;
        r.timeout_ms  = 20000;
        r.sink_path   = full;
        r.keep_alive  = true;        /* 109 个文件共用一条 TLS 连接 */
        r.expect_size = size;        /* 先校验再替换 —— 坏数据顶不掉好文件 */
        r.expect_crc  = crc;
        S.requests++;
        if (!netc_start(&r)) { *err = 2; return false; }

        uint32_t t0 = now_ms();
        while (netc_busy() && now_ms() - t0 < MF_FILE_MS) {
            report((uint32_t)netc_got());
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        bool ok   = (netc_state() == NETC_DONE);
        int  http = netc_http();
        int  e    = netc_err();
        int  got  = netc_len();
        netc_reset();
        if (ok) return true;

        *err = (http != 0 && http != 200) ? 1 : 2;
        ESP_LOGW(TAG, "取文件失败 http=%d err=0x%x got=%d/%u: %s",
                 http, (unsigned)e, got, (unsigned)size, url);
    }
    return false;
}

/* 写卸载清单 + 安装标记 (格式与 am_asset.c 的 zip 路径完全一致) */
static bool write_marks(const char *dir, const char *stem,
                        const am_mf_file_t *arr, int n, uint32_t total) {
    char lst[320];
    snprintf(lst, sizeof(lst), "%s/.pkg.%s.lst", dir, stem);
    FILE *f = fopen(lst, "wb");
    if (!f) { ESP_LOGE(TAG, "写清单失败: %s", lst); return false; }
    for (int i = 0; i < n; i++) {
        fputs(arr[i].path, f);
        fputc('\n', f);
    }
    fclose(f);

    /* 标记最后写：中途掉电只会"看起来没装"，不会"看起来装好了"。 */
    char mark[320];
    snprintf(mark, sizeof(mark), "%s/.installed.%s", dir, stem);
    FILE *m = fopen(mark, "w");
    if (!m) { ESP_LOGE(TAG, "写安装标记失败: %s", mark); return false; }
    fprintf(m, "%d %lu\n", n, (unsigned long)total);
    fclose(m);
    return true;
}

static void stem_of(const char *zip_path, char *out, size_t cap) {
    out[0] = 0;
    const char *b = zip_path ? strrchr(zip_path, '/') : NULL;
    b = b ? b + 1 : (zip_path ? zip_path : "");
    size_t k = 0;
    while (b[k] && b[k] != '.' && k + 1 < cap) { out[k] = b[k]; k++; }
    out[k] = 0;
}

/* 串口自测入口 (remcon `mf`) —— 不带进度回调的薄包装。
 * 单独开一个函数是为了让 remcon.c 不必包含本模块头(它没有 apps 的 include 路径)。 */
int am_asset_multi_install_cli(const char *mirror, const char *rel,
                               const char *target, const char *zip) {
    return am_asset_multi_install(mirror, rel, target, zip, NULL);
}

/* 串口自测入口 (remcon `mfun`) —— 走与商店卸载**同一个**函数，验证清单清理是否干净。 */
bool am_asset_uninstall_cli(const char *target, const char *zip) {
    extern bool am_asset_uninstall(const char *zip_path, const char *target_dir);
    return am_asset_uninstall(zip, target);
}

int am_asset_multi_install(const char *mirror_base, const char *manifest_rel,
                           const char *target_dir, const char *zip_path,
                           void (*cb)(const am_mf_prog_t *)) {
    if (!mirror_base || !mirror_base[0] || !manifest_rel || !manifest_rel[0] ||
        !target_dir || !target_dir[0]) return AM_MF_NO_MANIFEST;
    if (!am_mf_path_ok(manifest_rel)) {
        ESP_LOGE(TAG, "清单路径不合法: %s", manifest_rel);
        return AM_MF_NO_MANIFEST;
    }

    char stem[64];
    stem_of(zip_path, stem, sizeof(stem));
    if (!stem[0]) return AM_MF_NO_MANIFEST;

    memset(&S, 0, sizeof(S));
    S.dir    = target_dir;
    S.cb     = cb;

    /* 文件 URL = mirror + **清单所在目录** + 相对路径。
     *   例: mirror=https://bbk.linit.cn/bbk, manifest=pkgs/pal/manifest.json
     *       ⇒ https://bbk.linit.cn/bbk/pkgs/pal/ABC.MKF
     *   （清单里的 path 是相对**目标目录**的，不能直接拿它拼镜像根。） */
    {
        char base_rel[AM_MF_PATH_MAX];
        snprintf(base_rel, sizeof(base_rel), "%s", manifest_rel);
        char *sl = strrchr(base_rel, '/');
        if (sl) *sl = 0; else base_rel[0] = 0;
        if (!build_url(S.file_base, sizeof(S.file_base), mirror_base, base_rel)) {
            ESP_LOGE(TAG, "清单目录 URL 过长: %s", manifest_rel);
            return AM_MF_NO_MANIFEST;
        }
        size_t bl = strlen(S.file_base);
        while (bl > 1 && S.file_base[bl - 1] == '/') S.file_base[--bl] = 0;
    }

    uint32_t t_all0 = now_ms();
    netc_ka_stats_reset();          /* 本次安装的复用统计从这里开始算(含中途重连) */

    /* ---------- ① 拉清单 (PSRAM, 不占内部 RAM) ---------- */
    char *txt = heap_caps_malloc(MF_TEXT_MAX, MALLOC_CAP_SPIRAM);
    if (!txt) { ESP_LOGE(TAG, "PSRAM 不足: 清单缓冲 %d B", MF_TEXT_MAX); return AM_MF_ERR_NOMEM; }

    char murl[384];
    if (!build_url(murl, sizeof(murl), mirror_base, manifest_rel)) {
        ESP_LOGE(TAG, "清单 URL 过长");
        free(txt);
        return AM_MF_NO_MANIFEST;
    }
    uint32_t t_mf0 = now_ms();
    {
        netc_req_t r = { 0 };
        r.url = murl; r.timeout_ms = 15000;
        r.body = txt; r.body_cap = MF_TEXT_MAX;
        r.keep_alive = true;                  /* 接下来 109 个文件接着用这条连接 */
        if (!netc_start(&r)) { free(txt); return AM_MF_NO_MANIFEST; }
        uint32_t t0 = now_ms();
        while (netc_busy() && now_ms() - t0 < MF_FETCH_MS) vTaskDelay(pdMS_TO_TICKS(50));
        int len = netc_len(), http = netc_http();
        bool ok = (netc_state() == NETC_DONE) && http == 200 && len > 16;
        netc_reset();
        if (!ok) {
            ESP_LOGW(TAG, "清单不可用 http=%d len=%d url=%s ⇒ 回退 ZIP 下载", http, len, murl);
            free(txt);
            return AM_MF_NO_MANIFEST;
        }
        ESP_LOGI(TAG, "清单: %d B 耗时 %ums", len, (unsigned)(now_ms() - t_mf0));
    }
    uint32_t t_mf_ms = now_ms() - t_mf0;

    /* ---------- ② 解析 + 自洽校验 ---------- */
    am_mf_head_t h;
    if (am_mf_head_parse(txt, &h) != 0) {
        ESP_LOGE(TAG, "清单头部不合法(fmt/count/total) ⇒ 回退 ZIP");
        free(txt);
        return AM_MF_NO_MANIFEST;
    }
    am_mf_file_t *arr = heap_caps_calloc((size_t)h.nfiles, sizeof(am_mf_file_t), MALLOC_CAP_SPIRAM);
    if (!arr) {
        ESP_LOGE(TAG, "PSRAM 不足: 文件表 %d 条", h.nfiles);
        free(txt);
        return AM_MF_ERR_NOMEM;
    }
    int n = am_mf_files_parse(txt, arr, h.nfiles);
    free(txt);
    if (n != h.nfiles) {
        ESP_LOGE(TAG, "清单文件表解析失败 rc=%d (期望 %d 条) ⇒ 回退 ZIP", n, h.nfiles);
        free(arr);
        return AM_MF_NO_MANIFEST;
    }
    if (!am_mf_verify_total(arr, n, &h)) {
        ESP_LOGE(TAG, "清单总字节不自洽(count=%d total=%u) ⇒ 回退 ZIP", n, (unsigned)h.total_bytes);
        free(arr);
        return AM_MF_NO_MANIFEST;
    }

    S.total       = h.total_bytes;
    S.total_files = n;
    ESP_LOGI(TAG, "多文件安装: %s → %s | %d 个文件 / %u B",
             h.pkg[0] ? h.pkg : manifest_rel, target_dir, n, (unsigned)h.total_bytes);

    /* ---------- ③ 建目录 ---------- */
    am_asset_mkdirs(target_dir);
    {
        struct stat st;
        if (stat(target_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
            ESP_LOGE(TAG, "目标目录建不出来: %s", target_dir);
            free(arr);
            return AM_MF_ERR_LAYOUT;
        }
    }

    /* ---------- ④ 逐文件 ---------- */
    report(0);
    uint32_t t_dl_ms = 0, t_verify_ms = 0;
    int failed_idx = -1, failed_err = 0;
    for (int i = 0; i < n; i++) {
        char full[320];
        if (!am_mf_join(full, (int)sizeof(full), target_dir, arr[i].path)) {
            ESP_LOGE(TAG, "路径拼接失败(过长或不合法): %s", arr[i].path);
            failed_idx = i; failed_err = 1;
            break;
        }
        char pdir[320];
        parent_of(full, pdir, sizeof(pdir));
        if (pdir[0]) am_asset_mkdirs(pdir);

        /* 已存在且大小/CRC 都对 → 跳过。断点续传靠这条。 */
        struct stat st;
        bool skip = false;
        if (stat(full, &st) == 0 && st.st_size > 0 && (uint32_t)st.st_size == arr[i].size) {
            uint32_t tv0 = now_ms();
            skip = (netc_crc32_file(full) == arr[i].crc32);
            t_verify_ms += now_ms() - tv0;
        }
        if (skip) {
            S.skipped++;
            S.base += arr[i].size;
            S.done_files++;
            report(0);
            continue;
        }

        uint32_t td0 = now_ms();
        int err = 0;
        bool ok = fetch_one(arr[i].path, full, arr[i].size, arr[i].crc32, &err);
        t_dl_ms += now_ms() - td0;
        if (!ok) {
            S.retries += 0;   /* 已在内层累计 */
            failed_idx = i; failed_err = err;
            ESP_LOGE(TAG, "放弃于第 %d/%d 个文件: %s (err=%d)", i + 1, n, arr[i].path, err);
            break;
        }
        S.base += arr[i].size;
        S.done_files++;
        S.wrote += arr[i].size;      /* 走到这里就是真下了(跳过分支已 continue) */
        report(0);
    }

    /* ---------- ⑤ 收尾 ---------- */
    if (failed_idx >= 0) {
        /* 已完成的不删 —— 再点一次下载即从失败处续传(已下好的靠 CRC 跳过)。 */
        ESP_LOGE(TAG, "多文件安装中断: 完成 %d/%d, 卡上已写 %u B (失败文件 err=%d, "
                      "已完成的不删 —— 再点一次下载即从失败处续传)",
                 S.done_files, n, (unsigned)S.base, failed_err);
        free(arr);
        return AM_MF_ERR_FILE;
    }

    if (!write_marks(target_dir, stem, arr, n, S.total)) {
        free(arr);
        return AM_MF_ERR_MARK;
    }

    uint32_t total_ms = now_ms() - t_all0;
    int ka_hits = 0, ka_rec = 0;
    netc_ka_stats(&ka_hits, &ka_rec);
    /* 一次装完只打这一行，性能指标全在里面（速度/TF 卡写入量/请求数/重试/连接复用） */
    ESP_LOGI(TAG, "多文件安装完成: %d 个文件 / %u B | 清单 %ums 下载 %ums 校验 %ums 总 %ums "
                  "| 均速 %u B/s | 请求 %d 次 重试 %d 次 跳过 %d 个 | 复用 %d 次 重连 %d 次 "
                  "| 卡写入 %u B",
             n, (unsigned)S.total, (unsigned)t_mf_ms, (unsigned)t_dl_ms,
             (unsigned)t_verify_ms, (unsigned)total_ms,
             (unsigned)(total_ms ? (uint64_t)S.total * 1000u / total_ms : 0),
             S.requests, S.retries, S.skipped, ka_hits, ka_rec, (unsigned)S.wrote);
    free(arr);
    return AM_MF_OK;
}
