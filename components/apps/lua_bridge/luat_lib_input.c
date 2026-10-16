/* luat_lib_input.c — LinTOS Lua 桥 · input 模块
 * 拆分自 components/apps/pages/page_lua.c (2026-10-02 模块化, 对标 LuatOS 的 luat_lib_*.c)。
 * 契约: 只通过 lua_setfield(L, -2, "名字") 往**栈顶的 app 表**写字段;
 *       需要子表时自己 lua_newtable(L) → 填 → lua_setfield(L, -2, "子表名")。
 * 加新 API: 本文件写 static int l_xxx(lua_State *L), 再在文件末尾的 register 里加一行。
 */
#include "lua_bridge.h"


/* ============ Lua: 时间/输入/声音/震动/退出 ============ */
static int l_millis(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)(esp_timer_get_time() / 1000));
    return 1;
}

static int l_delay(lua_State *L) {
    int ms = (int)luaL_checkinteger(L, 1);
    /* 警告: 阻塞调用, 脚本勿在 render() 里用, 否则卡帧 */
    if (ms > 0) vTaskDelay(pdMS_TO_TICKS(ms));
    return 0;
}

static int l_key(lua_State *L) {
    if (g_lua_keys_head != g_lua_keys_tail) {
        int k = g_lua_keys[g_lua_keys_head];
        g_lua_keys_head = (g_lua_keys_head + 1) % KEY_FIFO_N;
        lua_pushinteger(L, (lua_Integer)k);
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

bool s_tone_inited = false;   /* [modsplit] 共享 */
/* ============================================================================
 * 游戏基线 API (2026-10-02): 按住状态 / 自动重复 / 日志 / 非阻塞音效
 * ============================================================================ */

/* ---- 按键「按住」状态 ----
 * 数据源 = input_get_held_gb_joypad()  —— **低电平有效**的电平掩码 (GB/GBC 模拟器每帧在查):
 *   bit0=A(确认) bit1=B(返回) bit2=Select bit3=Start bit4=右 bit5=左 bit6=上 bit7=下
 * 它是"保持态"(按住常亮), 且覆盖 蓝牙手柄 / 设备物理键(KEY=A, BOOT=B) / 网页手柄 / 屏幕虚拟键,
 * 所以桥里包一层就能得到 app.key_down(), **不需要改 input 层**。
 * 动作码 → GB 位: 1上=bit6  2下=bit7  3左=bit5  4右=bit4  5确认=A|Start  6返回=B。
 * ⛔ HOME(7) 不在此掩码内, 而且 HOME 被 os_core 直接消费(退出到桌面), 应用内根本观测不到。
 * ⚠ 触摸不算"按住" —— 触摸请走 app.touch() / app.touch_hold()。 */
#define KDOWN_UP    (1u << 6)
#define KDOWN_DOWN  (1u << 7)
#define KDOWN_LEFT  (1u << 5)
#define KDOWN_RIGHT (1u << 4)
#define KDOWN_CONF  ((1u << 0) | (1u << 3))   /* A 或 Start 都算确认 */
#define KDOWN_BACK  (1u << 1)                 /* B */

static bool key_is_down(int a) {
    uint8_t pressed = (uint8_t)~input_get_held_gb_joypad();   /* 取反: 位=1 表示按下 */
    if (!pressed) return false;
    switch (a) {
    case 1:  return (pressed & KDOWN_UP)    != 0;
    case 2:  return (pressed & KDOWN_DOWN)  != 0;
    case 3:  return (pressed & KDOWN_LEFT)  != 0;
    case 4:  return (pressed & KDOWN_RIGHT) != 0;
    case 5:  return (pressed & KDOWN_CONF)  != 0;
    case 6:  return (pressed & KDOWN_BACK)  != 0;
    default: return false;                    /* HOME: 掩码无此项 */
    }
}


uint32_t pl_now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }   /* [modsplit] 共享 */

/* app.key_down(a) → 该动作键当前是否**按住**。做"按住持续移动/连发"全靠它。 */
static int l_key_down(lua_State *L) {
    lua_pushboolean(L, key_is_down((int)luaL_checkinteger(L, 1)));
    return 1;
}


/* app.keys_mask() → 7 位掩码, 与动作码同序: bit0=上 bit1=下 bit2=左 bit3=右 bit4=确认 bit5=返回
 * (bit6=HOME 恒 0)。支持多键同时: `if m & 1 ~= 0 and m & 4 ~= 0` = 左上一起按(斜向)。 */
static int l_keys_mask(lua_State *L) {
    int m = 0;
    for (int a = 1; a <= 6; a++) if (key_is_down(a)) m |= (1 << (a - 1));
    lua_pushinteger(L, m);
    return 1;
}


/* app.key_repeat(a, first_ms, rate_ms) → 带自动重复的"按下沿"(btnP 语义):
 *   刚按下立刻 true; 按住超过 first_ms 后, 每 rate_ms 再 true 一次; 松手 → false 并复位。
 * 默认 first=400 / rate=90。列表滚动、连续发炮直接用, 不必自己数 millis。 */
static uint32_t s_rep_t0[8], s_rep_last[8];
static int l_key_repeat(lua_State *L) {
    int a     = (int)luaL_checkinteger(L, 1);
    int first = (int)luaL_optinteger(L, 2, 400);
    int rate  = (int)luaL_optinteger(L, 3, 90);
    if (a < 1 || a > 6) { lua_pushboolean(L, false); return 1; }
    if (rate < 20) rate = 20;
    uint32_t now = pl_now_ms();
    if (!key_is_down(a)) {
        s_rep_t0[a] = s_rep_last[a] = 0;
        lua_pushboolean(L, false);
        return 1;
    }
    if (s_rep_t0[a] == 0) {                       /* 本次按下沿 */
        s_rep_t0[a]   = now ? now : 1;            /* 0 是"未按"哨兵, 绝不能用 0 */
        s_rep_last[a] = now;
        lua_pushboolean(L, true);
        return 1;
    }
    if ((now - s_rep_t0[a]) >= (uint32_t)first &&
        (now - s_rep_last[a]) >= (uint32_t)rate) {
        s_rep_last[a] = now;
        lua_pushboolean(L, true);
        return 1;
    }
    lua_pushboolean(L, false);
    return 1;
}

/* app.redraw(): 标记本帧需重绘刷屏. Lua 应用 poll() 里逐帧刷新用(雷达/触屏绘制). */
static int l_redraw(lua_State *L) {
    (void)L;
    ui_ctx_t *ctx = s_lua_ctx;
    if (ctx) ctx->needs_redraw = true;
    return 0;
}


/* ============ Lua 桥: 触摸 / 系统 / 文件 / NVS / 网络 / WiFi / HID ============ */

/* ---- app.touch(): 返回 x,y 或 nil ---- */
static int l_app_touch(lua_State *L) {
    int x, y;
    if (input_get_touch_pos(&x, &y)) {
        lua_pushinteger(L, (lua_Integer)x);
        lua_pushinteger(L, (lua_Integer)y);
        return 2;
    }
    return 0;
}

/* ---- app.touch_start(): 本次按下的起点 x,y (拖拽/判滑动方向用); 无手势返回 nil ----
 * 与 app.touch() 同帧使用: 起点固定、当前点变化, 两者相减即位移。 */
static int l_touch_start(lua_State *L) {
    int x, y;
    if (input_touch_start_pos(&x, &y)) {
        lua_pushinteger(L, (lua_Integer)x);
        lua_pushinteger(L, (lua_Integer)y);
        return 2;
    }
    return 0;
}

/* ---- app.touch_hold(): 当前按住时长 ms (未按住返回 0); 脚本据此实现"长按" ---- */
static int l_touch_hold(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)input_touch_hold_ms());
    return 1;
}

void lua_mod_input_register(lua_State *L)
{
    lua_pushcfunction(L, l_millis);              lua_setfield(L, -2, "millis");
    lua_pushcfunction(L, l_key);                 lua_setfield(L, -2, "key");
    lua_pushcfunction(L, l_delay);               lua_setfield(L, -2, "delay");
    lua_pushcfunction(L, l_key_down);            lua_setfield(L, -2, "key_down");
    lua_pushcfunction(L, l_keys_mask);           lua_setfield(L, -2, "keys_mask");
    lua_pushcfunction(L, l_key_repeat);          lua_setfield(L, -2, "key_repeat");
    lua_pushcfunction(L, l_app_touch);           lua_setfield(L, -2, "touch");
    lua_pushcfunction(L, l_touch_start);         lua_setfield(L, -2, "touch_start");
    lua_pushcfunction(L, l_touch_hold);          lua_setfield(L, -2, "touch_hold");
    lua_pushcfunction(L, l_redraw);              lua_setfield(L, -2, "redraw");
}
