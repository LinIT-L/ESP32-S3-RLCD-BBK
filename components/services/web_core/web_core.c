/**
 * web_core.c — 全系统唯一的 HTTP Server 管理者（实现见 web_core.h 的设计说明）。
 *
 * 结构：
 *   s_srv        唯一 httpd 句柄
 *   s_slots[]    所有已注册 URI 的落地副本（URI 字符串 + 原 handler + 是否要鉴权）
 *   s_apps[]     应用注册表（name/prefix/默认应用），用于注销与 "/" 跳转
 *   所有 URI 都注册成同一个 trampoline，靠 user_ctx 找回各自的 handler ——
 *   这样"要不要鉴权"是**按应用**判的，不需要给每个 handler 包一层。
 */
#include "web_core.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_attr.h"          /* EXT_RAM_BSS_ATTR */

static const char *TAG = "WEBCORE";

/* ---------------- 内部结构 ---------------- */

typedef struct {
    char        uri[WEB_CORE_URI_MAX];  /* 拼接后的完整 URI（httpd 只存指针，必须常驻） */
    httpd_uri_t orig;                   /* 原 handler / method */
    bool        need_auth;
    bool        used;
} web_slot_t;

typedef struct {
    bool        used;
    char        name[16];
    char        prefix[24];
    bool        is_default;
    size_t      n_uris;
    web_slot_t  slots[WEB_CORE_MAX_URIS];
} web_app_slot_t;

static httpd_handle_t s_srv = NULL;
static uint16_t       s_port = 80;
static int            s_ref = 0;          /* 引用计数 */

/* ★ 2026-11-02: s_apps **整体搬到 PSRAM**。
 *   它就是一张注册表（8 应用 × 12 URI × 92B = 9,216 B）, 只在注册/注销/转发时被读,
 *   不参与任何时序, 放内部 RAM 白白吃掉 9K —— 这正是「核心内存莫名少了 10K」的来源
 *   (第一回合做 web_core 时它被当成普通静态数组, 落在 .dram0.bss)。
 *   注意: s_srv / s_tok / s_pin_check 留在内部 RAM —— 认证路径会被高频读, 且体积极小。 */
static EXT_RAM_BSS_ATTR web_app_slot_t s_apps[WEB_CORE_MAX_APPS];

/* 认证 */
static char               s_tok[WEB_CORE_TOKEN_LEN + 1];
static int64_t            s_tok_us = 0;
static web_core_pin_check_t s_pin_check = NULL;

/* ---------------- 小工具 ---------------- */

static void join_uri(char *dst, size_t cap, const char *prefix, const char *rel)
{
    if (!dst || cap == 0) return;
    dst[0] = '\0';
    if (!prefix || !prefix[0]) {
        snprintf(dst, cap, "%s", rel ? rel : "/");
        return;
    }
    if (!rel || !rel[0]) { snprintf(dst, cap, "%s", prefix); return; }
    if (rel[0] == '/') snprintf(dst, cap, "%s%s", prefix, rel);
    else               snprintf(dst, cap, "%s/%s", prefix, rel);
}

/* 所有 URI 的统一入口：先按应用判鉴权，再转给真正的 handler。 */
static esp_err_t core_trampoline(httpd_req_t *req)
{
    web_slot_t *s = (web_slot_t *)req->user_ctx;
    /* ★★★ 2026-11-01 修「关共享/退应用时闪退」★★★
     *   unregister / stop 会把槽位 memset 清零，而此时 httpd 工作线程可能**正在处理**
     *   一个已经派发进来的请求 —— 它的 user_ctx 还是这个槽位的地址（地址有效、内容已清空）。
     *   原来只判 `!s`，于是 s->orig.handler 为 NULL 时直接调用 ⇒ 空指针 panic。
     *   这里三道都判：槽位在不在、有没有被注销、handler 有没有被清空。 */
    if (!s || !s->used || !s->orig.handler) {
        ESP_LOGW(TAG, "user_ctx 失效(服务已注销?): %s", req->uri ? req->uri : "?");
        return web_core_send_err(req, "503 Service Unavailable", "gone");
    }
    if (s->need_auth && !web_core_auth_check(req))
        return web_core_send_err(req, "401 Unauthorized", "auth");
    return s->orig.handler(req);
}

/* 前缀本身（如 "/pad"）→ 补斜杠跳转。
 * 为什么需要：页面里的相对路径（fetch('joypad')）只有"带斜杠的目录 URL"才解析正确；
 * 用户手输 /pad 时若不跳转，相对请求会打到根下的 /joypad ⇒ 404。 */
static esp_err_t core_prefix_redirect(httpd_req_t *req)
{
    char loc[WEB_CORE_URI_MAX + 2];
    size_t n = strlen(req->uri);
    if (n > WEB_CORE_URI_MAX) n = WEB_CORE_URI_MAX;   /* 有意截断: 超长 URI 本来就进不来 */
    memcpy(loc, req->uri, n);
    loc[n]     = '/';
    loc[n + 1] = '\0';
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", loc);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, "redirect");
}

static esp_err_t core_root_handler(httpd_req_t *req)
{
    /* 优先跳到标记 is_default 的应用；没有就退到"第一个注册的应用"，
     * 这样只开收藏夹时访问 http://设备IP/ 也直接进 /fav/，不留白页。 */
    int pick = -1;
    for (int i = 0; i < WEB_CORE_MAX_APPS; i++) {
        if (!s_apps[i].used || !s_apps[i].prefix[0]) continue;
        if (s_apps[i].is_default) { pick = i; break; }
        if (pick < 0) pick = i;
    }
    if (pick >= 0) {
        char loc[WEB_CORE_URI_MAX];
        snprintf(loc, sizeof(loc), "%s/", s_apps[pick].prefix);
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", loc);
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        return httpd_resp_sendstr(req, "redirect");
    }
    /* 没有任何默认应用：给一句人话，别让浏览器白屏 */
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_sendstr(req, "<meta charset='utf-8'><h3>LinTOS</h3>"
                                   "<p>网页服务已就绪，但没有注册默认页面。</p>");
}

/* ---------------- 生命周期 ---------------- */

esp_err_t web_core_start(uint16_t port)
{
    if (s_srv) {                 /* 已启动：只加引用 */
        s_ref++;
        ESP_LOGI(TAG, "已在运行 (port=%u, ref=%d)", (unsigned)s_port, s_ref);
        return ESP_OK;
    }

    /* ★ 集中配置：全系统只有这一处 httpd_start。
     *   · 栈 8192：与旧收藏夹一致（默认 4096 起不来）
     *   · uri_match_fn=wildcard：支持末尾星号通配（收藏夹的 api 段与各应用兜底都要）
     *   · lru_purge：socket 用满时踢掉最久未用的，避免浏览器连不上
     *   · max_open_sockets 6：LWIP 总共 10 个，给客户端连接留余量 */
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port        = port ? port : 80;
    cfg.max_uri_handlers   = 24;
    cfg.max_open_sockets   = 6;
    cfg.stack_size         = 8192;
    cfg.lru_purge_enable   = true;
    cfg.uri_match_fn       = httpd_uri_match_wildcard;

    esp_err_t e = httpd_start(&s_srv, &cfg);
    if (e != ESP_OK) {
        s_srv = NULL;
        ESP_LOGE(TAG, "httpd_start 失败: %s (port=%u)", esp_err_to_name(e), (unsigned)cfg.server_port);
        return e;
    }
    s_port = cfg.server_port;
    s_ref  = 1;

    static const httpd_uri_t root_uri = {
        .uri = "/", .method = HTTP_GET, .handler = core_root_handler, .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_srv, &root_uri);

    ESP_LOGI(TAG, "HTTP 服务已启动: port=%u handlers=%d socks=%d",
             (unsigned)s_port, cfg.max_uri_handlers, cfg.max_open_sockets);
    return ESP_OK;
}

void web_core_stop(void)
{
    if (!s_srv) return;
    if (s_ref > 1) { s_ref--; ESP_LOGI(TAG, "引用 -1 (剩 %d)", s_ref); return; }
    s_ref = 0;
    /* ★★★ 2026-11-01 顺序修正：「先停服务，再清槽位」★★★
     *   原来是先 memset 槽位、后 httpd_stop ⇒ 中间那段时间 httpd 仍在跑，
     *   在途请求的 user_ctx 指向已被清零的槽位 ⇒ handler 为 NULL ⇒ panic。
     *   httpd_stop() 会关闭监听、等已派发的请求处理完，之后再清才不会再有人引用。 */
    httpd_stop(s_srv);
    s_srv = NULL;
    for (int i = 0; i < WEB_CORE_MAX_APPS; i++) {
        if (!s_apps[i].used) continue;
        memset(&s_apps[i], 0, sizeof(s_apps[i]));
    }
    web_core_auth_clear();
    ESP_LOGI(TAG, "HTTP 服务已停止");
}

bool           web_core_is_running(void) { return s_srv != NULL; }
uint16_t       web_core_port(void)       { return s_port; }
httpd_handle_t web_core_handle(void)     { return s_srv; }

/* ---------------- 应用注册 ---------------- */

esp_err_t web_core_register(const web_app_t *app)
{
    if (!app || !app->name) return ESP_ERR_INVALID_ARG;
    if (!s_srv) {
        ESP_LOGE(TAG, "注册 %s 失败: 服务未启动（先 web_core_start）", app->name);
        return ESP_ERR_INVALID_STATE;
    }
    if (app->n_uris > WEB_CORE_MAX_URIS) return ESP_ERR_INVALID_SIZE;

    /* 同名重复注册 → 视为已注册（幂等），避免重复进页面时越注册越多 */
    for (int i = 0; i < WEB_CORE_MAX_APPS; i++) {
        if (s_apps[i].used && strncmp(s_apps[i].name, app->name, sizeof(s_apps[i].name) - 1) == 0) {
            ESP_LOGW(TAG, "%s 已注册，忽略重复注册", app->name);
            return ESP_OK;
        }
    }

    int slot = -1;
    for (int i = 0; i < WEB_CORE_MAX_APPS; i++) if (!s_apps[i].used) { slot = i; break; }
    if (slot < 0) {
        ESP_LOGE(TAG, "应用槽已满 (%d)", WEB_CORE_MAX_APPS);
        return ESP_ERR_NO_MEM;
    }

    web_app_slot_t *a = &s_apps[slot];
    memset(a, 0, sizeof(*a));
    a->used       = true;
    a->is_default = app->is_default;
    a->n_uris     = app->n_uris;
    snprintf(a->name, sizeof(a->name), "%s", app->name);
    if (app->prefix) snprintf(a->prefix, sizeof(a->prefix), "%s", app->prefix);

    for (size_t k = 0; k < app->n_uris; k++) {
        web_slot_t *s = &a->slots[k];
        join_uri(s->uri, sizeof(s->uri), a->prefix, app->uris[k].uri);
        s->orig      = app->uris[k];
        s->need_auth = app->need_auth;
        s->used      = true;

        httpd_uri_t reg = s->orig;
        reg.uri     = s->uri;
        reg.handler = core_trampoline;
        reg.user_ctx = s;
        esp_err_t e = httpd_register_uri_handler(s_srv, &reg);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "%s 注册 %s 失败: %s", app->name, s->uri, esp_err_to_name(e));
            /* 回滚本应用已注册的 URI，保持"要么全有要么全无" */
            for (size_t j = 0; j < k; j++) {
                if (a->slots[j].used)
                    httpd_unregister_uri_handler(s_srv, a->slots[j].uri, a->slots[j].orig.method);
            }
            memset(a, 0, sizeof(*a));
            return e;
        }
        ESP_LOGI(TAG, "%s → %s", app->name, s->uri);
    }

    /* 额外注册"裸前缀"（/pad）→ 302 到 /pad/，保证页面内相对路径可用。
     * 只有注册了 "/" 的应用才需要（那是它的入口页）。 */
    if (a->prefix[0] && a->n_uris < WEB_CORE_MAX_URIS) {
        bool has_index = false;
        for (size_t k = 0; k < a->n_uris; k++)
            if (strcmp(a->slots[k].orig.uri, "/") == 0) { has_index = true; break; }
        if (has_index) {
            web_slot_t *r = &a->slots[a->n_uris];
            size_t pn = strlen(a->prefix);
            if (pn >= sizeof(r->uri)) pn = sizeof(r->uri) - 1;
            memcpy(r->uri, a->prefix, pn);
            r->uri[pn] = 0;
            r->orig.uri      = r->uri;
            r->orig.method   = HTTP_GET;
            r->orig.handler  = core_prefix_redirect;
            r->orig.user_ctx = NULL;
            r->need_auth     = app->need_auth;
            r->used          = true;
            httpd_uri_t reg = r->orig;
            reg.uri = r->uri; reg.handler = core_trampoline; reg.user_ctx = r;
            if (httpd_register_uri_handler(s_srv, &reg) == ESP_OK) {
                a->n_uris++;
                ESP_LOGI(TAG, "%s → %s (补斜杠跳转)", app->name, r->uri);
            } else {
                memset(r, 0, sizeof(*r));
            }
        }
    }
    return ESP_OK;
}

esp_err_t web_core_unregister(const web_app_t *app)
{
    if (!app || !app->name) return ESP_ERR_INVALID_ARG;
    for (int i = 0; i < WEB_CORE_MAX_APPS; i++) {
        if (!s_apps[i].used || strncmp(s_apps[i].name, app->name, sizeof(s_apps[i].name) - 1) != 0)
            continue;
        if (s_srv) {
            for (size_t k = 0; k < s_apps[i].n_uris; k++) {
                web_slot_t *s = &s_apps[i].slots[k];
                if (s->used) httpd_unregister_uri_handler(s_srv, s->uri, s->orig.method);
            }
        }
        memset(&s_apps[i], 0, sizeof(s_apps[i]));
        ESP_LOGI(TAG, "%s 已注销", app->name);
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

/* ---------------- 认证 ---------------- */

void web_core_auth_set_pin_check(web_core_pin_check_t fn) { s_pin_check = fn; }

void web_core_auth_clear(void)
{
    memset(s_tok, 0, sizeof(s_tok));
    s_tok_us = 0;
}

bool web_core_auth_login(const char *pin, char *tok_out, size_t cap)
{
    if (!pin || !pin[0] || !s_pin_check) return false;
    if (!s_pin_check(pin)) {
        ESP_LOGW(TAG, "PIN 校验失败");
        return false;
    }
    /* 16 个 hex 字符的随机 token（与收藏夹现有长度一致） */
    static const char hex[] = "0123456789abcdef";
    uint8_t rnd[WEB_CORE_TOKEN_LEN / 2];
    esp_fill_random(rnd, sizeof(rnd));
    for (int i = 0; i < (int)sizeof(rnd); i++) {
        s_tok[i * 2]     = hex[(rnd[i] >> 4) & 0xF];
        s_tok[i * 2 + 1] = hex[rnd[i] & 0xF];
    }
    s_tok[WEB_CORE_TOKEN_LEN] = '\0';
    s_tok_us = esp_timer_get_time();
    if (tok_out && cap > WEB_CORE_TOKEN_LEN)
        snprintf(tok_out, cap, "%s", s_tok);
    ESP_LOGI(TAG, "签发网页 token（%d 分钟有效）", WEB_CORE_TOKEN_TTL_SEC / 60);
    return true;
}

bool web_core_auth_check(httpd_req_t *req)
{
    if (!s_tok[0]) return false;
    if (esp_timer_get_time() - s_tok_us > (int64_t)WEB_CORE_TOKEN_TTL_SEC * 1000000LL) return false;
    char hv[64];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hv, sizeof(hv)) != ESP_OK) return false;
    const char *b = hv;
    if (strncmp(b, "Bearer ", 7) == 0)      b += 7;
    else if (strncmp(b, "bearer ", 7) == 0) b += 7;
    return strcmp(b, s_tok) == 0;
}

/* ---------------- 响应小工具 ---------------- */

esp_err_t web_core_send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json ? json : "{}");
}

esp_err_t web_core_send_err(httpd_req_t *req, const char *status_line, const char *code)
{
    char body[64];
    snprintf(body, sizeof(body), "{\"ok\":false,\"err\":\"%s\"}", code ? code : "err");
    if (status_line) httpd_resp_set_status(req, status_line);
    return web_core_send_json(req, body);
}
