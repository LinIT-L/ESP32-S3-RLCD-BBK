/**
 * web_files.c — TF 卡网页文件管理（/files/，第三回合）。
 *
 * 设计要点（为什么这么做，逐条对应验收要求）：
 *   1. **不新建 HTTP Server**：全部走 web_core 的前缀注册，端口/生命周期/认证都是同一份。
 *   2. **大文件不占 RAM**：上传用一块 8KB PSRAM 缓冲反复 recv→fwrite；下载用 4KB 反复
 *      fread→send_chunk。任何大小的文件都不会整包进内存。
 *   3. **不留半截正式文件**：上传先写同目录的隐藏临时文件 `.part-xxxx`，长度核对通过才
 *      rename 覆盖正式文件；失败/断连/空间不足一律删临时文件。
 *   4. **路径穿越**：所有路径都过 wf_norm()——只允许 /sdcard 之下，拒绝 `..`、控制字符、
 *      反斜杠，`.`/空段被吃掉。文件名同样走这条路（拼成完整路径再归一化）。
 *   5. **不破坏现有访问**：只用 POSIX/FatFs 与 sd_scan 的公开接口，不加自己的挂载逻辑；
 *      写操作在本模块内用互斥量串行化（FatFs 同一卷并发写不安全）。
 *   6. **认证**：页面与 login 公开（要显示登录框），api 整段 need_auth=true；
 *      token 只在 Authorization 头里，URL 里不带。
 *
 * ⚠ 已知限制（写进交付报告，别当成已实现）：
 *   · 临时文件机制只是"失败即清理"，**不是**断点续传；重传从头开始。
 *   · 下载用 chunked（无 Content-Length），浏览器不显示总进度。
 *   · sd_scan 的 sd_op_watchdog_begin/end 目前是空实现，卡被拔时底层可能阻塞在
 *     FatFs 里；本模块只能靠"每块都检查返回值"尽早失败，没有强制复位兜底。
 */
#include "web_files.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "keyvault.h"        /* PIN 校验（与收藏夹同一个 keyvault） */
#include "sd_scan.h"
#include "web_core.h"
#include "web_files_page.h"
#include "web_files_path.h"   /* wf_norm/wf_join/wf_urldecode: 与宿主机单测同一份实现 */
#include "wifi_manager.h"

static const char *TAG = "WEBFILES";

/* WF_ROOT / WF_PATH_MAX 由 web_files_path.h 提供（单测同一份） */
#define WF_IO_CHUNK    8192     /* 上传搬运块（PSRAM） */
#define WF_TX_CHUNK    4096     /* 下载发送块（PSRAM） */
#define WF_LIST_BUF    (40 * 1024)
#define WF_AP_SSID     "LinTOS-Files"

static bool              s_running = false;
static bool              s_ap_ours = false;   /* 热点是不是本模块开的 */
static SemaphoreHandle_t s_io_lock = NULL;

/* ---------------- 小工具 ---------------- */

/* 路径/参数的实现见 web_files_path.h（与宿主机单测共用同一份），这里不再重复定义。 */

/* JSON 字符串转义（中文按原字节透传，只处理 "、\ 与控制字符）。 */
static void wf_esc(char *o, size_t cap, const char *s)
{
    static const char hx[] = "0123456789abcdef";
    size_t k = 0;
    if (!cap) return;
    for (; s && *s && k + 8 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { o[k++] = '\\'; o[k++] = (char)c; }
        else if (c < 0x20) {
            o[k++] = '\\'; o[k++] = 'u'; o[k++] = '0'; o[k++] = '0';
            o[k++] = hx[c >> 4]; o[k++] = hx[c & 15];
        } else o[k++] = (char)c;
    }
    o[k] = 0;
}

/**
 * 取参数：先查 query（httpd 已解码），再退回自定义请求头（值需 percent-encoded）。
 * 为什么要请求头这条路：中文名 URL 编码后每字 9 字节，CONFIG_HTTPD_MAX_URI_LEN=512，
 * 长名字/深目录会超；头部上限 1024，且页面始终走头部，query 只作短路径的兼容。
 */
static bool wf_arg(httpd_req_t *req, const char *key, char *out, size_t cap, const char *hdr)
{
    if (!out || cap == 0) return false;
    out[0] = 0;
    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen > 0 && qlen < 480) {
        char q[480];
        if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
            char v[WF_PATH_MAX];
            if (httpd_query_key_value(q, key, v, sizeof(v)) == ESP_OK) {
                snprintf(out, cap, "%s", v);
                return out[0] != 0;
            }
        }
    }
    if (hdr) {
        size_t hl = httpd_req_get_hdr_value_len(req, hdr);
        if (hl > 0 && hl < WF_PATH_MAX) {
            char v[WF_PATH_MAX];
            if (httpd_req_get_hdr_value_str(req, hdr, v, sizeof(v)) == ESP_OK) {
                wf_urldecode(v, out, cap);
                return out[0] != 0;
            }
        }
    }
    return false;
}

/* 逐级 mkdir（已存在不算错）。 */
static void wf_mkdirs(const char *path)
{
    char tmp[WF_PATH_MAX];
    if (snprintf(tmp, sizeof(tmp), "%s", path) >= (int)sizeof(tmp)) return;
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        mkdir(tmp, 0777);
        *p = '/';
    }
    mkdir(tmp, 0777);
}

/* 取父目录：" /sdcard/a/b " → " /sdcard/a "。 */
static bool wf_parent(const char *path, char *out, size_t cap)
{
    const char *s = strrchr(path, '/');
    if (!s || s == path) return false;
    size_t n = (size_t)(s - path);
    if (n >= cap) return false;
    memcpy(out, path, n);
    out[n] = 0;
    return true;
}

/* 递归删除（目录整删）。深度上限防跑飞。 */
static bool wf_rm_rf(const char *path, int depth)
{
    struct stat st;
    if (depth > 8) return false;
    if (stat(path, &st) != 0) return false;
    if (!S_ISDIR(st.st_mode)) return remove(path) == 0;
    DIR *d = opendir(path);
    if (!d) return false;
    struct dirent *ent;
    bool ok = true;
    while ((ent = readdir(d)) != NULL) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        char full[WF_PATH_MAX];
        if (snprintf(full, sizeof(full), "%s/%s", path, ent->d_name) >= (int)sizeof(full)) {
            ok = false;
            continue;
        }
        if (!wf_rm_rf(full, depth + 1)) ok = false;
    }
    closedir(d);
    return ok && rmdir(path) == 0;
}

/* 读写互斥：FatFs 同一卷并发写不安全，本模块的写操作统一串行。 */
static bool wf_lock(uint32_t ms)
{
    if (!s_io_lock) return true;   /* 没建锁（极端情况）就当无锁，不阻塞功能 */
    return xSemaphoreTake(s_io_lock, pdMS_TO_TICKS(ms)) == pdTRUE;
}
static void wf_unlock(void) { if (s_io_lock) xSemaphoreGive(s_io_lock); }

/* ---------------- 页面 / 登录 ---------------- */

static esp_err_t h_page(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, wf_page_html(), HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_login(httpd_req_t *req)
{
    char pin[24] = { 0 };
    int n = httpd_req_recv(req, pin, sizeof(pin) - 1);
    if (n <= 0) return web_core_send_err(req, "400 Bad Request", "len");
    pin[n] = 0;
    keyvault_init();
    /* 没设 PIN 时不能"放行"——那等于把 TF 卡整个公开。给专门错误码让页面提示。 */
    if (!keyvault_has_pin()) return web_core_send_err(req, "401 Unauthorized", "nopin");
    char tok[WEB_CORE_TOKEN_LEN + 1];
    if (!web_core_auth_login(pin, tok, sizeof(tok)))
        return web_core_send_err(req, "401 Unauthorized", "auth");
    char out[64];
    snprintf(out, sizeof(out), "{\"ok\":true,\"t\":\"%s\"}", tok);
    return web_core_send_json(req, out);
}

static bool wf_pin_check(const char *pin)
{
    keyvault_init();
    if (!keyvault_has_pin()) return false;
    return keyvault_verify_pin(pin) == 0;
}

/* ---------------- 列目录 ---------------- */

static esp_err_t wf_mount_guard(httpd_req_t *req)
{
    if (!sd_is_mounted()) return web_core_send_err(req, "409 Conflict", "nomount");
    return ESP_OK;
}

static esp_err_t h_list(httpd_req_t *req)
{
    char raw[WF_PATH_MAX] = { 0 };
    char path[WF_PATH_MAX];
    wf_arg(req, "p", raw, sizeof(raw), "X-Path");
    if (!wf_norm(raw, path, sizeof(path))) return web_core_send_err(req, "400 Bad Request", "path");

    /* 未挂载不是错误：页面要能显示"TF 卡未挂载"，所以照常 200 + mounted:false。 */
    if (!sd_is_mounted()) {
        char out[160];
        snprintf(out, sizeof(out),
                 "{\"ok\":true,\"mounted\":false,\"p\":\"/sdcard\",\"total\":0,\"free\":0,\"more\":0,\"e\":[]}");
        return web_core_send_json(req, out);
    }

    DIR *d = opendir(path);
    if (!d) return web_core_send_err(req, "404 Not Found", "nodir");

    char *buf = heap_caps_malloc(WF_LIST_BUF, MALLOC_CAP_SPIRAM);
    if (!buf) {
        closedir(d);
        ESP_LOGE(TAG, "列目录缓冲分配失败");
        return web_core_send_err(req, "500 Internal Server Error", "mem");
    }

    uint64_t total = 0, freeb = 0;
    sd_get_info(&total, &freeb);      /* 已挂载时只读 VFS info，不会重挂 */

    char esc[WF_PATH_MAX * 2];
    wf_esc(esc, sizeof(esc), path);
    int n = snprintf(buf, WF_LIST_BUF,
                     "{\"ok\":true,\"mounted\":true,\"p\":\"%s\",\"total\":%llu,\"free\":%llu,\"e\":[",
                     esc, (unsigned long long)total, (unsigned long long)freeb);

    struct dirent *ent;
    bool first = true, more = false;
    while ((ent = readdir(d)) != NULL) {
        const char *nm = ent->d_name;
        /* 跳过 . / .. 、点开头的隐藏项（含本模块的 .part-* 临时文件与 _ 开头的目录） */
        if (nm[0] == '.') continue;
        char full[WF_PATH_MAX];
        if (snprintf(full, sizeof(full), "%s/%s", path, nm) >= (int)sizeof(full)) continue;
        struct stat st;
        if (stat(full, &st) != 0) continue;
        bool isdir = S_ISDIR(st.st_mode);
        if (!isdir && !S_ISREG(st.st_mode)) continue;   /* 跳过设备节点等 */
        /* 预算：名字最长 255 → 转义最多 6 倍 + 固定字段 */
        if (n + 64 + (int)strlen(nm) * 6 >= WF_LIST_BUF) { more = true; break; }
        wf_esc(esc, sizeof(esc), nm);
        n += snprintf(buf + n, WF_LIST_BUF - n, "%s{\"n\":\"%s\",\"d\":%d,\"s\":%lu}",
                      first ? "" : ",", esc, isdir ? 1 : 0,
                      (unsigned long)(isdir ? 0 : (unsigned long)st.st_size));
        first = false;
    }
    closedir(d);
    snprintf(buf + n, WF_LIST_BUF - n, "],\"more\":%d}", more ? 1 : 0);

    esp_err_t e = web_core_send_json(req, buf);
    free(buf);
    return e;
}

/* ---------------- 上传（流式，边收边写） ---------------- */

static esp_err_t h_upload(httpd_req_t *req)
{
    char rawn[WF_PATH_MAX] = { 0 }, name[WF_PATH_MAX] = { 0 };
    char rawp[WF_PATH_MAX] = { 0 }, dir[WF_PATH_MAX], target[WF_PATH_MAX];

    wf_arg(req, "p", rawp, sizeof(rawp), "X-Path");
    if (!wf_arg(req, "n", name, sizeof(name), "X-Name"))
        return web_core_send_err(req, "400 Bad Request", "name");
    if (!wf_norm(rawp, dir, sizeof(dir))) return web_core_send_err(req, "400 Bad Request", "path");
    if (!wf_join(dir, name, target, sizeof(target))) return web_core_send_err(req, "400 Bad Request", "name");

    esp_err_t g = wf_mount_guard(req);
    if (g != ESP_OK) return g;

    char cl[20] = { 0 };
    if (httpd_req_get_hdr_value_str(req, "Content-Length", cl, sizeof(cl)) != ESP_OK)
        return web_core_send_err(req, "411 Length Required", "len");
    long long len = atoll(cl);
    if (len < 0) return web_core_send_err(req, "400 Bad Request", "len");

    /* 空间预检：留 64KB 余量给文件系统元数据 */
    uint64_t total = 0, freeb = 0;
    if (sd_get_info(&total, &freeb) == 0 && freeb > 0 && (uint64_t)len + 65536ULL > freeb) {
        ESP_LOGW(TAG, "空间不足: 需要 %lld, 剩 %llu", len, (unsigned long long)freeb);
        return web_core_send_err(req, "507 Insufficient Storage", "space");
    }

    struct stat st;
    bool exists = (stat(target, &st) == 0);
    if (exists && S_ISDIR(st.st_mode)) return web_core_send_err(req, "409 Conflict", "isdir");
    char ov[4] = { 0 };
    wf_arg(req, "o", ov, sizeof(ov), "X-Ov");
    if (exists && ov[0] != '1')
        return web_core_send_err(req, "409 Conflict", "exists");   /* 覆盖必须显式确认 */

    /* 目标目录（含文件夹上传的中间层）先建好 */
    char pdir[WF_PATH_MAX];
    if (wf_parent(target, pdir, sizeof(pdir))) wf_mkdirs(pdir);

    if (!wf_lock(5000)) return web_core_send_err(req, "503 Service Unavailable", "busy");

    /* 临时文件放在同目录（rename 才能原子），名字以 '.' 开头 ⇒ 列表里不可见 */
    char tmp[WF_PATH_MAX + 24];
    int tn = snprintf(tmp, sizeof(tmp), "%s/.part-%08lx",
                      (wf_parent(target, pdir, sizeof(pdir)) ? pdir : dir),
                      (unsigned long)esp_random());
    if (tn <= 0 || tn >= (int)sizeof(tmp)) {
        wf_unlock();
        return web_core_send_err(req, "400 Bad Request", "toobig");
    }
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        wf_unlock();
        ESP_LOGE(TAG, "建临时文件失败: %s", tmp);
        return web_core_send_err(req, "500 Internal Server Error", "io");
    }

    char *buf = heap_caps_malloc(WF_IO_CHUNK, MALLOC_CAP_SPIRAM);
    if (!buf) {
        fclose(f);
        remove(tmp);
        wf_unlock();
        return web_core_send_err(req, "500 Internal Server Error", "mem");
    }

    long long got = 0;
    bool ok = true;
    while (got < len) {
        size_t want = (size_t)((len - got) > WF_IO_CHUNK ? WF_IO_CHUNK : (len - got));
        int r = httpd_req_recv(req, buf, want);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;        /* 客户端慢：继续等，不算失败 */
        if (r <= 0) { ok = false; ESP_LOGW(TAG, "上传中断: %lld/%lld", got, len); break; }
        if (fwrite(buf, 1, (size_t)r, f) != (size_t)r) {  /* 拔卡/写满在这体现 */
            ok = false;
            ESP_LOGE(TAG, "写卡失败: %s", tmp);
            break;
        }
        got += r;
    }
    free(buf);
    if (fclose(f) != 0) ok = false;                       /* 收尾失败 = 没落盘 */
    if (ok && got != len) ok = false;

    if (ok) {
        if (exists) remove(target);                       /* FATFS rename 不覆盖 */
        if (rename(tmp, target) != 0) {
            ESP_LOGE(TAG, "改名失败: %s → %s", tmp, target);
            ok = false;
        }
    }
    if (!ok) remove(tmp);
    wf_unlock();

    if (!ok) return web_core_send_err(req, "500 Internal Server Error", "io");

    char out[96];
    snprintf(out, sizeof(out), "{\"ok\":true,\"s\":%lld}", got);
    ESP_LOGI(TAG, "上传完成: %s (%lld B)", target, got);
    return web_core_send_json(req, out);
}

/* ---------------- 下载（分块发送） ---------------- */

static esp_err_t h_download(httpd_req_t *req)
{
    char raw[WF_PATH_MAX] = { 0 }, path[WF_PATH_MAX];
    wf_arg(req, "p", raw, sizeof(raw), "X-Path");
    if (!wf_norm(raw, path, sizeof(path))) return web_core_send_err(req, "400 Bad Request", "path");
    esp_err_t g = wf_mount_guard(req);
    if (g != ESP_OK) return g;

    struct stat st;
    if (stat(path, &st) != 0) return web_core_send_err(req, "404 Not Found", "notfound");
    if (S_ISDIR(st.st_mode)) return web_core_send_err(req, "409 Conflict", "isdir");
    if (!S_ISREG(st.st_mode)) return web_core_send_err(req, "400 Bad Request", "plain");

    FILE *f = fopen(path, "rb");
    if (!f) return web_core_send_err(req, "500 Internal Server Error", "io");

    /* Content-Disposition：中文名必须用 RFC5987 的 filename*=UTF-8''%XX，否则浏览器乱码 */
    char cd[320];
    int k = snprintf(cd, sizeof(cd), "attachment; filename*=UTF-8''");
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    for (const char *p = base; *p && k > 0 && k < (int)sizeof(cd) - 4; p++) {
        unsigned char c = (unsigned char)*p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '.' || c == '-' || c == '_' || c == '~') cd[k++] = (char)c;
        else k += snprintf(cd + k, sizeof(cd) - (size_t)k, "%%%02X", c);
    }
    if (k > 0 && k < (int)sizeof(cd)) cd[k] = 0; else cd[sizeof(cd) - 1] = 0;

    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", cd);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    char *buf = heap_caps_malloc(WF_TX_CHUNK, MALLOC_CAP_SPIRAM);
    if (!buf) {
        fclose(f);
        return web_core_send_err(req, "500 Internal Server Error", "mem");
    }

    esp_err_t ret = ESP_OK;
    size_t r;
    while ((r = fread(buf, 1, WF_TX_CHUNK, f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, r) != ESP_OK) { ret = ESP_FAIL; break; }
    }
    free(buf);
    fclose(f);
    if (ret != ESP_OK) return ret;
    return httpd_resp_send_chunk(req, NULL, 0);          /* 结束 chunked 流 */
}

/* ---------------- 建目录 / 删除 / 改名 ---------------- */

static esp_err_t h_mkdir(httpd_req_t *req)
{
    char rawp[WF_PATH_MAX] = { 0 }, rawn[WF_PATH_MAX] = { 0 };
    char dir[WF_PATH_MAX], target[WF_PATH_MAX];
    wf_arg(req, "p", rawp, sizeof(rawp), "X-Path");
    if (!wf_arg(req, "n", rawn, sizeof(rawn), "X-Name"))
        return web_core_send_err(req, "400 Bad Request", "name");
    if (!wf_norm(rawp, dir, sizeof(dir))) return web_core_send_err(req, "400 Bad Request", "path");
    if (!wf_join(dir, rawn, target, sizeof(target)))
        return web_core_send_err(req, "400 Bad Request", "name");
    esp_err_t g = wf_mount_guard(req);
    if (g != ESP_OK) return g;

    if (!wf_lock(3000)) return web_core_send_err(req, "503 Service Unavailable", "busy");
    int rc = mkdir(target, 0777);
    wf_unlock();
    if (rc != 0) {
        struct stat st;
        if (stat(target, &st) == 0) return web_core_send_err(req, "409 Conflict", "exists");
        return web_core_send_err(req, "500 Internal Server Error", "io");
    }
    ESP_LOGI(TAG, "新建目录: %s", target);
    return web_core_send_json(req, "{\"ok\":true}");
}

static esp_err_t h_del(httpd_req_t *req)
{
    char raw[WF_PATH_MAX] = { 0 }, path[WF_PATH_MAX];
    wf_arg(req, "p", raw, sizeof(raw), "X-Path");
    if (!wf_norm(raw, path, sizeof(path))) return web_core_send_err(req, "400 Bad Request", "path");
    if (!strcmp(path, WF_ROOT)) return web_core_send_err(req, "400 Bad Request", "path");  /* 不许删根 */
    esp_err_t g = wf_mount_guard(req);
    if (g != ESP_OK) return g;

    if (!wf_lock(5000)) return web_core_send_err(req, "503 Service Unavailable", "busy");
    bool ok = wf_rm_rf(path, 0);
    wf_unlock();
    if (!ok) return web_core_send_err(req, "500 Internal Server Error", "io");
    ESP_LOGI(TAG, "已删除: %s", path);
    return web_core_send_json(req, "{\"ok\":true}");
}

static esp_err_t h_rename(httpd_req_t *req)
{
    char rawp[WF_PATH_MAX] = { 0 }, rawn[WF_PATH_MAX] = { 0 };
    char path[WF_PATH_MAX], target[WF_PATH_MAX], pdir[WF_PATH_MAX];
    wf_arg(req, "p", rawp, sizeof(rawp), "X-Path");
    if (!wf_arg(req, "n", rawn, sizeof(rawn), "X-Name"))
        return web_core_send_err(req, "400 Bad Request", "name");
    if (!wf_norm(rawp, path, sizeof(path))) return web_core_send_err(req, "400 Bad Request", "path");
    if (strchr(rawn, '/')) return web_core_send_err(req, "400 Bad Request", "name");  /* 改名不换目录 */
    if (!wf_parent(path, pdir, sizeof(pdir))) return web_core_send_err(req, "400 Bad Request", "path");
    if (!wf_join(pdir, rawn, target, sizeof(target)))
        return web_core_send_err(req, "400 Bad Request", "name");
    esp_err_t g = wf_mount_guard(req);
    if (g != ESP_OK) return g;

    struct stat st;
    if (stat(path, &st) != 0) return web_core_send_err(req, "404 Not Found", "notfound");
    if (!strcmp(path, target)) return web_core_send_json(req, "{\"ok\":true}");
    if (stat(target, &st) == 0) return web_core_send_err(req, "409 Conflict", "exists");

    if (!wf_lock(3000)) return web_core_send_err(req, "503 Service Unavailable", "busy");
    int rc = rename(path, target);
    wf_unlock();
    if (rc != 0) return web_core_send_err(req, "500 Internal Server Error", "io");
    ESP_LOGI(TAG, "改名: %s → %s", path, target);
    return web_core_send_json(req, "{\"ok\":true}");
}

/* ---------------- 分发 ---------------- */

static esp_err_t h_api(httpd_req_t *req)
{
    const char *p = strstr(req->uri, "/api/");   /* req->uri 是定长数组，不会为 NULL */
    if (!p) return web_core_send_err(req, "404 Not Found", "notfound");
    p += 5;
    if (!strcmp(p, "list"))     return h_list(req);
    if (!strcmp(p, "upload"))   return h_upload(req);
    if (!strcmp(p, "download")) return h_download(req);
    if (!strcmp(p, "mkdir"))    return h_mkdir(req);
    if (!strcmp(p, "del"))      return h_del(req);
    if (!strcmp(p, "rename"))   return h_rename(req);
    return web_core_send_err(req, "404 Not Found", "notfound");
}

/* ---------------- 注册 / 生命周期 ---------------- */

static const httpd_uri_t s_page_uris[] = {
    { .uri = "/",      .method = HTTP_GET,  .handler = h_page,  .user_ctx = NULL },
    { .uri = "/login", .method = HTTP_POST, .handler = h_login, .user_ctx = NULL },
};
static const web_app_t s_page_app = {
    .name = "files", .prefix = "/files",
    .uris = s_page_uris, .n_uris = sizeof(s_page_uris) / sizeof(s_page_uris[0]),
    .need_auth = false, .is_default = false,
};

static const httpd_uri_t s_api_uris[] = {
    { .uri = "/api/*", .method = HTTP_ANY, .handler = h_api, .user_ctx = NULL },
};
static const web_app_t s_api_app = {
    .name = "files_api", .prefix = "/files",
    .uris = s_api_uris, .n_uris = 1,
    .need_auth = true,   /* 整段 API 强制 Bearer（含上传/下载/删除） */
    .is_default = false,
};

esp_err_t web_files_start(void)
{
    if (s_running) return ESP_OK;
    if (!s_io_lock) s_io_lock = xSemaphoreCreateMutex();

    /* ★★★ 2026-11-01 linit 定调：这条功能是**局域网共享**，不开热点 ★★★
     *   使用方式 = 掌机连上家里的 WiFi(STA)，电脑/手机在同一网络下访问 http://<设备IP>/files/
     *   原来"没连 WiFi 就自动开热点 8.8.8.8"那条路**用户明确不要**，已删除。
     *   顺带解决了必须先有 lwIP 才能起 httpd 的顺序问题（见下方说明）：
     *   未连接就直接失败返回，绝不在没有协议栈的情况下调 httpd_start。 */
    if (!wifi_manager_is_connected()) {
        ESP_LOGE(TAG, "WiFi 未连接 ⇒ 局域网共享需要设备先连上 WiFi(STA)");
        return ESP_FAIL;
    }

    web_core_auth_set_pin_check(wf_pin_check);

    esp_err_t e = web_core_start(0);          /* 此时 lwIP 已随 STA 连接就绪 */
    if (e != ESP_OK) return e;
    e = web_core_register(&s_page_app);
    if (e != ESP_OK) goto fail_page;
    e = web_core_register(&s_api_app);
    if (e != ESP_OK) goto fail_api;

    wifi_manager_ps_hold(true);      /* 传文件期间别省电，否则每个请求多等几百 ms */
    s_running = true;

    char ip[20] = "-";
    web_files_get_addr(ip, sizeof(ip));
    ESP_LOGI(TAG, "TF 卡共享已开启(局域网): http://%s/files/  (PSRAM 剩余 %u)",
             ip, (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return ESP_OK;

fail_api:
    web_core_unregister(&s_page_app);
fail_page:
    web_core_stop();
    return e;
}

void web_files_stop(void)
{
    if (!s_running) return;
    s_running = false;
    web_core_unregister(&s_api_app);
    web_core_unregister(&s_page_app);
    web_core_stop();
    if (!web_core_is_running()) web_core_auth_clear();   /* 服务真停了才作废 token */
    if (s_ap_ours) { wifi_manager_ap_stop(); s_ap_ours = false; }
    wifi_manager_ps_hold(false);
    ESP_LOGI(TAG, "TF 卡共享已关闭");
}

bool web_files_is_running(void) { return s_running; }

bool web_files_get_addr(char *out, size_t cap)
{
    if (!out || cap == 0) return false;
    out[0] = 0;
    if (s_ap_ours) { snprintf(out, cap, "8.8.8.8"); return true; }
    return wifi_manager_get_ip(out, cap);
}
