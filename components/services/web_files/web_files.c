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
 *   · sd_scan 里曾经那对"关键操作看门狗"已删除 (空实现, 且全项目无人调用), 现在
 *     **没有任何超时保护**: 卡被拔时底层可能阻塞在 FatFs 里; 本模块只能靠
 *     "每块都检查返回值"尽早失败, 没有强制复位兜底。
 */
#include "web_files.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"       /* ★ 2026-11-25: 活动保活时间戳 (h_api 入口) */
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"    /* ★ 2026-11-24: 交卡清场 / 开机恢复任务 */

#include "keyvault.h"        /* PIN 校验（与收藏夹同一个 keyvault） */
#include "sd_arb.h"          /* ★ 2026-11-24: SD 访问仲裁 —— 交卡期间必须让路 */
#include "sd_scan.h"
#include "web_core.h"
#include "web_files_page.h"
#include "web_files_path.h"   /* wf_norm/wf_join/wf_urldecode: 与宿主机单测同一份实现 */
#include "wifi_manager.h"

#include "nvs.h"              /* ★ 2026-11-24: 记录"用户开过共享", 重启后自动恢复 */

static const char *TAG = "WEBFILES";

/* WF_ROOT / WF_PATH_MAX 由 web_files_path.h 提供（单测同一份） */
#define WF_IO_CHUNK    8192     /* 上传搬运块（PSRAM） */
#define WF_TX_CHUNK    4096     /* 下载发送块（PSRAM） */
#define WF_LIST_BUF    (40 * 1024)
#define WF_AP_SSID     "LinTOS-Files"

static bool              s_running = false;
static bool              s_ap_ours = false;   /* 热点是不是本模块开的 */
static SemaphoreHandle_t s_io_lock = NULL;
/* ★ 2026-11-24: 交卡/关闭共享时要能"打断"正在跑的上传下载 —— 否则
 *   FTL 一收卡就把 VFS 注销掉, 传输线程手里的 FILE* 立刻变悬垂指针。
 *   s_xfer  = 正在流式传输的请求数 (上传/下载)
 *   s_abort = 请求它们尽快收尾 (每块检查一次)
 *   s_paused= 不再受理新请求 (从收到交卡请求到重新挂载完成) */
static volatile int      s_xfer     = 0;
static volatile bool     s_abort    = false;
static volatile bool     s_paused   = false;
/* ★ 2026-11-25: 最近一次 API 访问的时刻(ms)。
 * web_files_busy() 的 s_xfer 只覆盖**流式上传/下载**, 列目录/建目录/删除/改名这些
 * 短请求不在其中 —— 用户在浏览器里翻目录时, 每个请求之间可能停顿 >1 分钟,
 * 屏保监督任务只看 s_xfer 就会判"闲置"进壁纸、再 3 分钟关射频, 正在浏览的会话被砍。
 * 在 h_api() 入口统一刷新本时间戳(覆盖全部 API), 供 web_files_active() 判活。 */
static volatile uint32_t s_last_access_ms = 0;
#define WF_ACTIVE_WINDOW_MS  30000u   /* 30s 内有访问视为"活动", 覆盖翻目录的停顿 */

/* 前向声明: web_files_stop() 在定义之前就要用它 */
static void wf_auto_save(bool on);

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

/* 参数取值缓冲上限：整条 URI 受 CONFIG_HTTPD_MAX_URI_LEN(512) 限制，
 * 整行头受 CONFIG_HTTPD_MAX_REQ_HDR_LEN(1024) 限制，512 足够装编码后的值。 */
#define WF_ARG_ENC_MAX 512

/* 编码串解码后要多少字节（不含结尾 0）。
 * 为什么要它：wf_urldecode() 满了就静默截断，而**截断后的路径仍然是合法的**
 * （比如 /sdcard/A/B 截成 /sdcard/A），上传/建目录会悄悄写到另一个目录里。
 * 所以这里先算长度，放不下就明确报错，绝不截断复用。 */
static size_t wf_dec_len(const char *s)
{
    size_t n = 0;
    while (s && *s) {
        if (s[0] == '%' && s[1] && s[2] && wf_hex(s[1]) >= 0 && wf_hex(s[2]) >= 0) s += 3;
        else s++;
        n++;
    }
    return n;
}

/**
 * 取参数：先查 query，再退回自定义请求头（两条路的值都是 percent-encoded）。
 *
 * 返回值：1 = 取到 / 0 = 没带这个参数 / -1 = 带了但太长（调用方应回 400 toobig）
 *
 * ★ 2026-11-24 修复「浏览器看不到子目录/内容」的两个要点：
 *
 *   (a) IDF 的 httpd_query_key_value() **不做 percent 解码**（httpd_parse.c 里
 *       逐字节 memcpy），所以 "/sdcard/data" 到这来是 "%2Fsdcard%2Fdata"，
 *       直接交给 wf_norm() 必判"不是 /sdcard 之下"→ 400 path。这里必须自己解。
 *
 *   (b) 不再用 httpd_req_get_url_query_str() 把整条 query 再拷进 q[480] 才查。
 *       req->uri 本身就是以 '\0' 结尾的整条请求目标（含 `?...`），把指针挪到 '?'
 *       之后交给 httpd_query_key_value() 正是它要的输入（它要求 query **不含** '?'）。
 *       这样省 480B 栈，也不再让超过 480 字节的 query 直接失效。
 *
 * 为什么要请求头这条路：中文名 URL 编码后每字 9 字节，URI 总长上限 512，
 * 深目录/长名字仍可能塞不进 query；头行有 1024，前端两个都发，头部作兜底。
 */
static int wf_arg(httpd_req_t *req, const char *key, char *out, size_t cap, const char *hdr)
{
    bool too_long = false;
    if (!out || cap < 2) return 0;
    out[0] = 0;

    const char *qm = strchr(req->uri, '?');       /* 路径段里不会出现裸 '?' */
    if (qm && qm[1]) {
        char enc[WF_ARG_ENC_MAX];
        esp_err_t qe = httpd_query_key_value(qm + 1, key, enc, sizeof(enc));
        if (qe == ESP_OK) {
            if (wf_dec_len(enc) < cap) {
                wf_urldecode(enc, out, cap);
                if (out[0]) return 1;
            } else {
                too_long = true;
            }
        } else if (qe == ESP_ERR_HTTPD_RESULT_TRUNC) {
            too_long = true;                      /* 值本身超长，退回头部再试 */
        }
    }
    if (hdr) {
        size_t hl = httpd_req_get_hdr_value_len(req, hdr);
        if (hl > 0) {
            char v[WF_ARG_ENC_MAX];
            esp_err_t he = httpd_req_get_hdr_value_str(req, hdr, v, sizeof(v));
            if (he == ESP_OK) {
                if (wf_dec_len(v) >= cap) return -1;   /* 不截断, 明确报错 */
                wf_urldecode(v, out, cap);
                if (out[0]) return 1;
            } else if (he == ESP_ERR_HTTPD_RESULT_TRUNC) {
                return -1;
            }
        }
    }
    return too_long ? -1 : 0;
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

/* 读写互斥 + SD 仲裁闸门。
 * ★ 2026-11-24: 原来只保护"本模块自己的写操作串行", 挡不住 USB MSC 交卡:
 *   交卡时 esp_vfs_fat_unregister_path() 会 free 掉 VFS 上下文, 正在传输的
 *   FILE* 立刻变成悬垂指针。现在所有走文件系统的 handler 都先过 sd_arb_enter(),
 *   交卡窗口里直接拿不到锁 ⇒ 返回 503, 不可能再打开/读写卡。 */
static bool wf_lock(uint32_t ms)
{
    if (!sd_arb_enter()) return false;          /* 卡不在本机手上 */
    if (!s_io_lock) return true;                /* 没建锁（极端情况）就当无锁，不阻塞功能 */
    if (xSemaphoreTake(s_io_lock, pdMS_TO_TICKS(ms)) != pdTRUE) {
        sd_arb_leave();                         /* 拿不到锁必须把仲裁配额还回去 */
        return false;
    }
    return true;
}
static void wf_unlock(void)
{
    if (s_io_lock) xSemaphoreGive(s_io_lock);
    sd_arb_leave();
}

/* ---------------- 页面 / 登录 ---------------- */

static esp_err_t h_page(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, wf_page_html(), HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_login(httpd_req_t *req)
{
    /* ★ 2026-11-08 linit 明确要求: 网络访问存储**去掉 PIN**。
     *   同一局域网内打开 http://<设备IP>/files/ 直接可用, 不再要求输入 PIN、
     *   也不再报 nopin（原来"没设 PIN 就不放行"的策略只适用于这条通路, 已按新要求取消）。
     *   token 机制保留（30 分钟 / Bearer 校验）—— 只是不再有"证明你是机主"这一步。
     *   ⚠ 恢复 PIN: 把这里换回 web_core_auth_login(pin, tok, sizeof(tok)) 并重新启用下方 nopin 分支。 */
    char junk[24];
    (void)httpd_req_recv(req, junk, sizeof(junk) - 1);   /* 读掉旧页面可能带的 PIN body, 防 keep-alive 残留 */

    char tok[WEB_CORE_TOKEN_LEN + 1];
    if (!web_core_auth_login_open(tok, sizeof(tok)))
        return web_core_send_err(req, "500 Internal Server Error", "io");
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
    /* ★ 2026-11-24: 交卡窗口里直接拒绝, 不要等下面 sd_is_mounted() 判 ——
     *   那时 VFS 可能刚注销, 报 409 "nomount" 会让前端以为"卡没了"。 */
    if (s_paused) return web_core_send_err(req, "503 Service Unavailable", "busy");
    if (!sd_is_mounted()) return web_core_send_err(req, "409 Conflict", "nomount");
    return ESP_OK;
}

static esp_err_t h_list(httpd_req_t *req)
{
    char raw[WF_PATH_MAX] = { 0 };
    char path[WF_PATH_MAX];
    /* -1 = 参数带了但解码后放不下（见 wf_arg 头注释）。必须明确 400，
     * 不能退回根目录"假装成功" —— 那会让用户以为自己在子目录里。 */
    if (wf_arg(req, "p", raw, sizeof(raw), "X-Path") < 0)
        return web_core_send_err(req, "400 Bad Request", "toobig");
    if (!wf_norm(raw, path, sizeof(path))) return web_core_send_err(req, "400 Bad Request", "path");

    /* 未挂载不是错误：页面要能显示"TF 卡未挂载"，所以照常 200 + mounted:false。 */
    if (!sd_is_mounted()) {
        char out[160];
        snprintf(out, sizeof(out),
                 "{\"ok\":true,\"mounted\":false,\"p\":\"/sdcard\",\"total\":0,\"free\":0,\"more\":0,\"e\":[]}");
        return web_core_send_json(req, out);
    }

    /* ★ 2026-11-24: 列目录同样要过仲裁锁 —— opendir/readdir/stat 都在 VFS 上,
     *   交卡窗口里进来就是踩已释放的 fat_ctx。 */
    if (!wf_lock(2000)) return web_core_send_err(req, "503 Service Unavailable", "busy");

    DIR *d = opendir(path);
    if (!d) {
        wf_unlock();
        return web_core_send_err(req, "404 Not Found", "nodir");
    }

    char *buf = heap_caps_malloc(WF_LIST_BUF, MALLOC_CAP_SPIRAM);
    if (!buf) {
        closedir(d);
        wf_unlock();
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
        /* ★ 2026-11-24: 拼 stat 路径要单独的缓冲。
         *   WF_PATH_MAX(200) 约束的是"客户端传进来的目录路径"，而目录路径本身
         *   就可能接近 200 字节；再拼上最长 255 字节的文件名必然溢出，原来那行
         *   `continue` 会把条目**静默丢掉** —— 表现就是"目录里明明有文件，网页上
         *   却显示空目录"（根目录里的长中文名、以及较深的子目录都会中招）。 */
        char full[WF_PATH_MAX + 256 + 2];
        if (snprintf(full, sizeof(full), "%s/%s", path, nm) >= (int)sizeof(full)) {
            ESP_LOGW(TAG, "条目路径过长，跳过: %s/%s", path, nm);
            continue;
        }
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
    wf_unlock();                      /* 卡上的事做完了, 发送阶段不占 SD 仲裁 */

    esp_err_t e = web_core_send_json(req, buf);
    free(buf);
    return e;
}

/* ---------------- 上传（流式，边收边写） ---------------- */

static esp_err_t h_upload(httpd_req_t *req)
{
    char name[WF_PATH_MAX] = { 0 };
    char rawp[WF_PATH_MAX] = { 0 }, dir[WF_PATH_MAX], target[WF_PATH_MAX];

    if (wf_arg(req, "p", rawp, sizeof(rawp), "X-Path") < 0)
        return web_core_send_err(req, "400 Bad Request", "toobig");
    if (wf_arg(req, "n", name, sizeof(name), "X-Name") != 1)
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

    /* ★ 2026-11-24: 从这里开始全部是文件系统访问 (stat / mkdir / 临时文件),
     *   统一在 SD 仲裁 + 本模块锁之内; 原来 stat 与 wf_mkdirs 都在锁外。 */
    if (!wf_lock(5000)) return web_core_send_err(req, "503 Service Unavailable", "busy");

    struct stat st;
    bool exists = (stat(target, &st) == 0);
    if (exists && S_ISDIR(st.st_mode)) {
        wf_unlock();
        return web_core_send_err(req, "409 Conflict", "isdir");
    }
    char ov[4] = { 0 };
    (void)wf_arg(req, "o", ov, sizeof(ov), "X-Ov");   /* 三态值这里不关心, 只看首字符 */
    if (exists && ov[0] != '1') {
        wf_unlock();
        return web_core_send_err(req, "409 Conflict", "exists");   /* 覆盖必须显式确认 */
    }

    /* 目标目录（含文件夹上传的中间层）先建好 */
    char pdir[WF_PATH_MAX];
    if (wf_parent(target, pdir, sizeof(pdir))) wf_mkdirs(pdir);

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
    s_xfer++;                        /* ★ 标记"有长传输在跑" —— 交卡前要等它退干净 */
    while (got < len) {
        if (s_abort) {               /* 交卡/关闭共享: 立刻收尾, 不再往卡上写 */
            ok = false;
            ESP_LOGW(TAG, "上传被中止(交卡/关闭共享): %lld/%lld", got, len);
            break;
        }
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
    s_xfer--;
    free(buf);
    if (fclose(f) != 0) ok = false;                       /* 收尾失败 = 没落盘 */
    if (ok && got != len) ok = false;

    if (ok) {
        /* ★ 2026-11-22 修「覆盖上传失败 ⇒ 旧文件也没了」:
         *   原来先 remove(target) 再 rename(tmp), rename 一失败, 旧文件已删 + tmp 也被下面删掉
         *   ⇒ 用户上传中断/卡满时**原文件直接消失**。改为: 旧文件先改名成 .bak 占位,
         *   新文件成功落位后再删 .bak; 任何一步失败都把 .bak 改回去。 */
        char bak[WF_PATH_MAX + 8];
        bool had_old = exists;
        if (had_old) {
            snprintf(bak, sizeof(bak), "%s.bak", target);
            remove(bak);                                  /* 上一轮残留 */
            if (rename(target, bak) != 0) {
                ESP_LOGE(TAG, "旧文件改名占位失败: %s", target);
                ok = false;
            }
        }
        if (ok && rename(tmp, target) != 0) {
            ESP_LOGE(TAG, "改名失败: %s → %s", tmp, target);
            if (had_old) rename(bak, target);             /* 回滚: 旧文件放回原位 */
            ok = false;
        }
        if (ok && had_old) remove(bak);
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
    /* ★ 2026-11-24 安全: 这里**不能**用 "X-Path" 头兜底 —— 头里是前端当前的
     *   **目录**(CUR)，而本接口的 p 是具体**文件**路径。query 缺失/超长一旦退回头部，
     *   拿到的就是目录 ⇒ 会退化成 409 isdir（看着像"下载失败"），而不是如实报错。
     *   只有 list/mkdir/upload 的 p 本身就是目录，才该用头部兜底。 */
    if (wf_arg(req, "p", raw, sizeof(raw), NULL) < 0)
        return web_core_send_err(req, "400 Bad Request", "toobig");
    if (!wf_norm(raw, path, sizeof(path))) return web_core_send_err(req, "400 Bad Request", "path");
    esp_err_t g = wf_mount_guard(req);
    if (g != ESP_OK) return g;

    /* ★ 2026-11-24: 下载是全流程持锁的 —— 从 stat 到 fclose 之间卡必须归本机。
     *   原来这里**完全不持锁**, 于是"电脑端一边拖大文件、用户一边点挂载到电脑"
     *   就成了最典型的崩溃场景: 传输线程还在 fread, 另一个任务把 VFS 注销了。 */
    if (!wf_lock(3000)) return web_core_send_err(req, "503 Service Unavailable", "busy");

    struct stat st;
    if (stat(path, &st) != 0) { wf_unlock(); return web_core_send_err(req, "404 Not Found", "notfound"); }
    if (S_ISDIR(st.st_mode)) { wf_unlock(); return web_core_send_err(req, "409 Conflict", "isdir"); }
    if (!S_ISREG(st.st_mode)) { wf_unlock(); return web_core_send_err(req, "400 Bad Request", "plain"); }

    FILE *f = fopen(path, "rb");
    if (!f) { wf_unlock(); return web_core_send_err(req, "500 Internal Server Error", "io"); }

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
        wf_unlock();
        return web_core_send_err(req, "500 Internal Server Error", "mem");
    }

    esp_err_t ret = ESP_OK;
    size_t r;
    s_xfer++;                        /* ★ 长传输标记: 交卡前 web_files_quiesce() 要等它归零 */
    while ((r = fread(buf, 1, WF_TX_CHUNK, f)) > 0) {
        if (s_abort) {               /* 交卡/关闭共享: 立刻停, 让出 SD 卡 */
            ESP_LOGW(TAG, "下载被中止(交卡/关闭共享): %s", path);
            ret = ESP_FAIL;
            break;
        }
        if (httpd_resp_send_chunk(req, buf, r) != ESP_OK) { ret = ESP_FAIL; break; }
    }
    s_xfer--;
    /* ★ 2026-11-22: 区分"读完了"和"读卡出错"。原来只判 fread>0, 卡读错误被当成 EOF,
     *   仍然发结束块 ⇒ 客户端拿到一个**静默截断**的文件却显示下载成功。 */
    bool rd_err = (ferror(f) != 0);
    heap_caps_free(buf);
    fclose(f);
    wf_unlock();                     /* 句柄已关, 卡可以让出 */
    if (ret != ESP_OK) return ret;
    if (rd_err) {
        ESP_LOGE(TAG, "发送中读卡错误, 主动断开不补结束块: %s", path);
        return ESP_FAIL;                                  /* 不发终止块 ⇒ 客户端判为失败 */
    }
    return httpd_resp_send_chunk(req, NULL, 0);          /* 结束 chunked 流 */
}

/* ---------------- 建目录 / 删除 / 改名 ---------------- */

static esp_err_t h_mkdir(httpd_req_t *req)
{
    char rawp[WF_PATH_MAX] = { 0 }, rawn[WF_PATH_MAX] = { 0 };
    char dir[WF_PATH_MAX], target[WF_PATH_MAX];
    if (wf_arg(req, "p", rawp, sizeof(rawp), "X-Path") < 0)
        return web_core_send_err(req, "400 Bad Request", "toobig");
    if (wf_arg(req, "n", rawn, sizeof(rawn), "X-Name") != 1)
        return web_core_send_err(req, "400 Bad Request", "name");
    if (!wf_norm(rawp, dir, sizeof(dir))) return web_core_send_err(req, "400 Bad Request", "path");
    if (!wf_join(dir, rawn, target, sizeof(target)))
        return web_core_send_err(req, "400 Bad Request", "name");
    esp_err_t g = wf_mount_guard(req);
    if (g != ESP_OK) return g;

    if (!wf_lock(3000)) return web_core_send_err(req, "503 Service Unavailable", "busy");
    int rc = mkdir(target, 0777);
    /* 判"已存在"也算文件系统访问, 必须在锁内 —— 否则交卡窗口里 stat 会踩已注销的 VFS */
    struct stat st;
    bool existed = (rc != 0) && (stat(target, &st) == 0);
    wf_unlock();
    if (rc != 0) {
        if (existed) return web_core_send_err(req, "409 Conflict", "exists");
        return web_core_send_err(req, "500 Internal Server Error", "io");
    }
    ESP_LOGI(TAG, "新建目录: %s", target);
    return web_core_send_json(req, "{\"ok\":true}");
}

static esp_err_t h_del(httpd_req_t *req)
{
    char raw[WF_PATH_MAX] = { 0 }, path[WF_PATH_MAX];
    /* ★ 2026-11-24 安全（这条最要紧）: 不能用 "X-Path" 头兜底。
     *   头里是前端当前**目录**(CUR)，p 却是要删的**目标**。query 缺失/超长时若退回头部，
     *   path 就变成当前目录 ⇒ 用户点"删除"一个文件，结果**整个目录被递归删掉**。
     *   宁可 400，也绝不拿"看起来合法"的错路径去删东西。 */
    if (wf_arg(req, "p", raw, sizeof(raw), NULL) < 0)
        return web_core_send_err(req, "400 Bad Request", "toobig");
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
    /* ★ 2026-11-24 安全: p 同样不能用 "X-Path" 头兜底 —— 头里是当前目录，
     *   退回头部会把"给文件改名"变成"给目录改名"（target 落在父目录里）。
     *   n 仍然允许头部兜底（X-Name），它跟目录无关。 */
    if (wf_arg(req, "p", rawp, sizeof(rawp), NULL) < 0)
        return web_core_send_err(req, "400 Bad Request", "toobig");
    if (wf_arg(req, "n", rawn, sizeof(rawn), "X-Name") != 1)
        return web_core_send_err(req, "400 Bad Request", "name");
    if (!wf_norm(rawp, path, sizeof(path))) return web_core_send_err(req, "400 Bad Request", "path");
    if (strchr(rawn, '/')) return web_core_send_err(req, "400 Bad Request", "name");  /* 改名不换目录 */
    if (!wf_parent(path, pdir, sizeof(pdir))) return web_core_send_err(req, "400 Bad Request", "path");
    if (!wf_join(pdir, rawn, target, sizeof(target)))
        return web_core_send_err(req, "400 Bad Request", "name");
    esp_err_t g = wf_mount_guard(req);
    if (g != ESP_OK) return g;

    /* ★ 2026-11-24: 两次 stat 也必须在锁内 —— 它们同样是 VFS 访问 */
    if (!wf_lock(3000)) return web_core_send_err(req, "503 Service Unavailable", "busy");
    struct stat st;
    if (stat(path, &st) != 0) { wf_unlock(); return web_core_send_err(req, "404 Not Found", "notfound"); }
    if (!strcmp(path, target)) { wf_unlock(); return web_core_send_json(req, "{\"ok\":true}"); }
    if (stat(target, &st) == 0) { wf_unlock(); return web_core_send_err(req, "409 Conflict", "exists"); }

    int rc = rename(path, target);
    wf_unlock();
    if (rc != 0) return web_core_send_err(req, "500 Internal Server Error", "io");
    ESP_LOGI(TAG, "改名: %s → %s", path, target);
    return web_core_send_json(req, "{\"ok\":true}");
}

/* ---------------- 分发 ---------------- */

static esp_err_t h_api(httpd_req_t *req)
{
    /* ★ 活动保活: 任何文件管理请求都算"用户在用"。用 esp_timer 而非 os_millis,
     *   避免 services→system 新增依赖; 纯内存读, 屏保任务也可安全查询。 */
    s_last_access_ms = (uint32_t)(esp_timer_get_time() / 1000);

    /* ★★★ 2026-11-24 根因修复：「浏览器打开 /files/ 看不到 TF 卡任何内容」★★★
     *
     *   原来: strstr(req->uri, "/api/") + 5, 然后 `strcmp(p, "list")` 精确比较。
     *   但 IDF 的 req->uri 是**整条请求目标**（esp_http_server.h:
     *   "The URI of this request"），`?...` 查询串也在里面；httpd_uri() 只是用
     *   url_parse_res 的 UF_PATH **段**去做路由匹配，并不会把 req->uri 截成纯路径。
     *
     *   于是 p 实际是 "list?p=%2Fsdcard"，跟 "list" 永不相等 ⇒ **每一个带参数的
     *   API 都掉到末尾的 404 "notfound"**：列表空、进不去子目录、下载/改名/删除全废，
     *   前端 load() 只会把"文件不存在"打在提示行上。这就是"看不到卡内容"的真因。
     *
     *   （同一个坑 page_fav.c:1881 早就踩过并留了注释："通配匹配时 req->uri 可能带
     *   查询串(?pin=...), 必须先截断再比较"。这里与它对齐。）
     *
     *   修法：先按 '?' 截出路径段，只在路径段内找 "/api/"，再按长度精确比对。
     */
    size_t plen = strcspn(req->uri, "?");           /* 路径段长度，不含 ?query */
    const char *at = strstr(req->uri, "/api/");
    if (!at || (size_t)(at - req->uri) + 5 > plen)
        return web_core_send_err(req, "404 Not Found", "notfound");
    at += 5;

    size_t nlen = plen - (size_t)(at - req->uri);
    char ep[24];                                    /* 端点名都很短(download 最长) */
    if (nlen == 0 || nlen >= sizeof(ep))
        return web_core_send_err(req, "404 Not Found", "notfound");
    memcpy(ep, at, nlen);
    ep[nlen] = 0;

    if (!strcmp(ep, "list"))     return h_list(req);
    if (!strcmp(ep, "upload"))   return h_upload(req);
    if (!strcmp(ep, "download")) return h_download(req);
    if (!strcmp(ep, "mkdir"))    return h_mkdir(req);
    if (!strcmp(ep, "del"))      return h_del(req);
    if (!strcmp(ep, "rename"))   return h_rename(req);
    ESP_LOGW(TAG, "未知接口: %s", req->uri);
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
    s_paused = false;      /* ★ 恢复受理 (交卡期间可能被 web_files_quiesce 暂停过) */
    s_abort  = false;

    /* ★★★ 2026-11-01 linit 定调：这条功能是**局域网共享**，不开热点 ★★★
     *   使用方式 = 掌机连上家里的 WiFi(STA)，电脑/手机在同一网络下访问 http://<设备IP>/files/
     *   原来"没连 WiFi 就自动开热点 8.8.8.8"那条路**用户明确不要**，已删除。
     *   顺带解决了必须先有 lwIP 才能起 httpd 的顺序问题（见下方说明）：
     *   未连接就直接失败返回，绝不在没有协议栈的情况下调 httpd_start。 */
    if (!wifi_manager_is_connected()) {
        ESP_LOGE(TAG, "WiFi 未连接 ⇒ 局域网共享需要设备先连上 WiFi(STA)");
        return ESP_FAIL;
    }

    /* ★ 2026-11-08: h_login 已走免 PIN 分支（web_core_auth_login_open），这里注入的
     *   wf_pin_check 只作为"恢复 PIN"时的现成钩子，正常流程不再被调用。 */
    web_core_auth_set_pin_check(wf_pin_check);

    esp_err_t e = web_core_start(0);          /* 此时 lwIP 已随 STA 连接就绪 */
    if (e != ESP_OK) return e;
    e = web_core_register(&s_page_app);
    if (e != ESP_OK) goto fail_page;
    e = web_core_register(&s_api_app);
    if (e != ESP_OK) goto fail_api;

    /* ★ 2026-11-07 linit「统一极致省电」: 原来这里 wifi_manager_ps_hold(true) 关省电。
     *   新策略下网页服务不再有切档权 —— 局域网传文件的响应会略慢, 但不许再全醒。 */
    s_running = true;
    wf_auto_save(true);       /* ★ 记住"用户开过共享": 异常重启后由开机任务自动恢复 */

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
    /* ★ 2026-11-24: 先把正在跑的上传/下载收掉 —— web_core_stop() 会等 httpd 任务退出,
     *   而 handler 若卡在 fread/fwrite 上是等不出来的(甚至已经把卡交出去了)。
     *   收不掉也要继续关服务(否则界面永远关不掉共享), 只记一条日志。 */
    if (!web_files_quiesce(3000))
        ESP_LOGW(TAG, "关闭共享时仍有传输未收尾 (已强制继续)");
    s_running = false;
    web_core_unregister(&s_api_app);
    web_core_unregister(&s_page_app);
    web_core_stop();
    if (!web_core_is_running()) web_core_auth_clear();   /* 服务真停了才作废 token */
    if (s_ap_ours) { wifi_manager_ap_stop(); s_ap_ours = false; }
    wf_auto_save(false);        /* ★ 用户主动关闭 ⇒ 重启后不再自动拉起 */
    ESP_LOGI(TAG, "TF 卡共享已关闭");
}

/* ═══════════════════════════════════════════════════════════════════════
 * 交卡前清场 / 收卡后恢复
 *
 * USB MSC 接管 TF 卡时必须保证: 没有任何 HTTP handler 还握着 /sdcard 上的
 * 文件句柄(FILE 与 DIR)。短 handler(列目录/建目录/删除/改名)由 sd_arb 的在途计数兜住;
 * 长传输(上传/下载)必须显式打断 —— 它们一跑就是几十秒, 等在途计数归零等于死等。
 * ═══════════════════════════════════════════════════════════════════════ */
bool web_files_quiesce(uint32_t timeout_ms)
{
    s_paused = true;          /* 不再受理新请求 (返回 503 busy) */
    s_abort  = true;          /* 让正在跑的传输在下一块收尾 */
    TickType_t t0 = xTaskGetTickCount();
    while (s_xfer > 0) {
        if (timeout_ms == 0 ||
            (uint32_t)(xTaskGetTickCount() - t0) >= pdMS_TO_TICKS(timeout_ms)) {
            ESP_LOGW(TAG, "quiesce 超时: 仍有 %d 个传输未收尾", s_xfer);
            s_abort = false;
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    s_abort = false;
    return true;
}

void web_files_resume(void)
{
    s_abort  = false;
    s_paused = false;
    ESP_LOGI(TAG, "已恢复受理 (SD 卡回到本机)");
}

/* ---- 开机自动恢复: 用户上次是"开着共享"关机的, 连上 WiFi 后自动拉起 ----
 * 原来完全没有持久化: s_running 是静态变量, 一重启就 false, 于是
 * "设备异常重启后网页服务再也没起来" —— 不是启动失败, 是**根本没人再启动它**
 * (唯一入口是存储页那个按钮)。 */
#define WF_NVS_NS    "webcfg"
#define WF_NVS_AUTO  "auto"

static void wf_auto_save(bool on)
{
    nvs_handle_t h;
    if (nvs_open(WF_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, WF_NVS_AUTO, on ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

static bool wf_auto_load(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(WF_NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    esp_err_t e = nvs_get_u8(h, WF_NVS_AUTO, &v);
    nvs_close(h);
    return (e == ESP_OK && v == 1);
}

static void wf_boot_task(void *arg)
{
    (void)arg;
    /* 等 STA 连上 (最长 90s); 期间用户手动关掉共享则放弃 */
    for (int i = 0; i < 180; i++) {
        if (!wf_auto_load()) { vTaskDelete(NULL); return; }
        if (wifi_manager_is_connected()) {
            esp_err_t e = web_files_start();
            ESP_LOGI(TAG, "[开机恢复] 网络访问存储 %s", (e == ESP_OK) ? "已自动恢复" : "恢复失败");
            vTaskDelete(NULL);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGW(TAG, "[开机恢复] 90s 内未连上 WiFi ⇒ 不自动开启共享");
    vTaskDelete(NULL);
}

void web_files_boot_restore(void)
{
    if (web_files_is_running()) return;
    if (!wf_auto_load()) return;
    if (xTaskCreate(wf_boot_task, "wf_boot", 4096, NULL, 2, NULL) != pdPASS)
        ESP_LOGW(TAG, "[开机恢复] 任务创建失败");
}

bool web_files_is_running(void) { return s_running; }

/* ★ 2026-11-25: 传输中 = 有上传/下载流在跑。
 * 直接复用 h_upload / h_download 已经在维护的 s_xfer（交卡前的 quiesce 也靠它），
 * 不新建第二套状态: 两处口径必须一致，否则会出现"quiesce 认为还在传、省电认为没事"。
 * 不再额外判 s_running: 服务没跑时 s_xfer 必然为 0，多一个判据只会多一种不同步的可能。 */
bool web_files_busy(void) { return s_xfer > 0; }

/* ★ 2026-11-25: "有用户在用网页文件共享"的保活判据。
 * 与 busy 的区别: busy 只看正在跑的上传/下载流(交卡同步用, 必须精确);
 *   active 额外把"最近 30s 内有过任何 API 请求(列目录/建目录/删除/改名...)"也算进来 ——
 *   浏览器翻目录时每个请求之间可能停顿, 只看 s_xfer 会被省电误判成闲置。
 * 只做时间戳比较, 纯内存读, 屏保监督任务(PSRAM 栈)可安全调用。
 * 服务没跑(s_last_access_ms==0)时恒 false, 不会长期阻止省电。 */
bool web_files_active(void)
{
    if (s_last_access_ms == 0) return false;
    if (s_xfer > 0) return true;
    return (uint32_t)(esp_timer_get_time() / 1000) - s_last_access_ms < WF_ACTIVE_WINDOW_MS;
}

bool web_files_get_addr(char *out, size_t cap)
{
    if (!out || cap == 0) return false;
    out[0] = 0;
    if (s_ap_ours) { snprintf(out, cap, "8.8.8.8"); return true; }
    return wifi_manager_get_ip(out, cap);
}
