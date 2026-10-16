/* luat_lib_wifi.c — LinTOS Lua 桥 · wifi 模块
 * 拆分自 components/apps/pages/page_lua.c (2026-10-02 模块化, 对标 LuatOS 的 luat_lib_*.c)。
 * 契约: 只通过 lua_setfield(L, -2, "名字") 往**栈顶的 app 表**写字段;
 *       需要子表时自己 lua_newtable(L) → 填 → lua_setfield(L, -2, "子表名")。
 * 加新 API: 本文件写 static int l_xxx(lua_State *L), 再在文件末尾的 register 里加一行。
 */
#include "lua_bridge.h"

/* ==== 联网链 (解锁 netdect 的「选网 → 输密码 → 连接」) ==== */
static int l_wifi_cur_ssid(lua_State *L) {     /* 已连接 SSID; 未连接返回 nil */
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK && ap.ssid[0]) {
        lua_pushstring(L, (const char *)ap.ssid);
        return 1;
    }
    return 0;
}
static int l_wifi_connecting(lua_State *L) { lua_pushboolean(L, wifi_manager_is_connecting()); return 1; }
static int l_wifi_connect(lua_State *L) {      /* connect(ssid[, pass]) -> bool (保存并连接) */
    const char *ssid = luaL_checkstring(L, 1);
    const char *pass = luaL_optstring(L, 2, "");
    lua_pushboolean(L, wifi_manager_connect(ssid, pass));
    return 1;
}

/* ★ 2026-10-15 linit "联网归网络模块": app.wifi.setup() 打开**系统网络模块**的连接流程
 * (扫描 + 输密码 + 保存重连全归它), 应用只提供入口、不自己连网。
 * 另有 app.wifi.connected(): 是否已联网 (只读查询, 门禁/离线提示用)。 */
static int l_wifi_setup(lua_State *L) {
    (void)L;
    if (s_lua_ctx) settings_wifi_connect_open(s_lua_ctx);
    return 0;
}
static int l_wifi_connected(lua_State *L) {
    lua_pushboolean(L, wifi_manager_is_connected());
    return 1;
}



/* ---- app.wifi.* : 无线探测 ---- */
static int l_wifi_init(lua_State *L) {
    lua_pushboolean(L, os_wifi_probe_init() == ESP_OK);
    return 1;
}

static int l_wifi_scan(lua_State *L) {
    lua_pushboolean(L, os_wifi_scan() == ESP_OK);
    return 1;
}

static int l_wifi_scan_done(lua_State *L) {
    lua_pushboolean(L, os_wifi_is_scan_done());
    return 1;
}

static int l_wifi_ap_count(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)os_wifi_ap_count());
    return 1;
}

static int l_wifi_ap_info(lua_State *L) {
    int i = (int)luaL_checkinteger(L, 1);
    const wifi_ap_record_t *ap = os_wifi_ap_record(i);
    if (!ap) return 0;
    lua_pushinteger(L, (lua_Integer)ap->rssi);
    lua_pushstring(L, (const char *)ap->ssid);
    lua_pushinteger(L, (lua_Integer)ap->primary);      /* 信道 */
    lua_pushinteger(L, (lua_Integer)ap->authmode);     /* 加密 */
    char mac[18]; snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
        ap->bssid[0], ap->bssid[1], ap->bssid[2], ap->bssid[3], ap->bssid[4], ap->bssid[5]);
    lua_pushstring(L, mac);
    return 5;
}

static int l_wifi_probe_init(lua_State *L) {
    lua_pushboolean(L, os_wifi_sniff_start() == ESP_OK);
    return 1;
}

static int l_wifi_probe_stop(lua_State *L) {
    os_wifi_sniff_stop();
    return 0;
}

static int l_wifi_probe_count(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)os_wifi_probe_count());
    return 1;
}

static int l_wifi_probe_info(lua_State *L) {
    int i = (int)luaL_checkinteger(L, 1);
    const wp_probe_t *p = os_wifi_probe_get(i);
    if (!p) return 0;
    char mac[18]; snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
        p->sa[0], p->sa[1], p->sa[2], p->sa[3], p->sa[4], p->sa[5]);
    lua_pushstring(L, mac);
    lua_pushinteger(L, (lua_Integer)p->rssi);
    lua_pushinteger(L, (lua_Integer)p->channel);
    /* V1.3: 补 ssid 与捕获次数 —— C 版探针页要显示 "ssid" 与 "x<count> CH<n>" */
    lua_pushstring(L, p->ssid);
    lua_pushinteger(L, (lua_Integer)p->count);
    return 5;
}

static int l_wifi_release(lua_State *L) {
    os_wifi_probe_deinit();
    return 0;
}

static int l_wifi_ch_stat(lua_State *L) {
    const wp_ch_stat_t *ch = os_wifi_ch_stat();
    if (!ch) return 0;
    lua_newtable(L);
    for (int i=0;i<WP_CH_COUNT;i++){
        lua_pushinteger(L, (lua_Integer)ch[i].beacon);
        lua_pushinteger(L, (lua_Integer)ch[i].probe);
        lua_createtable(L,0,2);
        lua_pushvalue(L,-3); lua_setfield(L,-2,"beacon");
        lua_pushvalue(L,-3); lua_setfield(L,-2,"probe");
        lua_rawseti(L, -4, i+1);
        lua_pop(L, 2);
    }
    return 1;
}

void lua_mod_wifi_register(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, l_wifi_init);           lua_setfield(L, -2, "init");
    lua_pushcfunction(L, l_wifi_scan);           lua_setfield(L, -2, "scan");
    lua_pushcfunction(L, l_wifi_scan_done);      lua_setfield(L, -2, "scan_done");
    lua_pushcfunction(L, l_wifi_ap_count);       lua_setfield(L, -2, "ap_count");
    lua_pushcfunction(L, l_wifi_ap_info);        lua_setfield(L, -2, "ap_info");
    lua_pushcfunction(L, l_wifi_probe_init);     lua_setfield(L, -2, "probe_init");
    lua_pushcfunction(L, l_wifi_probe_stop);     lua_setfield(L, -2, "probe_stop");
    lua_pushcfunction(L, l_wifi_probe_count);    lua_setfield(L, -2, "probe_count");
    lua_pushcfunction(L, l_wifi_probe_info);     lua_setfield(L, -2, "probe_info");
    lua_pushcfunction(L, l_wifi_release);        lua_setfield(L, -2, "release");
    lua_pushcfunction(L, l_wifi_ch_stat);        lua_setfield(L, -2, "ch_stat");
    lua_pushcfunction(L, l_wifi_cur_ssid);       lua_setfield(L, -2, "cur_ssid");
    lua_pushcfunction(L, l_wifi_connecting);     lua_setfield(L, -2, "connecting");
    lua_pushcfunction(L, l_wifi_connect);        lua_setfield(L, -2, "connect");
    lua_pushcfunction(L, l_wifi_setup);          lua_setfield(L, -2, "setup");
    lua_pushcfunction(L, l_wifi_connected);      lua_setfield(L, -2, "connected");
    lua_pushcfunction(L, l_wifi_clear_saved);    lua_setfield(L, -2, "clear_saved");
    lua_setfield(L, -2, "wifi");
}
