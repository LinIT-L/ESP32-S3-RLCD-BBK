/* luat_lib_espnow.c — LinTOS Lua 桥 · espnow 模块
 *
 * 给 Lua 应用用 ESP-NOW **不用路由器**和别的 LinTOS 板子直接收发 (≤238 字节/包)。
 * 典型: 双机联机小游戏、遥控器、对讲信标。
 *
 * 契约: 只通过 lua_setfield(L, -2, "名字") 往**栈顶的 app 表**写字段;
 *       子表自己 lua_newtable(L) → 填 → lua_setfield(L, -2, "espnow")。
 *
 * 用法骨架 (Lua):
 *   app.espnow.start()
 *   app.espnow.name("遥控A")                  -- 本机名 (发现时被对方看到)
 *   app.espnow.scan(1500)                     -- 找设备
 *   for i, d in ipairs(app.espnow.devices()) do
 *       print(d.mac, d.name, d.rssi, d.paired)
 *   end
 *   app.espnow.pair("aa:bb:cc:dd:ee:ff")      -- 配对 (第2参给16字节则加密)
 *   app.espnow.bcast("hello")                 -- 广播
 *   local mac, data = app.espnow.recv()       -- 非阻塞轮询, 没数据返回 nil
 *   app.espnow.stop()                         -- 退出应用时务必调
 *
 * ⚠ 必须 start 之后才能收 (esp-now 没起时回调不挂)。收包走队列, 应用每帧 poll 一次即可。
 * ⚠ 两端必须在**同一信道**: 连了同一路由器就一样; 都没联网默认 1 信道。
 * ⚠ recv() 只返回**应用数据**; 发现/配对的控制帧在本层内部消化, 不会串到应用里。
 */
#include "lua_bridge.h"
#include "espnow_link.h"
#include "espnow_peer.h"
#include "espnow_proto.h"    /* ESPNOW_APP_MAX: 应用可用负载上限 (238) */
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>

/* "aa:bb:cc:dd:ee:ff" / "aabbccddeeff" 两种写法都收 */
static bool parse_mac(const char *s, uint8_t m[6])
{
    if (!s) return false;
    int v[6], n = 0;
    const char *p = s;
    while (*p && n < 6) {
        while (*p == ':' || *p == '-' || *p == ' ') p++;
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F'))) break;
        char b[3] = { p[0], p[1], 0 };
        if (!b[1]) return false;
        v[n++] = (int)strtol(b, NULL, 16);
        p += 2;
    }
    if (n != 6) return false;
    for (int i = 0; i < 6; i++) m[i] = (uint8_t)v[i];
    return true;
}

static void mac_to_str(const uint8_t m[6], char out[18])
{
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

/* app.espnow.start() → bool  幂等: 会顺带把 WiFi 射频拉起来 */
static int l_now_start(lua_State *L)
{
    lua_pushboolean(L, espnow_link_start() == ESP_OK);
    return 1;
}

/* app.espnow.stop() → bool  只关 esp-now, 不动用户的 WiFi */
static int l_now_stop(lua_State *L)
{
    (void)L;
    espnow_link_stop();
    return 0;
}

/* app.espnow.ready() → bool */
static int l_now_ready(lua_State *L)
{
    lua_pushboolean(L, espnow_link_is_running());
    return 1;
}

/* app.espnow.mac() → "aa:bb:..."  本机 MAC (= WiFi STA MAC, 就是别人要发给你的地址) */
static int l_now_mac(lua_State *L)
{
    uint8_t m[6] = {0};
    char s[18];
    if (espnow_link_self_mac(m) != ESP_OK) { lua_pushnil(L); return 1; }
    mac_to_str(m, s);
    lua_pushstring(L, s);
    return 1;
}

/* app.espnow.channel() → int        查当前信道
 * app.espnow.channel(n) → int       强制切到 n 信道 (1..13), 返回切完的信道
 * ⚠ 连了路由器时信道由 AP 决定, 强制切会掉线 —— 只有"都没联网"的场合才用。 */
static int l_now_channel(lua_State *L)
{
    if (lua_gettop(L) >= 1 && lua_isinteger(L, 1))
        espnow_link_set_channel((uint8_t)lua_tointeger(L, 1));
    lua_pushinteger(L, espnow_link_channel());
    return 1;
}

/* app.espnow.add(mac[, key]) → bool
 * mac 可以是 "aa:bb:cc:dd:ee:ff"; key 给 16 字节字符串则启用 ESP-NOW 加密。
 * 发给某个固定对端前必须先 add (广播不用)。 */
static int l_now_add(lua_State *L)
{
    uint8_t m[6];
    if (!parse_mac(luaL_checkstring(L, 1), m)) { lua_pushboolean(L, false); return 1; }
    const uint8_t *lmk = NULL;
    size_t klen = 0;
    if (lua_gettop(L) >= 2 && lua_isstring(L, 2)) {
        lmk = (const uint8_t *)lua_tolstring(L, 2, &klen);
        if (klen != ESPNOW_LMK_LEN) lmk = NULL;      /* 长度不对就当没给 */
    }
    lua_pushboolean(L, espnow_link_add_peer(m, lmk) == ESP_OK);
    return 1;
}

/* app.espnow.del(mac) → bool */
static int l_now_del(lua_State *L)
{
    uint8_t m[6];
    if (!parse_mac(luaL_checkstring(L, 1), m)) { lua_pushboolean(L, false); return 1; }
    lua_pushboolean(L, espnow_link_del_peer(m) == ESP_OK);
    return 1;
}

/* app.espnow.send(mac, data) → bool   点对点 (≤238 字节) */
static int l_now_send(lua_State *L)
{
    uint8_t m[6];
    if (!parse_mac(luaL_checkstring(L, 1), m)) { lua_pushboolean(L, false); return 1; }
    size_t n = 0;
    const char *d = luaL_checklstring(L, 2, &n);
    if (n == 0 || n > ESPNOW_APP_MAX) { lua_pushboolean(L, false); return 1; }
    lua_pushboolean(L, espnow_link_send(m, d, (int)n) == ESP_OK);
    return 1;
}

/* app.espnow.bcast(data) → bool   广播 (同信道所有未加密设备都能收到) */
static int l_now_bcast(lua_State *L)
{
    size_t n = 0;
    const char *d = luaL_checklstring(L, 1, &n);
    if (n == 0 || n > ESPNOW_APP_MAX) { lua_pushboolean(L, false); return 1; }
    lua_pushboolean(L, espnow_link_broadcast(d, (int)n) == ESP_OK);
    return 1;
}

/* app.espnow.recv() → mac, data | nil
 * 非阻塞: 取一条收到的包; 队列空返回 nil。Lua 字符串是二进制安全的, 直接用。 */
static int l_now_recv(lua_State *L)
{
    uint8_t m[6];
    static uint8_t buf[ESPNOW_APP_MAX];
    int n = espnow_link_recv(m, buf, sizeof(buf));
    if (n <= 0) { lua_pushnil(L); return 1; }
    char s[18];
    mac_to_str(m, s);
    lua_pushstring(L, s);
    lua_pushlstring(L, (const char *)buf, (size_t)n);
    return 2;
}

/* app.espnow.pending() → int   队列里还有几条没取 */
static int l_now_pending(lua_State *L)
{
    lua_pushinteger(L, espnow_link_pending());
    return 1;
}

/* app.espnow.stats() → table {rx, drop, tx, fail, ctrl, bad, rssi}  排障用 */
static int l_now_stats(lua_State *L)
{
    espnow_link_stats_t st;
    espnow_link_get_stats(&st);
    lua_newtable(L);
#define PUT_INT(k, v) do { lua_pushinteger(L, (lua_Integer)(v)); lua_setfield(L, -2, (k)); } while (0)
    PUT_INT("rx",    st.rx_ok);
    PUT_INT("drop",  st.rx_drop);
    PUT_INT("tx",    st.tx_ok);
    PUT_INT("fail",  st.tx_fail);
    PUT_INT("ctrl",  st.rx_ctrl);
    PUT_INT("bad",   st.rx_bad);
    PUT_INT("rssi",  st.last_rssi);
#undef PUT_INT
    return 1;
}

/* ==================== 设备发现 / 配对 (2026-11-24 新增) ==================== */

/* app.espnow.name([s]) → string   读/写本机设备名 (发现应答里带出去, 最长 15 字节) */
static int l_now_name(lua_State *L)
{
    if (lua_gettop(L) >= 1 && lua_isstring(L, 1))
        espnow_peer_set_local_name(lua_tostring(L, 1));
    lua_pushstring(L, espnow_peer_local_name());
    return 1;
}

/* app.espnow.scan([ms]) → bool   开始设备发现 (默认 1500ms 窗口, 上限 20000)
 * 广播 DISCOVER, 同信道的其它 LinTOS 会回 ANNOUNCE; 结果用 devices() 取。
 * ⚠ 必须 start() 之后调; 两端必须同信道。 */
static int l_now_scan(lua_State *L)
{
    uint32_t win = 1500;
    if (lua_gettop(L) >= 1 && lua_isinteger(L, 1)) win = (uint32_t)lua_tointeger(L, 1);
    lua_pushboolean(L, espnow_peer_discover(win) == ESP_OK);
    return 1;
}

/* app.espnow.scanning() → bool   是否还在发现窗口内 */
static int l_now_scanning(lua_State *L)
{
    lua_pushboolean(L, espnow_peer_discover_active());
    return 1;
}

/* app.espnow.devices() → table   已发现的设备列表 (数组, 可直接 ipairs)
 * 每项: { mac="aa:bb:..", name="..", rssi=-42, seen=<毫秒前>, paired=bool, enc=bool }
 * ⚠ seen 是"距最近一次收到该设备包"的毫秒数, -1 = 从未收到。 */
static int l_now_devices(lua_State *L)
{
    const int n = espnow_peer_dev_count();
    lua_createtable(L, n > 0 ? n : 0, 0);
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    int idx = 0;
    for (int i = 0; i < n; i++) {
        const espnow_dev_t *d = espnow_peer_dev_get(i);
        if (!d) continue;
        char s[18];
        mac_to_str(d->mac, s);
        lua_createtable(L, 0, 6);
        lua_pushstring(L, s);                                  lua_setfield(L, -2, "mac");
        lua_pushlstring(L, d->name, (size_t)d->name_len);       lua_setfield(L, -2, "name");
        lua_pushinteger(L, d->rssi);                            lua_setfield(L, -2, "rssi");
        lua_pushinteger(L, d->last_seen_ms ? (lua_Integer)(now - d->last_seen_ms) : -1);
        lua_setfield(L, -2, "seen");
        lua_pushboolean(L, d->paired);                          lua_setfield(L, -2, "paired");
        lua_pushboolean(L, d->encrypted);                       lua_setfield(L, -2, "enc");
        lua_rawseti(L, -2, ++idx);
    }
    return 1;
}

/* app.espnow.pair(mac[, key]) → bool   配对 (绑定)
 * key 给 16 字节则启用 ESP-NOW 原生加密 (可防同信道冒充/蹭收, 但见下方安全说明)。
 * 配对是**双向**的: 对端在线时会在收到后自动把本机加进它的 peer 表并回执。
 * ⚠ 安全边界: LMK 是明文传过去的, 配对那一刻无法防空口窃听; 没有身份认证/
 *   前向保密。需要强安全时应在应用层再加一层预共享密钥。加密对端最多 6 台
 *   (ESP-NOW 驱动硬限制), 明文 peer 总数最多 20。 */
static int l_now_pair(lua_State *L)
{
    uint8_t m[6];
    if (!parse_mac(luaL_checkstring(L, 1), m)) { lua_pushboolean(L, false); return 1; }
    const uint8_t *key = NULL;
    size_t klen = 0;
    if (lua_gettop(L) >= 2 && lua_isstring(L, 2)) {
        key = (const uint8_t *)lua_tolstring(L, 2, &klen);
        if (klen != ESPNOW_LMK_LEN) { lua_pushboolean(L, false); return 1; }
    }
    esp_err_t e = espnow_peer_pair(m, key, key ? (int)klen : 0);
    if (e == ESP_OK) espnow_peer_persist();   /* 非回调上下文, 落盘安全 */
    lua_pushboolean(L, e == ESP_OK);
    return 1;
}

/* app.espnow.unpair(mac) → bool   解除配对 (同时通知对端删掉本机) */
static int l_now_unpair(lua_State *L)
{
    uint8_t m[6];
    if (!parse_mac(luaL_checkstring(L, 1), m)) { lua_pushboolean(L, false); return 1; }
    esp_err_t e = espnow_peer_unpair(m);
    if (e == ESP_OK) espnow_peer_persist();
    lua_pushboolean(L, e == ESP_OK);
    return 1;
}

/* app.espnow.paired() → int   已配对设备数 (掉电保存, 重启后仍在) */
static int l_now_paired(lua_State *L)
{
    lua_pushinteger(L, espnow_peer_paired_count());
    return 1;
}

/* app.espnow.info() → table   一次性状态快照
 * { running=bool, mac="..", channel=int, name="..", paired=int, devices=int, scan=bool } */
static int l_now_info(lua_State *L)
{
    lua_newtable(L);
    lua_pushboolean(L, espnow_link_is_running());        lua_setfield(L, -2, "running");
    uint8_t m[6] = { 0 };
    char s[18] = "";
    if (espnow_link_self_mac(m) == ESP_OK) mac_to_str(m, s);
    lua_pushstring(L, s);                                lua_setfield(L, -2, "mac");
    lua_pushinteger(L, espnow_link_channel());           lua_setfield(L, -2, "channel");
    lua_pushstring(L, espnow_peer_local_name());         lua_setfield(L, -2, "name");
    lua_pushinteger(L, espnow_peer_paired_count());      lua_setfield(L, -2, "paired");
    lua_pushinteger(L, espnow_peer_dev_count());         lua_setfield(L, -2, "devices");
    lua_pushboolean(L, espnow_peer_discover_active());   lua_setfield(L, -2, "scan");
    return 1;
}

void lua_mod_espnow_register(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, l_now_start);    lua_setfield(L, -2, "start");
    lua_pushcfunction(L, l_now_stop);     lua_setfield(L, -2, "stop");
    lua_pushcfunction(L, l_now_ready);    lua_setfield(L, -2, "ready");
    lua_pushcfunction(L, l_now_mac);      lua_setfield(L, -2, "mac");
    lua_pushcfunction(L, l_now_name);     lua_setfield(L, -2, "name");
    lua_pushcfunction(L, l_now_channel);  lua_setfield(L, -2, "channel");
    lua_pushcfunction(L, l_now_scan);     lua_setfield(L, -2, "scan");
    lua_pushcfunction(L, l_now_scanning); lua_setfield(L, -2, "scanning");
    lua_pushcfunction(L, l_now_devices);  lua_setfield(L, -2, "devices");
    lua_pushcfunction(L, l_now_pair);     lua_setfield(L, -2, "pair");
    lua_pushcfunction(L, l_now_unpair);   lua_setfield(L, -2, "unpair");
    lua_pushcfunction(L, l_now_paired);   lua_setfield(L, -2, "paired");
    lua_pushcfunction(L, l_now_add);      lua_setfield(L, -2, "add");
    lua_pushcfunction(L, l_now_del);      lua_setfield(L, -2, "del");
    lua_pushcfunction(L, l_now_send);     lua_setfield(L, -2, "send");
    lua_pushcfunction(L, l_now_bcast);    lua_setfield(L, -2, "bcast");
    lua_pushcfunction(L, l_now_recv);     lua_setfield(L, -2, "recv");
    lua_pushcfunction(L, l_now_pending);  lua_setfield(L, -2, "pending");
    lua_pushcfunction(L, l_now_stats);    lua_setfield(L, -2, "stats");
    lua_pushcfunction(L, l_now_info);     lua_setfield(L, -2, "info");
    lua_setfield(L, -2, "espnow");
}
