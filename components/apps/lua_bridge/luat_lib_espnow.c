/* luat_lib_espnow.c — LinTOS Lua 桥 · espnow 模块
 *
 * 给 Lua 应用用 ESP-NOW **不用路由器**和别的 LinTOS 板子直接收发 (≤250 字节/包)。
 * 典型: 双机联机小游戏、遥控器、对讲信标。
 *
 * 契约: 只通过 lua_setfield(L, -2, "名字") 往**栈顶的 app 表**写字段;
 *       子表自己 lua_newtable(L) → 填 → lua_setfield(L, -2, "espnow")。
 *
 * 用法骨架 (Lua):
 *   app.espnow.start()
 *   app.espnow.bcast("hello")                 -- 广播
 *   local mac, data = app.espnow.recv()       -- 非阻塞轮询, 没数据返回 nil
 *   app.espnow.stop()                         -- 退出应用时务必调
 *
 * ⚠ 必须 start 之后才能收 (esp-now 没起时回调不挂)。收包走队列, 应用每帧 poll 一次即可。
 * ⚠ 两端必须在**同一信道**: 连了同一路由器就一样; 都没联网默认 1 信道。
 */
#include "lua_bridge.h"
#include "espnow_link.h"
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

/* app.espnow.send(mac, data) → bool   点对点 (≤250 字节) */
static int l_now_send(lua_State *L)
{
    uint8_t m[6];
    if (!parse_mac(luaL_checkstring(L, 1), m)) { lua_pushboolean(L, false); return 1; }
    size_t n = 0;
    const char *d = luaL_checklstring(L, 2, &n);
    if (n == 0 || n > ESPNOW_MAX_PAYLOAD) { lua_pushboolean(L, false); return 1; }
    lua_pushboolean(L, espnow_link_send(m, d, (int)n) == ESP_OK);
    return 1;
}

/* app.espnow.bcast(data) → bool   广播 (同信道所有未加密设备都能收到) */
static int l_now_bcast(lua_State *L)
{
    size_t n = 0;
    const char *d = luaL_checklstring(L, 1, &n);
    if (n == 0 || n > ESPNOW_MAX_PAYLOAD) { lua_pushboolean(L, false); return 1; }
    lua_pushboolean(L, espnow_link_broadcast(d, (int)n) == ESP_OK);
    return 1;
}

/* app.espnow.recv() → mac, data | nil
 * 非阻塞: 取一条收到的包; 队列空返回 nil。Lua 字符串是二进制安全的, 直接用。 */
static int l_now_recv(lua_State *L)
{
    uint8_t m[6];
    static uint8_t buf[ESPNOW_MAX_PAYLOAD];
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

/* app.espnow.stats() → table {rx, drop, tx, fail, rssi}  排障用 */
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
    PUT_INT("rssi",  st.last_rssi);
#undef PUT_INT
    return 1;
}

void lua_mod_espnow_register(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, l_now_start);    lua_setfield(L, -2, "start");
    lua_pushcfunction(L, l_now_stop);     lua_setfield(L, -2, "stop");
    lua_pushcfunction(L, l_now_ready);    lua_setfield(L, -2, "ready");
    lua_pushcfunction(L, l_now_mac);      lua_setfield(L, -2, "mac");
    lua_pushcfunction(L, l_now_channel);  lua_setfield(L, -2, "channel");
    lua_pushcfunction(L, l_now_add);      lua_setfield(L, -2, "add");
    lua_pushcfunction(L, l_now_del);      lua_setfield(L, -2, "del");
    lua_pushcfunction(L, l_now_send);     lua_setfield(L, -2, "send");
    lua_pushcfunction(L, l_now_bcast);    lua_setfield(L, -2, "bcast");
    lua_pushcfunction(L, l_now_recv);     lua_setfield(L, -2, "recv");
    lua_pushcfunction(L, l_now_pending);  lua_setfield(L, -2, "pending");
    lua_pushcfunction(L, l_now_stats);    lua_setfield(L, -2, "stats");
    lua_setfield(L, -2, "espnow");
}
