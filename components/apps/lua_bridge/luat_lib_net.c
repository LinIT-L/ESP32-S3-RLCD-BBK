/* luat_lib_net.c — LinTOS Lua 桥 · net 模块
 * 拆分自 components/apps/pages/page_lua.c (2026-10-02 模块化, 对标 LuatOS 的 luat_lib_*.c)。
 * 契约: 只通过 lua_setfield(L, -2, "名字") 往**栈顶的 app 表**写字段;
 *       需要子表时自己 lua_newtable(L) → 填 → lua_setfield(L, -2, "子表名")。
 * 加新 API: 本文件写 static int l_xxx(lua_State *L), 再在文件末尾的 register 里加一行。
 */
#include "lua_bridge.h"
#include "os_sources.h"
#include "net_client.h"     /* 唯一 HTTP 客户端: 本文件只做参数搬运 + 状态映射 */


/* ---- app.net.* : 网络状态 / HTTP / ping ---- */
/* app.net.mirrors() → { "https://bbk.linit.cn/bbk", ... }
 *
 * 分发包的**镜像源清单**, 取自固件里的 os_sources.c —— 应用商店 (am_store_net.c)
 * 用的就是同一份, 所以"加源/换源/调顺序"只改固件一处, 商店与 Lua 应用自动同步。
 * Lua 侧用法: 依次试 mirrors[i] .. "/maps.json", 第一个通的就用它下地图包。
 * (index.json / maps.json 里也各带一份 mirrors[], 那是运行期的, 会覆盖这份引导列表。) */
static int l_net_mirrors(lua_State *L) {
    int n = os_sources_count();
    lua_newtable(L);
    for (int i = 0; i < n; i++) {
        lua_pushstring(L, os_sources_get(i));
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

static int l_net_is_connected(lua_State *L) {
    lua_pushboolean(L, wifi_manager_is_connected());
    return 1;
}

static int l_net_get_ip(lua_State *L) {
    char ip[32];
    if (wifi_manager_get_ip(ip, sizeof(ip))) { lua_pushstring(L, ip); return 1; }
    return 0;
}

/* app.net.http_get(host, port, path[, timeout]) → body 或 nil
 * ★ 2026-10-15 "联网统一收口": 本函数原来是**第三套** esp_http_client 实现 (同步阻塞)。
 *   现在改成 net_client 的同步包装 —— 行为不变 (调用方仍阻塞等结果), 但底层只剩一个客户端。 */
static int l_net_http_get(lua_State *L) {
    const char *host = luaL_checkstring(L, 1);
    int port = (int)luaL_optinteger(L, 2, 80);
    const char *path = luaL_optstring(L, 3, "/");
    int timeout = (int)luaL_optinteger(L, 4, 5000);
    char url[320];
    snprintf(url, sizeof(url), "http://%s:%d%s", host, port, path);
    /* 4KB 缓冲搬 PSRAM —— 纯 socket 读(无 DMA/无禁 cache 窗口), 内部 RAM 更该省着用 */
    EXT_RAM_BSS_ATTR static char body[4097];
    netc_req_t r = { 0 };
    r.url = url; r.timeout_ms = timeout > 0 ? timeout : 5000;
    r.body = body; r.body_cap = sizeof(body);
    if (!netc_start(&r)) return 0;
    uint32_t t0 = (uint32_t)(esp_timer_get_time() / 1000);
    while (netc_busy() && (uint32_t)(esp_timer_get_time() / 1000) - t0 < (uint32_t)r.timeout_ms + 2000)
        vTaskDelay(pdMS_TO_TICKS(20));
    int n = (netc_state() == NETC_DONE) ? netc_len() : 0;
    netc_reset();
    if (n > 0) { lua_pushlstring(L, body, (size_t)n); return 1; }
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
 *   app.net.fetch_start(url[, timeout_ms[, path]]) -> bool   启动一次 GET
 *       不传 path  → 结果留在内存 (≤ NET_BODY_MAX), fetch_poll 拿到 body
 *       **传 path** → **边收边写文件** (PSRAM 里不驻留整个包, 上限只受 TF 卡空间限制)
 *                     见下面「流式落盘」的说明
 *   app.net.fetch_poll() -> "wait"                         还没回来
 *                        -> "ok", body, status              成功 (非流式)
 *                        -> "ok", nil,  status, nbytes      成功 (流式, 第 4 个值是字节数)
 *                        -> "err", "", errcode, errname     失败 (esp_err_t + 名字)
 *                        -> "idle"                          没有请求
 *   app.net.fetch_got()  -> 已收字节数 (流式模式下用来画进度条)
 * 同一时刻只允许一个请求; 结果被 poll 取走后状态回 idle。 */
/* ★★ 2026-10-15 "联网统一收口" (linit): 请求执行全部交给 **net_client** (唯一 HTTP 客户端)。
 *   本文件只剩两件事: 参数搬运 + 状态映射 —— Lua 侧契约**一行未改**
 *   (fetch_start 返回 bool / fetch_poll 返回 wait|ok|err|idle / fetch_got 返回字节数)。
 *   net_client 的**单请求槽**天然实现了原来那句"同一时刻只允许一个请求"。 */
#define NET_BODY_MAX  (128 * 1024)   /* 内存落点上限: 地图索引 fj.json 有 47KB */
/* ★ PSRAM: 纯 socket 读, 无 DMA/无禁 cache 窗口, 内部 RAM 更该省着用 */
static EXT_RAM_BSS_ATTR char s_fetch_body[NET_BODY_MAX + 1];
static bool s_fetch_streaming;       /* 本次请求是否流式落盘 (决定 poll 返回几个值) */


static int l_net_fetch_start(lua_State *L) {
    const char *url = luaL_checkstring(L, 1);
    int timeout = (int)luaL_optinteger(L, 2, 8000);
    const char *path = luaL_optstring(L, 3, NULL);
    /* ★★ 必须先联网才能发请求 (2026-10-02 实机踩到整机 reboot):
     *   lwip 是 `wifi_manager_enable()` 里的 esp_netif_init() 建的 —— **从没联过网时
     *   g_tcpip_mbox 是空的**, 这时 getaddrinfo 会直接
     *   `assert failed: tcpip_send_msg_wait_sem (Invalid mbox)` → panic → 重启。
     *   ⇒ 未连接一律拒绝启动, 返回 false, 由脚本显示"未联网"。(连接过就一直安全) */
    if (!wifi_manager_is_connected()) { lua_pushboolean(L, false); return 1; }
    netc_req_t r = { 0 };
    r.url = url;
    r.timeout_ms = timeout > 0 ? timeout : 8000;
    if (path && path[0]) { r.sink_path = path; s_fetch_streaming = true; }   /* 流式落盘 */
    else { r.body = s_fetch_body; r.body_cap = (int)sizeof(s_fetch_body); s_fetch_streaming = false; }
    /* net_client 忙 (上一次还在跑) 或参数不合法 → false, 与原来"上一个还在跑"语义一致 */
    lua_pushboolean(L, netc_start(&r));
    return 1;
}

/* app.net.fetch_got() -> 已收字节数 (流式下载画进度条用; 非流式也能看"收了多少") */
static int l_net_fetch_got(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)netc_got());
    return 1;
}

static int l_net_fetch_poll(lua_State *L) {
    netc_state_t st = netc_state();
    if (st == NETC_RUN) { lua_pushstring(L, "wait"); return 1; }
    if (st == NETC_DONE) {
        int n = netc_len(), code = netc_http();
        bool streaming = s_fetch_streaming;
        netc_reset();
        lua_pushstring(L, "ok");
        if (streaming) lua_pushnil(L);                       /* 流式: body 在文件里 */
        else lua_pushlstring(L, s_fetch_body, (size_t)n);
        lua_pushinteger(L, code);
        if (streaming) lua_pushinteger(L, n);                /* 第 4 个值: 落盘字节数 */
        return streaming ? 4 : 3;
    }
    if (st == NETC_ERR) {
        int code = netc_err();
        netc_reset();
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
    lua_pushcfunction(L, l_net_mirrors);         lua_setfield(L, -2, "mirrors");
    lua_pushcfunction(L, l_net_get_ip);          lua_setfield(L, -2, "get_ip");
    lua_pushcfunction(L, l_net_http_get);        lua_setfield(L, -2, "http_get");
    lua_pushcfunction(L, l_net_fetch_start);     lua_setfield(L, -2, "fetch_start");
    lua_pushcfunction(L, l_net_fetch_poll);      lua_setfield(L, -2, "fetch_poll");
    lua_pushcfunction(L, l_net_fetch_got);       lua_setfield(L, -2, "fetch_got");
    lua_pushcfunction(L, l_net_ping);            lua_setfield(L, -2, "ping");
    lua_pushcfunction(L, l_net_port);            lua_setfield(L, -2, "port");
    lua_pushcfunction(L, l_net_arp);             lua_setfield(L, -2, "arp");
    lua_pushcfunction(L, l_net_arp_list);        lua_setfield(L, -2, "arp_list");
    lua_pushcfunction(L, l_net_ipinfo);          lua_setfield(L, -2, "ipinfo");
    lua_pushcfunction(L, l_net_vendor);          lua_setfield(L, -2, "vendor");
    lua_setfield(L, -2, "net");
}
