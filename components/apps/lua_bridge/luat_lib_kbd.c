/* luat_lib_kbd.c — LinTOS Lua 桥 · kbd 模块
 * 拆分自 components/apps/pages/page_lua.c (2026-10-02 模块化, 对标 LuatOS 的 luat_lib_*.c)。
 * 契约: 只通过 lua_setfield(L, -2, "名字") 往**栈顶的 app 表**写字段;
 *       需要子表时自己 lua_newtable(L) → 填 → lua_setfield(L, -2, "子表名")。
 * 加新 API: 本文件写 static int l_xxx(lua_State *L), 再在文件末尾的 register 里加一行。
 */
#include "lua_bridge.h"
#include "kbd_c42.h"      /* 42 键紧凑键盘: 预算/展示用真实模块 */


/* app.kbd.preview(name[, x, y]) → 高度(px)
 *
 * 键盘展示/选型用: 按名字把项目里**真实的一份**键盘画到 (x, y)，返回它占的像素高。
 * 与 app.kbd.use()+draw() 的区别: use/draw 只画"当前选中的那一套"，preview 一次画一套，
 * 不改动 use/draw 的选中状态与几何，所以能在一屏里连续画几套做对比。
 *
 * name 取值 (只收**通用文本键盘**; 计算器/虚拟键鼠/PIN 九宫格/手柄等专用键盘不在此列):
 *   "std"     52 键全高 (5 排 145px)
 *   "edit"    52 键压低 (排高 19，总高 95px)
 *   "rows"    52 键前 4 排 (116px)
 *   "c42"     42 键·弹窗版 (排高 25 / 满宽横线 / 16×24 字，总高 125px)
 *   "c42page" 42 键·页内版 (排高 26 / 每键分割线 / 16px 混排，宽 = 屏宽-27，总高 130px)。
 *   一句话: 按名字把一套真实键盘画到 (x,y) 并返回像素高, 未知名字返回 -1。
 */
static int l_kbd_preview(lua_State *L) {
    const char *n = luaL_checkstring(L, 1);
    int x = (int)luaL_optinteger(L, 2, 0);
    int y = (int)luaL_optinteger(L, 3, 0);
    if (!s_lcd) { lua_pushinteger(L, -1); return 1; }

    if (!strcmp(n, "std") || !strcmp(n, "edit") || !strcmp(n, "rows")) {
        kbd_style_t st = (!strcmp(n, "edit")) ? KBD_STYLE_EDIT
                       : (!strcmp(n, "rows")) ? KBD_STYLE_ROWS : KBD_STYLE_STD;
        kbd_geom_t g;
        kbd_geom_default(&g, st, y);
        g.x = x;
        g.rows = (st == KBD_STYLE_ROWS) ? 4 : KBD_ROWS;
        kbd_t tmp = kbd_std;                 /* 用副本画, 不污染全局状态 */
        kbd_draw_style(s_lcd, &g, &tmp, -1);
        lua_pushinteger(L, kbd_style_height(st, g.rows, g.row_h));
        return 1;
    }
    if (!strcmp(n, "c42") || !strcmp(n, "c42page")) {
        kbd_c42_geo_t g;
        kbd_c42_state_t st = { false, false };
        if (!strcmp(n, "c42")) kbd_c42_geo_popup(&g, x, y);
        else                   kbd_c42_geo_page(&g, x, y, ST7305_WIDTH - 27);
        kbd_c42_draw(s_lcd, &g, &st, -1);
        lua_pushinteger(L, kbd_c42_height(&g));
        return 1;
    }
    lua_pushinteger(L, -1);
    return 1;
}


/* app.kbd.use(style[, ...]) — 一次选定 样式 + 几何 + 布局表 */
static int l_kbd_use(lua_State *L) {
    const char *n = luaL_checkstring(L, 1);
    int st = kbd_style_by_name(n);
    if (st < 0) { lua_pushboolean(L, false); return 1; }

    if (st == KBD_STYLE_DIALOG) {
        int x  = (int)luaL_checkinteger(L, 2);
        int y  = (int)luaL_checkinteger(L, 3);
        int w  = (int)luaL_checkinteger(L, 4);
        int rh = (int)luaL_checkinteger(L, 5);
        s_kbd_geom.style = KBD_STYLE_DIALOG;
        s_kbd_geom.x = x; s_kbd_geom.y = y; s_kbd_geom.w = w;
        s_kbd_geom.row_h = rh; s_kbd_geom.rows = KBD_ROWS;
    } else if (st == KBD_STYLE_ROWS) {
        int rows = (int)luaL_optinteger(L, 2, KBD_ROWS);
        int y    = (int)luaL_optinteger(L, 3, ST7305_HEIGHT - KBD_ROW * rows);
        kbd_geom_default(&s_kbd_geom, KBD_STYLE_ROWS, y);
        s_kbd_geom.rows = rows;
    } else {
        int defy = (st == KBD_STYLE_EDIT) ? (ST7305_HEIGHT - KBD_EDIT_ROW * KBD_ROWS)
                                          : (ST7305_HEIGHT - KBD_H);
        int y = (int)luaL_optinteger(L, 2, defy);
        kbd_geom_default(&s_kbd_geom, (kbd_style_t)st, y);
    }
    /* 布局表固定 = 标准 52 键 (低高度/缩放只是几何参数, 不是另一份布局) */
    s_kbd = kbd_std;
    s_kbd_inited = true;
    lua_pushboolean(L, true);
    return 1;
}

static int l_kbd_reset(lua_State *L) {
    (void)L;
    if (!s_kbd_inited) kbd_lua_sync();
    else s_kbd = kbd_std;
    return 0;
}

static int l_kbd_draw(lua_State *L) {
    kbd_lua_sync();
    int press = (int)luaL_optinteger(L, 1, -1);
    kbd_draw_style(s_lcd, &s_kbd_geom, &s_kbd, press);
    return 0;
}

static int l_kbd_hit(lua_State *L) {          /* (tx, ty) → idx | -1 */
    kbd_lua_sync();
    int tx = (int)luaL_checkinteger(L, 1), ty = (int)luaL_checkinteger(L, 2);
    lua_pushinteger(L, kbd_hit_style(&s_kbd, &s_kbd_geom, tx, ty));
    return 1;
}

static int l_kbd_geom(lua_State *L) {
    kbd_lua_sync();
    lua_newtable(L);
    lua_pushstring(L, kbd_style_name(s_kbd_geom.style)); lua_setfield(L, -2, "style");
    lua_pushinteger(L, s_kbd_geom.x);      lua_setfield(L, -2, "x");
    lua_pushinteger(L, s_kbd_geom.y);      lua_setfield(L, -2, "y");
    lua_pushinteger(L, s_kbd_geom.w);      lua_setfield(L, -2, "w");
    lua_pushinteger(L, s_kbd_geom.row_h);  lua_setfield(L, -2, "row_h");
    lua_pushinteger(L, s_kbd_geom.rows);   lua_setfield(L, -2, "rows");
    return 1;
}

static int l_kbd_height(lua_State *L) {
    kbd_lua_sync();
    lua_pushinteger(L, kbd_style_height(s_kbd_geom.style, s_kbd_geom.rows, s_kbd_geom.row_h));
    return 1;
}

static int l_kbd_count(lua_State *L) { kbd_lua_sync(); lua_pushinteger(L, s_kbd.n); return 1; }
static int l_kbd_press(lua_State *L) {
    kbd_lua_sync();
    lua_pushinteger(L, kbd_press(&s_kbd, (int)luaL_checkinteger(L, 1)));
    return 1;
}

static int l_kbd_char(lua_State *L) {
    kbd_lua_sync();
    char c = kbd_char(&s_kbd, (int)luaL_checkinteger(L, 1));
    if (!c) { lua_pushnil(L); return 1; }
    char b[2] = { c, 0 };
    lua_pushstring(L, b);
    return 1;
}

static int l_kbd_label(lua_State *L) {
    kbd_lua_sync();
    lua_pushstring(L, kbd_label(&s_kbd, (int)luaL_checkinteger(L, 1)));
    return 1;
}

static int l_kbd_layer(lua_State *L) {
    kbd_lua_sync();
    lua_pushboolean(L, kbd_layer_on(&s_kbd, (int)luaL_checkinteger(L, 1)));
    return 1;
}

static int l_kbd_get(lua_State *L) {          /* ("caps"|"shift"|"fn") */
    kbd_lua_sync();
    const char *k = luaL_optstring(L, 1, "caps");
    if (k && (k[0] == 's'))      lua_pushboolean(L, s_kbd.shift);
    else if (k && (k[0] == 'f')) lua_pushinteger(L, s_kbd.fn_mode);
    else                         lua_pushboolean(L, s_kbd.caps);
    return 1;
}

static int l_kbd_set(lua_State *L) {          /* ("caps"|"shift"|"fn", v) */
    kbd_lua_sync();
    const char *k = luaL_checkstring(L, 1);
    if (k[0] == 's')      s_kbd.shift   = lua_toboolean(L, 2) != 0;
    else if (k[0] == 'f') s_kbd.fn_mode = (int)luaL_optinteger(L, 2, 0) % 3;
    else                  s_kbd.caps    = lua_toboolean(L, 2) != 0;
    return 0;
}

void lua_mod_kbd_register(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, l_kbd_use);             lua_setfield(L, -2, "use");
    lua_pushcfunction(L, l_kbd_reset);           lua_setfield(L, -2, "reset");
    lua_pushcfunction(L, l_kbd_draw);            lua_setfield(L, -2, "draw");
    lua_pushcfunction(L, l_kbd_hit);             lua_setfield(L, -2, "hit");
    lua_pushcfunction(L, l_kbd_geom);            lua_setfield(L, -2, "geom");
    lua_pushcfunction(L, l_kbd_height);          lua_setfield(L, -2, "height");
    lua_pushcfunction(L, l_kbd_count);           lua_setfield(L, -2, "count");
    lua_pushcfunction(L, l_kbd_press);           lua_setfield(L, -2, "press");
    lua_pushcfunction(L, l_kbd_char);            lua_setfield(L, -2, "char");
    lua_pushcfunction(L, l_kbd_label);           lua_setfield(L, -2, "label");
    lua_pushcfunction(L, l_kbd_layer);        lua_setfield(L, -2, "layer_on");
    lua_pushcfunction(L, l_kbd_get);             lua_setfield(L, -2, "get");
    lua_pushcfunction(L, l_kbd_set);             lua_setfield(L, -2, "set");
    lua_pushcfunction(L, l_kbd_preview);         lua_setfield(L, -2, "preview");

    /* ★ 2026-10-03 补齐动作码/几何常量。
     *   原先只注册了函数, 没注册常量 —— 于是脚本写 `if act == app.kbd.CHAR` 时
     *   右边是 nil, **判断永远不成立 → 键盘按下去打不出字**（morse.lua 就踩了这个坑,
     *   因为我当时只照它抄、没核对常量是否真的存在）。数值与 keyboard.h 的
     *   kbd_action_t 一一对应, 改那边务必同步这里。 */
    lua_pushinteger(L, KBD_ACT_CHAR);  lua_setfield(L, -2, "CHAR");
    lua_pushinteger(L, KBD_ACT_ENTER); lua_setfield(L, -2, "ENTER");
    lua_pushinteger(L, KBD_ACT_BKSP);  lua_setfield(L, -2, "BKSP");
    lua_pushinteger(L, KBD_ACT_SPACE); lua_setfield(L, -2, "SPACE");
    lua_pushinteger(L, KBD_ACT_ESC);   lua_setfield(L, -2, "ESC");
    lua_pushinteger(L, KBD_ACT_TAB);   lua_setfield(L, -2, "TAB");
    lua_pushinteger(L, KBD_ACT_UP);    lua_setfield(L, -2, "UP");
    lua_pushinteger(L, KBD_ACT_DOWN);  lua_setfield(L, -2, "DOWN");
    lua_pushinteger(L, KBD_ACT_LEFT);  lua_setfield(L, -2, "LEFT");
    lua_pushinteger(L, KBD_ACT_RIGHT); lua_setfield(L, -2, "RIGHT");
    lua_pushinteger(L, KBD_ACT_CAPS);  lua_setfield(L, -2, "CAPS");
    lua_pushinteger(L, KBD_ACT_SHIFT); lua_setfield(L, -2, "SHIFT");
    lua_pushinteger(L, KBD_ACT_FN);    lua_setfield(L, -2, "FN");
    lua_pushinteger(L, KBD_ACT_CTRL);  lua_setfield(L, -2, "CTRL");
    lua_pushinteger(L, KBD_ACT_ALT);   lua_setfield(L, -2, "ALT");
    lua_pushinteger(L, KBD_ROW);       lua_setfield(L, -2, "ROW");
    lua_pushinteger(L, KBD_EDIT_ROW);  lua_setfield(L, -2, "EDIT_ROW");
    lua_pushinteger(L, KBD_ROWS);      lua_setfield(L, -2, "ROWS");

    lua_setfield(L, -2, "kbd");
}
