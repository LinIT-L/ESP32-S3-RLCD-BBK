/**
 * web_core.h — 全系统唯一的 HTTP Server 管理者。
 *
 * 为什么存在（2026-11-01）：
 *   改造前有三个模块各自 httpd_start()：收藏夹（page_fav.c）、密钥管理器
 *   （page_keymanager.c）、网页手柄（web_gamepad.c），全都用 HTTPD_DEFAULT_CONFIG
 *   ⇒ 都是 80 端口，谁先起谁占，第二个必失败；而且各模块各管生命周期，
 *   退出一个可能把别人的服务停掉，加新网页功能只能再起一个 server。
 *
 * 现在：
 *   · 全系统**只有一个** httpd 实例，由本模块持有；端口、栈、socket 数集中配置。
 *   · 每个网页应用注册一个**前缀**（/pad、/fav、/keys、/files），URI 由本模块
 *     拼接后注册；应用只提供相对路径，不再关心端口与生命周期。
 *   · 引用计数：多个应用同时在线时，任何一个 stop 都不会把服务关掉。
 *   · 公共认证：PIN→随机 Token→Bearer，供第二回合的收藏夹/密钥管理器共用。
 *
 * ⚠ URI 匹配用 httpd_uri_match_wildcard（ESP-IDF 只支持**末尾** `*`），
 *   所以前缀是精确拼接（"/pad" + "/joypad" → "/pad/joypad"），不依赖前缀通配。
 */
#ifndef WEB_CORE_H
#define WEB_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WEB_CORE_MAX_APPS       8     /* 同时在线的网页应用数 */
#define WEB_CORE_MAX_URIS       12    /* 单个应用最多注册的 URI 数 */
#define WEB_CORE_URI_MAX        72    /* 拼接后的完整 URI 上限 */
#define WEB_CORE_TOKEN_LEN      16    /* token 字符数（hex） */
#define WEB_CORE_TOKEN_TTL_SEC  (30 * 60)   /* 与收藏夹现有 30 分钟一致 */

/* 一个网页应用 = 一个前缀 + 若干相对 URI。 */
typedef struct {
    const char         *name;    /* 日志用短名: "pad" / "fav" / "files" */
    const char         *prefix;  /* "/pad"; NULL 或 "" = 直接注册到根 */
    const httpd_uri_t  *uris;    /* uri 字段写**相对**路径: "/" "/joypad" */
    size_t              n_uris;
    bool                need_auth;  /* true = 该应用所有 URI 先过 Bearer 校验 */
    bool                is_default; /* true = 访问 "/" 时 302 到该应用前缀 */
} web_app_t;

/* ---------- 服务生命周期（引用计数） ---------- */

/* 启动（幂等）。已经启动时只把引用计数 +1，不会再起第二个 server。
 * port 传 0 用默认 80。返回 ESP_OK 表示"服务可用"。 */
esp_err_t web_core_start(uint16_t port);

/* 引用计数 -1；归零才真正 httpd_stop()。
 * ⚠ 应用退出必须调用本函数（而不是直接 httpd_stop），否则会误关别人的服务。 */
void web_core_stop(void);

bool           web_core_is_running(void);
uint16_t       web_core_port(void);
httpd_handle_t web_core_handle(void);   /* 给特殊需求用（正常注册走 register） */

/* ---------- 应用注册 ---------- */

/* 注册一个网页应用。prefix 与 uri 拼接后逐个 httpd_register_uri_handler()。 */
esp_err_t web_core_register(const web_app_t *app);

/* 注销（按 name 匹配，URI 与前缀必须和注册时一致）。 */
esp_err_t web_core_unregister(const web_app_t *app);

/* ---------- 公共认证（PIN → Token → Bearer） ---------- */

/* 由业务层注入 PIN 校验函数（收藏夹用 keyvault_verify_pin）。
 * 未注入时 login 一律失败，不会出现"没配 PIN 就放行"。 */
typedef bool (*web_core_pin_check_t)(const char *pin);
void web_core_auth_set_pin_check(web_core_pin_check_t fn);

/* 校验 PIN 并签发 token（写进 tok_out，含结尾 NUL）。成功返回 true。 */
bool web_core_auth_login(const char *pin, char *tok_out, size_t cap);

/* 校验请求头 `Authorization: Bearer <token>`（也接受小写 bearer）。
 * token 为空 / 过期（30 分钟）/ 不匹配 都返回 false。 */
bool web_core_auth_check(httpd_req_t *req);

/* 作废当前 token（应用退出 / 用户点"断开"时调用）。 */
void web_core_auth_clear(void);

/* ---------- 统一响应小工具（状态码与 JSON 格式全系统一致） ---------- */

esp_err_t web_core_send_json(httpd_req_t *req, const char *json);

/* status_line 形如 "400 Bad Request"；code 是短错误码（"body"/"auth"/"io"）。
 * 返回 {"ok":false,"err":"<code>"} */
esp_err_t web_core_send_err(httpd_req_t *req, const char *status_line, const char *code);

#ifdef __cplusplus
}
#endif

#endif /* WEB_CORE_H */
