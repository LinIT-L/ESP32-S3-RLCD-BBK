/* luat_lib_net.c — LinTOS Lua 桥 · net 模块
 * 拆分自 components/apps/pages/page_lua.c (2026-10-02 模块化, 对标 LuatOS 的 luat_lib_*.c)。
 * 契约: 只通过 lua_setfield(L, -2, "名字") 往**栈顶的 app 表**写字段;
 *       需要子表时自己 lua_newtable(L) → 填 → lua_setfield(L, -2, "子表名")。
 * 加新 API: 本文件写 static int l_xxx(lua_State *L), 再在文件末尾的 register 里加一行。
 */
#include "lua_bridge.h"


/* ---- app.net.* : 网络状态 / HTTP / ping ---- */
static int l_net_is_connected(lua_State *L) {
    lua_pushboolean(L, wifi_manager_is_connected());
    return 1;
}

static int l_net_get_ip(lua_State *L) {
    char ip[32];
    if (wifi_manager_get_ip(ip, sizeof(ip))) { lua_pushstring(L, ip); return 1; }
    return 0;
}

static int l_net_http_get(lua_State *L) {
    const char *host = luaL_checkstring(L, 1);
    int port = (int)luaL_optinteger(L, 2, 80);
    const char *path = luaL_optstring(L, 3, "/");
    int timeout = (int)luaL_optinteger(L, 4, 5000);
    char url[320];
    snprintf(url, sizeof(url), "http://%s:%d%s", host, port, path);
    esp_http_client_config_t cfg = {
        .url = url, .timeout_ms = timeout,
        .buffer_size = 2048, .disable_auto_redirect = false,
    };
    esp_http_client_handle_t cl = esp_http_client_init(&cfg);
    if (!cl) return 0;
    esp_err_t e = esp_http_client_perform(cl);
    if (e == ESP_OK) {
        /* ★ 2026-10-02: 4KB 缓冲搬 PSRAM —— 纯 socket 读(无 DMA/无禁 cache 窗口),
         *   留在内部 RAM 纯属浪费(本项目内部 RAM 最紧张, 见 docs/内部RAM预算…) */
        EXT_RAM_BSS_ATTR static char body[4097];
        int n = esp_http_client_read_response(cl, body, sizeof(body) - 1);
        if (n > 0) { body[n] = 0; lua_pushstring(L, body); esp_http_client_cleanup(cl); return 1; }
    }
    esp_http_client_cleanup(cl);
    return 0;
}

/* ---- app.net.ping(): 逐次 ICMP echo, 每次一个独立完整会话(count=1, 等 on_ping_end 才删会话) ---- */
typedef struct { volatile int done; volatile int ok; volatile uint32_t rtt; } lua_ping_ctx_t;
static lua_ping_ctx_t s_ping_ctx;
static void l_ping_succ(esp_ping_handle_t h, void *arg) {
    (void)arg;
    uint32_t gap = 0;
    if (esp_ping_get_profile(h, ESP_PING_PROF_TIMEGAP, &gap, sizeof(gap)) == ESP_OK) s_ping_ctx.rtt = gap;
    s_ping_ctx.ok = 1;
}

static void l_ping_end(esp_ping_handle_t h, void *arg) {
    (void)h; (void)arg; s_ping_ctx.done = 1;
}


/* ==== V1.2 新增: 局域网扫描 (netscan 服务) —— 解锁 netdect 类应用 ==== */
static int l_net_port(lua_State *L) {          /* app.net.port(ip, port[, ms]) -> bool */
    const char *ip = luaL_checkstring(L, 1);
    int port = (int)luaL_checkinteger(L, 2);
    int ms   = (int)luaL_optinteger(L, 3, 300);
    lua_pushboolean(L, netscan_port_probe(ip, (uint16_t)port, ms) == 1);
    return 1;
}

static void l_mac_str(const uint8_t m[6], char *out) {
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
}

static int l_net_arp(lua_State *L) {           /* app.net.arp(ip) -> "AA:BB:.." 或 nil */
    const char *ip = luaL_checkstring(L, 1);
    uint8_t m[6];
    if (!netscan_resolve_mac(ip, m)) return 0;
    char s[18]; l_mac_str(m, s);
    lua_pushstring(L, s);
    return 1;
}

static int l_net_arp_list(lua_State *L) {      /* app.net.arp_list() -> {{ip=,mac=},...} */
    static char    ips[32][16];
    static uint8_t macs[32][6];
    int n = netscan_arp_dump(ips, macs, 32);
    lua_newtable(L);
    for (int i = 0; i < n; i++) {
        char s[18]; l_mac_str(macs[i], s);
        lua_newtable(L);
        lua_pushstring(L, ips[i]); lua_setfield(L, -2, "ip");
        lua_pushstring(L, s);      lua_setfield(L, -2, "mac");
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

/* ==== 本机网络信息 / MAC 厂商 (解锁 netdect: 网络 & 信息两页要的就是这一组) ==== */
/* 与 C 版 page_netdect 的 nd_get_local() 同源: esp_netif_get_ip_info + esp_netif_get_dns_info。
   返回 ip, 掩码, 网关, DNS, 接口名 (未联网 / 无 netif 时返回 0 个值)。 */
static int l_net_ipinfo(lua_State *L) {
    esp_netif_t *ni = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!ni) ni = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (!ni) return 0;
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(ni, &ip) != ESP_OK) return 0;
    char b[20];
    snprintf(b, sizeof(b), IPSTR, IP2STR(&ip.ip));      lua_pushstring(L, b);
    snprintf(b, sizeof(b), IPSTR, IP2STR(&ip.netmask)); lua_pushstring(L, b);
    snprintf(b, sizeof(b), IPSTR, IP2STR(&ip.gw));      lua_pushstring(L, b);
    b[0] = 0;
    esp_netif_dns_info_t dns;
    if (esp_netif_get_dns_info(ni, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK)
        snprintf(b, sizeof(b), IPSTR, IP2STR(&dns.ip.u_addr.ip4));
    lua_pushstring(L, b);
    const char *d = esp_netif_get_desc(ni);
    lua_pushstring(L, d ? d : "?");
    /* 第 6 值: 本机 STA MAC (netdect 报告要"本机MAC") */
    uint8_t mac[6]; b[0] = 0;
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK)
        snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    lua_pushstring(L, b);
    return 6;
}

/* MAC 文本 → OUI 厂商名; 未命中返回 nil (OUI 字典常驻固件, 与 netdect 的厂商显示同源) */
static int l_net_vendor(lua_State *L) {
    const char *s = luaL_checkstring(L, 1);
    unsigned m[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 6) return 0;
    uint8_t mac[6] = { (uint8_t)m[0], (uint8_t)m[1], (uint8_t)m[2],
                       (uint8_t)m[3], (uint8_t)m[4], (uint8_t)m[5] };
    const char *v = NULL;
    if (os_net_mac_vendor(mac, &v) && v) { lua_pushstring(L, v); return 1; }
    return 0;
}


static int l_net_ping(lua_State *L) {
    const char *ip = luaL_checkstring(L, 1);
    int count = (int)luaL_optinteger(L, 2, 4);
    int timeout = (int)luaL_optinteger(L, 3, 500);
    struct in_addr a;
    if (!inet_aton(ip, &a)) return 0;
    uint32_t niv = ntohl(a.s_addr);
    lua_newtable(L);
    for (int i = 0; i < count; i++) {
        ip_addr_t t; IP_ADDR4(&t, (uint8_t)(niv>>24), (uint8_t)(niv>>16), (uint8_t)(niv>>8), (uint8_t)niv);
        esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
        cfg.target_addr = t; cfg.count = 1; cfg.interval_ms = 50;
        cfg.timeout_ms = (uint32_t)timeout; cfg.task_stack_size = 4096; cfg.task_prio = 4;
        esp_ping_callbacks_t cb = { .on_ping_success = l_ping_succ, .on_ping_end = l_ping_end, .cb_args = &s_ping_ctx };
        esp_ping_handle_t h = NULL;
        if (esp_ping_new_session(&cfg, &cb, &h) != ESP_OK) { lua_pushboolean(L, false); lua_rawseti(L, -2, i*2+1); lua_pushinteger(L, 0); lua_rawseti(L, -2, i*2+2); continue; }
        s_ping_ctx.done = 0; s_ping_ctx.ok = 0; s_ping_ctx.rtt = 0;
        esp_ping_start(h);
        uint32_t t0 = (uint32_t)(esp_timer_get_time()/1000);
        while (!s_ping_ctx.done && (uint32_t)(esp_timer_get_time()/1000) - t0 < (uint32_t)timeout + 100) vTaskDelay(4);
        esp_ping_delete_session(h);
        vTaskDelay(pdMS_TO_TICKS(10));
        lua_pushboolean(L, (int)s_ping_ctx.ok);
        lua_rawseti(L, -2, i*2+1);
        lua_pushinteger(L, (lua_Integer)s_ping_ctx.rtt);
        lua_rawseti(L, -2, i*2+2);
    }
    return 1;
}

/* ==== 异步 HTTP(S) GET (2026-10-02) ====
 * 为什么要有它: app.net.http_get() 是**阻塞**的 —— 一次 HTTPS 往返 1~3 秒,
 * 直接在 poll() 里调会把整个 UI 冻住(帧率掉到 0, 连返回键都不响应)。
 * 这里把请求丢到后台任务, 脚本每帧 fetch_poll() 看结果, UI 全程不掉帧。
 *
 *   app.net.fetch_start(url[, timeout_ms]) -> bool   启动一次 GET (http:// 或 https://)
 *   app.net.fetch_poll() -> "wait"                   还没回来
 *                        -> "ok", body, status      成功 (body 最多 16KB)
 *                        -> "err", "", errcode, errname  失败 (esp_err_t + 名字, 如 ESP_ERR_HTTP_CONNECT)
 *                        -> "idle"                  没有请求
 * 同一时刻只允许一个请求; 结果被 poll 取走后状态回 idle。 */
#define NET_URL_MAX   256
#define NET_BODY_MAX  16384

static char            s_fetch_url[NET_URL_MAX];              /* 内部 RAM: 小 */
static int             s_fetch_timeout = 8000;
static volatile int    s_fetch_state;     /* 0=idle 1=running 2=ok 3=err */
static volatile int    s_fetch_len, s_fetch_status, s_fetch_err;
/* ★ 16KB 缓冲放 PSRAM: 纯 socket 读, 无 DMA/无禁 cache 窗口, 内部 RAM 更该省着用 */
static EXT_RAM_BSS_ATTR char s_fetch_body[NET_BODY_MAX + 1];

static void l_net_fetch_task(void *arg) {
    (void)arg;
    /* 诊断 (28674 = ESP_ERR_HTTP_CONNECT 时最可能是"内部堆连续块不够"):
     * mbedtls 的 TLS in-buffer 在 context 里 **一次性 calloc**, 需要一整块连续内部 RAM
     * (本项目 CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=16384 → 至少要 16KB 连续)。
     * BT + WiFi 同时开着时内部堆只剩 20KB/最大块 13KB → calloc 失败 → 握手直接失败,
     * 对外就表现为 ESP_ERR_HTTP_CONNECT。这行日志一眼就能判定。 */
    ESP_LOGI(TAG, "fetch 前: 内部 free=%u largest=%u psram=%u (url=%s)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), s_fetch_url);
    esp_http_client_config_t cfg = {
        .url = s_fetch_url, .timeout_ms = s_fetch_timeout,
        .buffer_size = 1024,   /* 2048→1024: 内部 RAM 最紧张, 省 1KB (响应体我们自己拼) */
        .disable_auto_redirect = false,
        .crt_bundle_attach = esp_crt_bundle_attach,   /* https 用系统证书包 */
        .keep_alive_enable = false,
    };
    int total = 0, status = 0;
    esp_err_t e = ESP_FAIL;
    esp_http_client_handle_t cl = esp_http_client_init(&cfg);
    if (cl) {
        e = esp_http_client_open(cl, 0);
        if (e == ESP_OK) {
            esp_http_client_fetch_headers(cl);
            status = esp_http_client_get_status_code(cl);
            for (;;) {
                int n = esp_http_client_read(cl, s_fetch_body + total, NET_BODY_MAX - total);
                if (n <= 0) break;
                total += n;
                if (total >= NET_BODY_MAX) break;
            }
        }
        esp_http_client_close(cl);
        esp_http_client_cleanup(cl);
    }
    s_fetch_body[total] = 0;
    if (e != ESP_OK)
        ESP_LOGE(TAG, "fetch 失败: e=%d(%s) http=%d len=%d url=%s",
                 (int)e, esp_err_to_name(e), status, total, s_fetch_url);
    else
        ESP_LOGI(TAG, "fetch 成功: http=%d len=%d url=%s", status, total, s_fetch_url);
    s_fetch_len = total; s_fetch_status = status; s_fetch_err = (int)e;
    __sync_synchronize();                 /* 先写完数据再放状态, 读侧才看不到半截 */
    s_fetch_state = (e == ESP_OK) ? 2 : 3;
    vTaskDelete(NULL);
}

static int l_net_fetch_start(lua_State *L) {
    const char *url = luaL_checkstring(L, 1);
    int timeout = (int)luaL_optinteger(L, 2, 8000);
    if (s_fetch_state == 1) { lua_pushboolean(L, false); return 1; }  /* 上一个还在跑 */
    /* ★★ 必须先联网才能发请求 (2026-10-02 实机踩到整机 reboot):
     *   lwip 是 `wifi_manager_enable()` 里的 esp_netif_init() 建的 —— **从没联过网时
     *   g_tcpip_mbox 是空的**, 这时 getaddrinfo 会直接
     *   `assert failed: tcpip_send_msg_wait_sem (Invalid mbox)` → panic → 重启。
     *   实机回溯: l_net_fetch_task → esp_http_client_open → getaddrinfo → tcpip_send_msg_wait_sem。
     *   ⇒ 未连接一律拒绝启动, 返回 false, 由脚本显示"未联网"。
     * (另外: 全工程没有任何 esp_netif_deinit/tcpip_deinit —— lwip 起来之后不会再拆,
     *  所以"连接过就一直安全"; 只需守住"第一次"。) */
    if (!wifi_manager_is_connected()) { lua_pushboolean(L, false); return 1; }
    snprintf(s_fetch_url, sizeof(s_fetch_url), "%s", url);
    s_fetch_timeout = timeout > 0 ? timeout : 8000;
    s_fetch_len = 0; s_fetch_status = 0; s_fetch_err = 0;
    s_fetch_state = 1;
    /* 任务栈要内部 RAM: TLS 握手期间会短暂关中断/走 wifi 驱动, 与"禁 cache 不能跑 PSRAM 栈"同源 */
    if (xTaskCreatePinnedToCore(l_net_fetch_task, "lua_http", 8192, NULL, 4, NULL, 0) != pdPASS) {
        s_fetch_state = 3; s_fetch_err = -1;
        lua_pushboolean(L, false);
        return 1;
    }
    lua_pushboolean(L, true);
    return 1;
}

static int l_net_fetch_poll(lua_State *L) {
    int st = s_fetch_state;
    if (st == 1) { lua_pushstring(L, "wait"); return 1; }
    if (st == 2) {
        s_fetch_state = 0;
        lua_pushstring(L, "ok");
        lua_pushlstring(L, s_fetch_body, (size_t)s_fetch_len);
        lua_pushinteger(L, s_fetch_status);
        return 3;
    }
    if (st == 3) {
        int code = s_fetch_err;
        s_fetch_state = 0;
        lua_pushstring(L, "err");
        lua_pushstring(L, "");
        lua_pushinteger(L, code);
        lua_pushstring(L, esp_err_to_name((esp_err_t)code));   /* 第 4 个值: 人可读的错误名 */
        return 4;
    }
    lua_pushstring(L, "idle");
    return 1;
}

void lua_mod_net_register(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, l_net_is_connected);    lua_setfield(L, -2, "is_connected");
    lua_pushcfunction(L, l_net_get_ip);          lua_setfield(L, -2, "get_ip");
    lua_pushcfunction(L, l_net_http_get);        lua_setfield(L, -2, "http_get");
    lua_pushcfunction(L, l_net_fetch_start);     lua_setfield(L, -2, "fetch_start");
    lua_pushcfunction(L, l_net_fetch_poll);      lua_setfield(L, -2, "fetch_poll");
    lua_pushcfunction(L, l_net_ping);            lua_setfield(L, -2, "ping");
    lua_pushcfunction(L, l_net_port);            lua_setfield(L, -2, "port");
    lua_pushcfunction(L, l_net_arp);             lua_setfield(L, -2, "arp");
    lua_pushcfunction(L, l_net_arp_list);        lua_setfield(L, -2, "arp_list");
    lua_pushcfunction(L, l_net_ipinfo);          lua_setfield(L, -2, "ipinfo");
    lua_pushcfunction(L, l_net_vendor);          lua_setfield(L, -2, "vendor");
    lua_setfield(L, -2, "net");
}
