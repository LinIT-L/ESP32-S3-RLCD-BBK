/* luat_lib_sys.c — LinTOS Lua 桥 · sys 模块
 * 拆分自 components/apps/pages/page_lua.c (2026-10-02 模块化, 对标 LuatOS 的 luat_lib_*.c)。
 * 契约: 只通过 lua_setfield(L, -2, "名字") 往**栈顶的 app 表**写字段;
 *       需要子表时自己 lua_newtable(L) → 填 → lua_setfield(L, -2, "子表名")。
 * 加新 API: 本文件写 static int l_xxx(lua_State *L), 再在文件末尾的 register 里加一行。
 */
#include "lua_bridge.h"
#include "audio_player.h"   /* app.volume / app.mute (2026-10-24 补) */
#include "bt_manager.h"     /* app.bt 状态 (2026-10-24 补) */


/* app.log(...) → 打到串口控制台, 带 [APP] 前缀, 用 remcon / 串口日志就能捞回来。
 * 任意类型都能拼(数字/布尔/表走 luaL_tolstring)。调试游戏状态机靠它, 比在屏上画诊断行干净。
 * ⚠ 不用 luaL_Buffer: 它在栈上留内部 userdata, 期间再调 luaL_tolstring 会踩到它; 用定长 C 缓冲更稳。 */
static int l_log(lua_State *L) {
    int n = lua_gettop(L);
    char out[256];
    size_t off = 0;
    out[0] = '\0';
    for (int i = 1; i <= n; i++) {
        size_t len = 0;
        const char *s = luaL_tolstring(L, i, &len);   /* 会 push 一份字符串副本 */
        if (s) {
            size_t room = sizeof(out) - 1 - off;
            size_t cp = (len < room) ? len : room;
            if (cp) memcpy(out + off, s, cp);
            off += cp;
            out[off] = '\0';
        }
        lua_pop(L, 1);                                /* 弹出 luaL_tolstring 压入的副本 */
        if (i < n && off < sizeof(out) - 2) { out[off++] = ' '; out[off] = '\0'; }
    }
    ESP_LOGI(TAG, "[APP] %s", out);
    return 0;
}


/* app.tone_async(freq, ms[, amp]) → 非阻塞单音 (不卡帧). 连打 = 天然排队成小旋律。 */
static int l_tone_async(lua_State *L) {
    if (!s_tone_inited) { os_notify_tone_init(); s_tone_inited = true; }
    int f   = (int)luaL_checkinteger(L, 1);
    int ms  = (int)luaL_checkinteger(L, 2);
    int amp = (int)luaL_optinteger(L, 3, 0);
    os_notify_tone_async((uint16_t)f, (uint16_t)ms, (uint16_t)amp);
    return 0;
}


/* app.melody({{freq,ms}, ...}) → 非阻塞旋律 (后台任务顺序播)。
 * notes 必须存活到播完, 所以缓冲是本桥**私有的静态数组**; 代价是"上一段没播完时必须忽略新请求",
 * 否则会把正在播的缓冲改写掉 → 用预估结束时刻判断, 不需要 tone_player 加状态。 */
#define MEL_MAX 48
static os_note_t s_mel[MEL_MAX + 1];
static uint32_t  s_mel_end_ms;
static int l_melody(lua_State *L) {
    if (!lua_istable(L, 1)) return 0;
    uint32_t now = pl_now_ms();
    if (now < s_mel_end_ms) return 0;              /* 上一段还在播 → 丢弃, 别覆盖缓冲 */
    int n = (int)lua_rawlen(L, 1);
    if (n <= 0) return 0;
    if (n > MEL_MAX) n = MEL_MAX;
    int total = 0;
    for (int i = 1; i <= n; i++) {
        lua_rawgeti(L, 1, i);                      /* 期望 {freq, ms} */
        int f = 0, m = 0;
        if (lua_istable(L, -1)) {
            lua_rawgeti(L, -1, 1); f = (int)lua_tointeger(L, -1); lua_pop(L, 1);
            lua_rawgeti(L, -1, 2); m = (int)lua_tointeger(L, -1); lua_pop(L, 1);
        }
        lua_pop(L, 1);
        if (m <= 0) m = 120;
        if (f < 0)  f = 0;
        s_mel[i - 1].freq = f;
        s_mel[i - 1].ms   = m;
        total += m + 25;                           /* melody_task 对短音会补 25ms 间隔 */
    }
    s_mel[n].freq = 0;                             /* 终止标记 */
    s_mel[n].ms   = 0;
    s_mel_end_ms  = now + (uint32_t)total + 120;
    if (!s_tone_inited) { os_notify_tone_init(); s_tone_inited = true; }
    os_notify_tone_melody(s_mel, n + 1);
    return 0;
}


static int l_tone(lua_State *L) {
    if (!s_tone_inited) { os_notify_tone_init(); s_tone_inited = true; }
    int f = (int)luaL_checkinteger(L, 1), ms = (int)luaL_checkinteger(L, 2);
    os_notify_tone((uint16_t)f, (uint16_t)ms);
    return 0;
}

static bool s_vib_inited = false;
static int l_vibrate(lua_State *L) {
    if (!s_vib_inited) { os_notify_vibrate_init(); s_vib_inited = true; }
    int ms = (int)luaL_checkinteger(L, 1);
    int str = (int)luaL_optinteger(L, 2, 100);
    os_notify_vibrate_tap((uint16_t)ms, (uint8_t)str);
    return 0;
}

static int l_exit(lua_State *L) {
    (void)L;
    /* ★ 不再直接 s_exit_req: 走统一入口 → 全屏程序/游戏默认先弹"退出程序?"
     *   (脚本可用 app.exit_confirm(false) 关掉)。s_lua_ctx 由 page_lua 每帧维护。 */
    lua_request_exit(s_lua_ctx);
    return 0;
}

/* app.exit_confirm([on]) — 全屏程序/游戏"按返回先弹一句 退出程序?" 的开关 (默认开)。
 * 传 true 开 / 传 false 关 (纯浏览类可关掉); 不传参 = 查询当前值, 返回布尔。
 * 只在脚本没消费 BACK 时生效: 脚本把 BACK 当返回键用 (关子窗/收菜单) 依旧优先。 */
static int l_exit_confirm(lua_State *L) {
    if (lua_gettop(L) >= 1) g_lua_exit_ask = (lua_toboolean(L, 1) != 0);
    lua_pushboolean(L, g_lua_exit_ask);
    return 1;
}


/* 状态栏整条反白 (暗底页面用, 如无线探测的雷达视图)。
   注意: 脚本自己负责在 on_exit 里还原成 false, 否则会带到下一页。 */
static int l_sbar_invert(lua_State *L) {
    bool v = lua_toboolean(L, 1);
    os_statusbar_set_invert(v);
    ui_ctx_t *ctx = s_lua_ctx;
    if (ctx) ctx->needs_redraw = true;
    return 0;
}


/* 本地墙上时钟 (与状态栏同源: time() + localtime), 返回 (时, 分, 秒)。
   给"按时间命名存档"这类需求用 (白板 wbHHMMSSSS.bmp 就是 C 版的做法)。
   系统时间未同步 (没联网/没设时间) 时返回的就是开机以来的错值, 脚本自行容忍。 */
/* ★ 2026-10-30: app.sys.date() → month, day, weekday(1=周日..7=周六)
 *   用途: Lua 全屏页要自绘状态栏（系统不画），需要和桌面一致的"10-30 星期五"。 */
static int l_sys_date(lua_State *L) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    lua_pushinteger(L, t ? t->tm_mon + 1 : 1);
    lua_pushinteger(L, t ? t->tm_mday : 1);
    lua_pushinteger(L, t ? t->tm_wday + 1 : 1);
    return 3;
}

static int l_sys_clock(lua_State *L) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    lua_pushinteger(L, t ? t->tm_hour : 0);
    lua_pushinteger(L, t ? t->tm_min : 0);
    lua_pushinteger(L, t ? t->tm_sec : 0);
    return 3;
}


/* ---- app.sys.battery(): 返回电压 mV 或 nil ---- */
static int l_sys_battery(lua_State *L) {
    board_battery_status_t st;
    if (board_battery_read_cached(&st) == ESP_OK) { lua_pushinteger(L, (lua_Integer)st.voltage_mv); return 1; }
    return 0;
}

static int l_sys_heap(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return 1;
}

static int l_sys_time(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)(esp_timer_get_time() / 1000));
    return 1;
}

/* ==== 弹窗模块 app.dlg (让 Lua 应用的提示也走系统弹窗: 框架统一、只传文字) ====
 * 规范 (linit 2026-09-22): 所有弹窗提示必须走 os_dialog, 居中、0.5s 自动关闭,
 * 调用方只给文字 —— 框架/位置只有一处实现, 改框架全局生效。之前 Lua 无接口只能自绘,
 * 于是和系统弹窗长得不一样。
 * ★ 2026-10-03 补全 confirm/list: 用"轮询式"契约而不是给 Lua 加回调 ——
 *   弹窗由 C 侧 os_dialog_* 打开, 结果记在静态变量里, 脚本在 poll() 里调
 *   app.dlg.result() 取 (nil = 还没选, -1 = 取消/返回, >=0 = 确定(0)或列表下标)。
 *   这样完全贴合已有契约 (on_enter/render/poll/action), 不需要动桥的调用约定。 */
static int l_dlg_toast(lua_State *L) {          /* app.dlg.toast(msg[, ms]) */
    const char *msg = luaL_checkstring(L, 1);
    int ms = (int)luaL_optinteger(L, 2, 0);       /* 0 = 默认 0.5s */
    os_dialog_toast_ms(s_lua_ctx, msg, ms > 0 ? (uint32_t)ms : 0);
    return 0;
}

#define LUA_DLG_RESULT_NONE  (-2)
#define LUA_DLG_ITEM_MAX     60      /* 2026-10-15: 12→60 —— 省/市选择这类长列表要走标准弹窗
                                      * (C 侧 DLG_MAX_ITEMS 本来就是 60, 桥原来卡在 12)。
                                      * 静态开销: 60×30B = 1.8KB, 可接受。 */
#define LUA_DLG_ITEM_LEN     30
static volatile int s_dlg_result = LUA_DLG_RESULT_NONE;
static char         s_dlg_items[LUA_DLG_ITEM_MAX][LUA_DLG_ITEM_LEN];
static const char  *s_dlg_ptrs[LUA_DLG_ITEM_MAX];

static void l_dlg_cb(ui_ctx_t *ctx, int result, void *ud) {
    (void)ctx; (void)ud;
    s_dlg_result = result;              /* poll 里由 app.dlg.result() 取走 */
}

/* app.dlg.confirm(msg): 通用确认框 (确定=0 / 取消=-1) */
static int l_dlg_confirm(lua_State *L) {
    const char *msg = luaL_checkstring(L, 1);
    s_dlg_result = LUA_DLG_RESULT_NONE;
    if (s_lua_ctx) os_dialog_confirm_ex(s_lua_ctx, msg, 0, -1, l_dlg_cb, NULL);
    return 0;
}

/* app.dlg.list(title, {项...}[, 初始选中]): 列表选择框 (返回选中下标 1-based) */
static int l_dlg_list(lua_State *L) {
    const char *title = luaL_optstring(L, 1, "");
    if (!lua_istable(L, 2)) { lua_pushnil(L); return 1; }
    int n = (int)lua_rawlen(L, 2);
    if (n > LUA_DLG_ITEM_MAX) n = LUA_DLG_ITEM_MAX;
    for (int i = 0; i < n; i++) {
        lua_rawgeti(L, 2, i + 1);
        const char *it = lua_tostring(L, -1);
        snprintf(s_dlg_items[i], sizeof(s_dlg_items[i]), "%s", it ? it : "");
        s_dlg_ptrs[i] = s_dlg_items[i];
        lua_pop(L, 1);
    }
    int sel = (int)luaL_optinteger(L, 3, 0);
    if (sel < 0) sel = 0;
    if (sel >= n) sel = n - 1;
    s_dlg_result = LUA_DLG_RESULT_NONE;
    if (s_lua_ctx && n > 0) os_dialog_list(s_lua_ctx, title, s_dlg_ptrs, n, sel, l_dlg_cb, NULL);
    lua_pushinteger(L, n);
    return 1;
}

/* app.dlg.result(): nil=还没选; -1=取消; >=0 确定(0/1)/列表下标(1..n) */
static int l_dlg_result(lua_State *L) {
    if (s_dlg_result == LUA_DLG_RESULT_NONE) { lua_pushnil(L); return 1; }
    lua_pushinteger(L, s_dlg_result);
    return 1;
}

/* ===== 2026-10-24 补齐 "设备类" API =====
 * 之前 Lua 应用拿不到音量/静音/蓝牙状态, 只能画个假滑块 (音乐/游戏都卡在这)。
 * C 侧一直有这些能力, 只是桥没暴露 —— 这里按 lua_bridge.h 的三步规则补上。 */

/* app.volume([0-100]) → 当前音量百分比
 * 不传参 = 查询; 传参 = 设为该值(会被夹到 0-100 再映射到 0-9 硬件档)。
 * ⚠ 与系统音量是**同一个值**: 应用改完退出, 系统里看到的也是新值(符合直觉)。 */
static int l_volume(lua_State *L) {
    if (lua_gettop(L) >= 1) {
        int v = (int)luaL_checkinteger(L, 1);
        if (v < 0)   v = 0;
        if (v > 100) v = 100;
        audio_player_set_volume(v);
    }
    lua_pushinteger(L, audio_player_get_volume());
    return 1;
}

/* app.mute([true|false]) → 当前是否静音 (不传参 = 查询)
 * 静音只关输出, 不关播放进度; 与应用内的"暂停"是两件事。 */
static int l_mute(lua_State *L) {
    if (lua_gettop(L) >= 1) audio_player_set_muted(lua_toboolean(L, 1) != 0);
    lua_pushboolean(L, audio_player_is_muted());
    return 1;
}

/* app.bt() → 0=未启用  1=射频就绪(未连)  2=已连接(手柄/耳机)
 * 应用不需要自己开蓝牙: 开机不拉射频, 进设置页/手柄页才由 os_hw 拉起。 */
static int l_bt_state(lua_State *L) {
    int st = 0;
    if (bt_manager_is_ready()) st = bt_manager_is_connected() ? 2 : 1;
    lua_pushinteger(L, st);
    return 1;
}

void lua_mod_sys_register(lua_State *L)
{
    lua_pushcfunction(L, l_log);                 lua_setfield(L, -2, "log");
    lua_pushcfunction(L, l_tone);                lua_setfield(L, -2, "tone");
    lua_pushcfunction(L, l_tone_async);          lua_setfield(L, -2, "tone_async");
    lua_pushcfunction(L, l_melody);              lua_setfield(L, -2, "melody");
    lua_pushcfunction(L, l_vibrate);             lua_setfield(L, -2, "vibrate");
    /* ★ 2026-10-24 补齐的设备类 API (见上方注释) */
    lua_pushcfunction(L, l_volume);              lua_setfield(L, -2, "volume");
    lua_pushcfunction(L, l_mute);                lua_setfield(L, -2, "mute");
    lua_pushcfunction(L, l_bt_state);            lua_setfield(L, -2, "bt");
    lua_pushcfunction(L, l_exit);                lua_setfield(L, -2, "exit");
    lua_pushcfunction(L, l_exit_confirm);        lua_setfield(L, -2, "exit_confirm");
    lua_pushcfunction(L, l_sbar_invert);         lua_setfield(L, -2, "sbar");
    lua_newtable(L);
    lua_pushcfunction(L, l_sys_battery);         lua_setfield(L, -2, "battery");
    lua_pushcfunction(L, l_sys_heap);       lua_setfield(L, -2, "heap_free");
    lua_pushcfunction(L, l_sys_time);         lua_setfield(L, -2, "time_ms");
    lua_pushcfunction(L, l_sys_date);            lua_setfield(L, -2, "date");
    lua_pushcfunction(L, l_sys_clock);           lua_setfield(L, -2, "clock");
    lua_setfield(L, -2, "sys");
    lua_newtable(L);
    lua_pushcfunction(L, l_dlg_toast);           lua_setfield(L, -2, "toast");
    lua_pushcfunction(L, l_dlg_confirm);         lua_setfield(L, -2, "confirm");
    lua_pushcfunction(L, l_dlg_list);            lua_setfield(L, -2, "list");
    lua_pushcfunction(L, l_dlg_result);          lua_setfield(L, -2, "result");
    lua_setfield(L, -2, "dlg");
}
