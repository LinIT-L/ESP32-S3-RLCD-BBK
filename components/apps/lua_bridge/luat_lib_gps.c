/* luat_lib_gps.c — LinTOS Lua 桥 · gps 模块
 *
 * 暴露 gps_nmea.c 的定位快照给 Lua 应用 (自行车码表最先用它)。
 * 契约: 只通过 lua_setfield(L, -2, "名字") 往**栈顶的 app 表**写字段;
 *       子表自己 lua_newtable(L) → 填 → lua_setfield(L, -2, "gps")。
 *
 * ★ 数值一律走**整数定标**, 不走浮点 —— 本项目 Lua 编的是 LUA_32BITS=1,
 *   lua_Number 是 **float**, 只 ~7 位有效数字。直接推 double 经纬度
 *   (119.119000 就 9 位) 会被截断, 位置会漂。所以:
 *     经纬度 → 微度 (×1e6)    速度 → 0.1km/h (×10) 与 cm/s (×100)
 *     航向   → 0.1° (×10)     海拔 → 米 与 分米       HDOP → ×100
 *   显示时整数除法自己拼小数点即可, 全程不丢精度。
 */
#include "lua_bridge.h"
#include "gps.h"
#include "gps_nmea.h"

/* app.gps.start() → bool: 初始化 UART、**唤醒模块**、起后台解析任务 (幂等)
 * ★ 2026-10-09 修: 原来漏了 gps_wakeup() —— 开机时 app_board 已经把 GPS 送进
 *   "软件待机(46µA)", 不唤醒的话它是睡着的, 服务起来的 1.2s 线路探测一个字节都
 *   收不到 ⇒ 应用永远显示"无 GPS 数据"。启动顺序必须是 唤醒 → 再起服务。 */
static int l_gps_start(lua_State *L) {
    esp_err_t e = ESP_OK;
    if (!gps_is_ready()) e = gps_init(NULL);
    if (e == ESP_OK) e = gps_wakeup();
    if (e == ESP_OK) e = gps_service_start();
    lua_pushboolean(L, e == ESP_OK);
    return 1;
}

/* app.gps.stop() → bool: 停后台解析任务 + **把模块送回待机** (2026-10-09)
 * 应用退出时务必调用: 否则解析任务常驻, 且 GPS 会保持 ~10mA 连续跟踪
 * (待机只需 46µA) —— 直接吃电池。 */
static int l_gps_stop(lua_State *L) {
    gps_service_stop();
    gps_sleep();
    lua_pushboolean(L, true);
    return 1;
}

/* app.gps.fix() → table | nil
 * 全是整数定标值 (见文件头), 括号里是还原公式。 */
static int l_gps_fix(lua_State *L) {
    gps_fix_t f;
    if (!gps_nmea_get(&f)) { lua_pushnil(L); return 1; }

    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

    lua_newtable(L);
#define PUT_INT(k, v) do { lua_pushinteger(L, (lua_Integer)(v)); lua_setfield(L, -2, (k)); } while (0)
#define PUT_BOOL(k, v) do { lua_pushboolean(L, (v)); lua_setfield(L, -2, (k)); } while (0)

    PUT_BOOL("valid",  f.valid);
    PUT_BOOL("demo",   f.demo);
    PUT_BOOL("has_time", f.has_time);
    PUT_BOOL("has_date", f.has_date);

    PUT_INT("lat_e6", (int32_t)(f.lat * 1000000.0));        /* 微度 */
    PUT_INT("lon_e6", (int32_t)(f.lon * 1000000.0));

    PUT_INT("kmh10",   (int)(f.speed_mps * 3.6f * 10.0f + 0.5f));  /* 0.1 km/h */
    PUT_INT("mps100",  (int)(f.speed_mps * 100.0f + 0.5f));        /* cm/s */
    PUT_INT("crs10",   (int)(f.course_deg * 10.0f + 0.5f));        /* 0.1 度 */
    PUT_INT("alt_m",   (int)(f.alt_m + 0.5f));
    PUT_INT("alt_dm",  (int)(f.alt_m * 10.0f + 0.5f));             /* 分米 */
    PUT_INT("hdop100", (int)(f.hdop * 100.0f + 0.5f));

    PUT_INT("sats",    f.sats_used);
    PUT_INT("quality", f.fix_quality);
    PUT_INT("dim",     f.fix_dim);

    PUT_INT("hh", f.hour);  PUT_INT("mm", f.min);  PUT_INT("ss", f.sec);
    PUT_INT("yy", f.year);  PUT_INT("mo", f.month); PUT_INT("dd", f.day);

    /* 距上次有效定位多久 (ms); 从未定位 = -1 */
    PUT_INT("age_ms", f.last_fix_ms ? (int32_t)(now - f.last_fix_ms) : -1);
    PUT_INT("sentences", (int32_t)f.sentences);   /* 收了多少句 → 判断线通不通 */
    PUT_INT("badcs",     (int32_t)f.bad_checksum); /* 校验失败数 → 波特率/接线的指纹 */

#undef PUT_INT
#undef PUT_BOOL
    return 1;
}

/* app.gps.demo([on]) → bool: 开/关演示数据 (不读 UART); 不传参 = 查询 */
static int l_gps_demo(lua_State *L) {
    if (lua_gettop(L) >= 1) gps_nmea_demo(lua_toboolean(L, 1) != 0);
    lua_pushboolean(L, gps_nmea_is_demo());
    return 1;
}

/* app.gps.reset(): 清定位状态与统计 (不关演示开关) */
static int l_gps_reset(lua_State *L) {
    (void)L;
    gps_nmea_reset();
    return 0;
}

void lua_mod_gps_register(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, l_gps_start);   lua_setfield(L, -2, "start");
    lua_pushcfunction(L, l_gps_stop);    lua_setfield(L, -2, "stop");
    lua_pushcfunction(L, l_gps_fix);     lua_setfield(L, -2, "fix");
    lua_pushcfunction(L, l_gps_demo);    lua_setfield(L, -2, "demo");
    lua_pushcfunction(L, l_gps_reset);   lua_setfield(L, -2, "reset");
    lua_setfield(L, -2, "gps");
}
