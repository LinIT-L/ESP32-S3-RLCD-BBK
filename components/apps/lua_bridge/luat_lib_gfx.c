/* luat_lib_gfx.c — LinTOS Lua 桥 · gfx 模块
 * 拆分自 components/apps/pages/page_lua.c (2026-10-02 模块化, 对标 LuatOS 的 luat_lib_*.c)。
 * 契约: 只通过 lua_setfield(L, -2, "名字") 往**栈顶的 app 表**写字段;
 *       需要子表时自己 lua_newtable(L) → 填 → lua_setfield(L, -2, "子表名")。
 * 加新 API: 本文件写 static int l_xxx(lua_State *L), 再在文件末尾的 register 里加一行。
 */
#include "lua_bridge.h"


/* ---- 坐标/尺寸一律**容忍小数** (四舍五入取整) ----
 * ⚠ 不是"随手放宽": Lua 5.3+ 的 luaL_checkinteger 对**非整数值的浮点**直接抛
 *   "number has no integer representation"。而游戏/动画里位置必然是浮点运算结果
 *   (实测: 本仓库示例游戏 `c.y = c.y + vy*dt/16` → app.bmpr(x, 12.7, …) 抛错 →
 *    整帧 render 异常 → 应用被错误计数拖回列表)。
 *   所以**几何参数**走这个取值器;**颜色参数**仍用 luaL_checkinteger 严格检查
 *   (传 true/false 依然报错 —— 那条守卫要保留)。
 * ⚠ 非 static: img 等模块也要用 (原型在 lua_bridge.h)。 */
int gfx_i(lua_State *L, int idx) {
    lua_Number v = luaL_checknumber(L, idx);
    return (int)(v < 0 ? v - 0.5 : v + 0.5);
}

/* ============ 大号 ASCII (8×12 点阵整数倍放大, 仿 page_pomodoro) ============ */
void app_big_ascii(st7305_handle_t *lcd, int x, int y, char c, int scale, bool inv) {   /* [modsplit] 共享 */
    int idx = (c >= 0x20 && c <= 0x7E) ? (c - 0x20) : ('?' - 0x20);
    st7305_color_t fg = inv ? ST7305_COLOR_WHITE : ST7305_COLOR_BLACK;
    st7305_color_t bg = inv ? ST7305_COLOR_BLACK : ST7305_COLOR_WHITE;
    fill_rect(lcd, x, y, x + 8 * scale - 1, y + 16 * scale - 1, bg);
    const uint8_t *bmp = FONT8X12[idx];
    for (int row = 0; row < 16; row++) {
        uint8_t bits = bmp[row];
        for (int col = 0; col < 8; col++) {
            if (!(bits & (1 << (7 - col)))) continue;
            for (int dy = 0; dy < scale; dy++)
                for (int dx = 0; dx < scale; dx++)
                    st7305_draw_pixel(lcd, x + col * scale + dx, y + row * scale + dy, fg);
        }
    }
}

static int l_clear(lua_State *L) {
    st7305_color_t c = ST7305_COLOR_WHITE;
    if (lua_gettop(L) >= 1 && !lua_isnil(L, 1)) c = (st7305_color_t)((int)luaL_checkinteger(L, 1));
    st7305_clear(s_lcd, c);
    return 0;
}

static int l_px(lua_State *L) {
    int x = (int)gfx_i(L, 1), y = (int)gfx_i(L, 2);
    st7305_color_t c = (st7305_color_t)((int)luaL_checkinteger(L, 3));
    st7305_draw_pixel(s_lcd, x, y, c);
    return 0;
}

static int l_fill(lua_State *L) {
    int x0 = (int)gfx_i(L, 1), y0 = (int)gfx_i(L, 2);
    int x1 = (int)gfx_i(L, 3), y1 = (int)gfx_i(L, 4);
    st7305_color_t c = (st7305_color_t)((int)luaL_checkinteger(L, 5));
    fill_rect(s_lcd, x0, y0, x1, y1, c);
    return 0;
}

static int l_rect(lua_State *L) {   /* app.store_rect / app.rect 同义 */
    int x0 = (int)gfx_i(L, 1), y0 = (int)gfx_i(L, 2);
    int x1 = (int)gfx_i(L, 3), y1 = (int)gfx_i(L, 4);
    st7305_color_t c = (st7305_color_t)((int)luaL_checkinteger(L, 5));
    draw_rect_outline(s_lcd, x0, y0, x1, y1, c);
    return 0;
}

static int l_hline(lua_State *L) {
    int x0 = (int)gfx_i(L, 1), x1 = (int)gfx_i(L, 2), y = (int)gfx_i(L, 3);
    st7305_color_t c = (st7305_color_t)((int)luaL_checkinteger(L, 4));
    draw_hline(s_lcd, x0, x1, y, c);
    return 0;
}

static int l_vline(lua_State *L) {
    int x = (int)gfx_i(L, 1), y0 = (int)gfx_i(L, 2), y1 = (int)gfx_i(L, 3);
    st7305_color_t c = (st7305_color_t)((int)luaL_checkinteger(L, 4));
    draw_vline(s_lcd, x, y0, y1, c);
    return 0;
}

static int l_text(lua_State *L) {
    int x = (int)gfx_i(L, 1), y = (int)gfx_i(L, 2);
    const char *s = luaL_checkstring(L, 3);
    bool inv = (lua_gettop(L) >= 4 && !lua_isnil(L, 4)) ? (lua_toboolean(L, 4) != 0) : false;
    draw_text(s_lcd, x, y, s, inv);
    return 0;
}

static int l_textc(lua_State *L) {
    int y = (int)gfx_i(L, 1);
    const char *s = luaL_checkstring(L, 2);
    bool inv = (lua_gettop(L) >= 3 && !lua_isnil(L, 3)) ? (lua_toboolean(L, 3) != 0) : false;
    draw_text_centered(s_lcd, y, s, inv);
    return 0;
}

static int l_textw(lua_State *L) {
    const char *s = luaL_checkstring(L, 1);
    lua_pushinteger(L, (lua_Integer)text_width(s));
    return 1;
}

static int l_big(lua_State *L) {
    int x = (int)gfx_i(L, 1), y = (int)gfx_i(L, 2);
    const char *s = luaL_checkstring(L, 3);
    int scale = (int)luaL_optinteger(L, 4, 1);
    bool inv = (lua_gettop(L) >= 5 && !lua_isnil(L, 5)) ? (lua_toboolean(L, 5) != 0) : false;
    app_big_text(s_lcd, x, y, s, scale, inv);
    return 0;
}

static int l_bigc(lua_State *L) {
    int cx = (int)gfx_i(L, 1), y = (int)gfx_i(L, 2);
    const char *s = luaL_checkstring(L, 3);
    int scale = (int)luaL_optinteger(L, 4, 1);
    bool inv = (lua_gettop(L) >= 5 && !lua_isnil(L, 5)) ? (lua_toboolean(L, 5) != 0) : false;
    app_big_text_centered(s_lcd, cx, y, s, scale, inv);
    return 0;
}


/* ---- app.zh(x,y,str[,inv]): 16×16 中文点阵, UTF-8 字符串逐字绘制 (照 page_diag dzh16) ---- */

/* ---- app.zh24(x,y,str[,inv]): 24x24 中文点阵 (固件 font_zh 字库, 每字 72 字节) ----
 * C 版 page_diag 的 dz22/dz22s 用的就是这个 (步进 24)。 */

/* ---- app.bmp(x, y, w, h, data): 直绘 1bpp 位图 (data = Lua 字符串, 每行 (w+7)/8 字节, MSB 在左, 1=黑) ----
 * 用途: 应用把图标数据内嵌在自己的 .lua 里 (见 LAPP), 一行调用画出来, 无需固件内置图标。 */
/* 大位图直绘 (**无大小上限**): 第 5 参是**二进制 Lua 串** (每字节 8 像素, MSB 在前, 1=黑,
 * 0=透明不画), 长度 >= ceil(w/8)*h。
 *
 * 为什么需要它: app.bmp 收 hex 串且有 2048B 上限 (约 128×128), 只够贴图标。白板/图片类
 * 应用要"整幅重绘" (载入 BMP 后的基线快照), 尺寸 336×268 = 11KB, 用 app.bmp 会被拒;
 * 逐像素 app.px 又要 ~9 万次跨界调用, 一次重绘就卡住。这里一次调用把整块贴上去。
 * 数据走 luaL_checklstring (长度感知, 二进制安全, 不做 UTF-8 转换)。 */
static int l_bmpr(lua_State *L) {
    int x = (int)gfx_i(L, 1), y = (int)gfx_i(L, 2);
    int w = (int)gfx_i(L, 3), h = (int)gfx_i(L, 4);
    size_t len = 0;
    const char *d = luaL_checklstring(L, 5, &len);
    if (w <= 0 || h <= 0) return 0;
    size_t need = (size_t)((w + 7) / 8) * (size_t)h;
    if (len < need) return 0;                 /* 长度不够 → 容错不画 */
    st7305_draw_bitmap_1bit(s_lcd, x, y, w, h, (const uint8_t *)d);
    return 0;
}


static int l_bmp(lua_State *L) {
    int x = (int)gfx_i(L, 1), y = (int)gfx_i(L, 2);
    int w = (int)gfx_i(L, 3), h = (int)gfx_i(L, 4);
    size_t len = 0;
    const char *hex = luaL_checklstring(L, 5, &len);
    if (w <= 0 || h <= 0) return 0;
    size_t need = (size_t)((w + 7) / 8) * (size_t)h;
    if (need > 2048 || len < need * 2) return 0;      /* 上限 2048B (约 128x128), 容错不画 */
    static uint8_t s_bmp[2048];                        /* 解码缓冲 (内部 BSS, 2KB) */
    static const char *HX = "0123456789abcdef";
    for (size_t i = 0; i < need; i++) {
        int hi = -1, lo = -1;
        char a = hex[i * 2], b = hex[i * 2 + 1];
        for (int k = 0; k < 16; k++) {
            if      (a == HX[k] || a == (char)(HX[k] - 32)) hi = k;
            if      (b == HX[k] || b == (char)(HX[k] - 32)) lo = k;
        }
        if (hi < 0 || lo < 0) return 0;
        s_bmp[i] = (uint8_t)((hi << 4) | lo);
    }
    st7305_draw_bitmap_1bit(s_lcd, x, y, w, h, s_bmp);
    return 0;
}

static int l_zh24(lua_State *L) {
    int x = (int)gfx_i(L, 1), y = (int)gfx_i(L, 2);
    const char *str = luaL_checkstring(L, 3);
    bool inv = (lua_gettop(L) >= 4 && !lua_isnil(L, 4)) ? (lua_toboolean(L, 4) != 0) : false;
    while (*str) {
        int n = utf8_len(str);
        int idx = (n == 3) ? font_zh_find_utf8(str) : -1;
        st7305_color_t bg = inv ? ST7305_COLOR_BLACK : ST7305_COLOR_WHITE;
        st7305_color_t fg = inv ? ST7305_COLOR_WHITE : ST7305_COLOR_BLACK;
        fill_rect(s_lcd, x, y, x + ZH_FONT_W - 1, y + ZH_FONT_H - 1, bg);
        if (idx >= 0) {
            const uint8_t *bmp = zh_font_data[idx];
            for (int row = 0; row < ZH_FONT_H; row++)
                for (int col = 0; col < ZH_FONT_W; col++) {
                    int byte = bmp[row * 3 + (col / 8)];
                    st7305_draw_pixel(s_lcd, x + col, y + row, (byte & (1 << (7 - (col % 8)))) ? fg : bg);
                }
        }
        x += ZH_FONT_W;
        str += n;
    }
    return 0;
}

static int l_zh16(lua_State *L) {
    int x = (int)gfx_i(L, 1), y = (int)gfx_i(L, 2);
    const char *str = luaL_checkstring(L, 3);
    bool inv = (lua_gettop(L) >= 4 && !lua_isnil(L, 4)) ? (lua_toboolean(L, 4) != 0) : false;
    while (*str) {
        int n = utf8_len(str);
        int idx = (n == 3) ? font_zh16_find_utf8(str) : -1;   /* 字库只收 3 字节汉字 */
        st7305_color_t bg = inv ? ST7305_COLOR_BLACK : ST7305_COLOR_WHITE;
        st7305_color_t fg = inv ? ST7305_COLOR_WHITE : ST7305_COLOR_BLACK;
        fill_rect(s_lcd, x, y, x + 15, y + 15, bg);
        if (idx >= 0) {
            const uint8_t *bmp = zh16_font_data[idx];
            for (int row = 0; row < 16; row++)
                for (int col = 0; col < 16; col++) {
                    int byte = bmp[row * 2 + (col / 8)];
                    st7305_draw_pixel(s_lcd, x + col, y + row, (byte & (1 << (7 - (col % 8)))) ? fg : bg);
                }
        }
        x += 16;
        str += n;
    }
    return 0;
}

/* ---- app.zhs(x,y,str[,inv]): 12×12 中文点阵 (23×23 字库 2×2 降采样, 照 page_diag dzh) ---- */
static int l_zh(lua_State *L) {
    int x = (int)gfx_i(L, 1), y = (int)gfx_i(L, 2);
    const char *str = luaL_checkstring(L, 3);
    bool inv = (lua_gettop(L) >= 4 && !lua_isnil(L, 4)) ? (lua_toboolean(L, 4) != 0) : false;
    const int bpr = (ZH_FONT_W + 7) / 8;
    while (*str) {
        int n = utf8_len(str);
        int idx = (n == 3) ? font_zh_find_utf8(str) : -1;   /* 字库只收 3 字节汉字 */
        st7305_color_t bg = inv ? ST7305_COLOR_BLACK : ST7305_COLOR_WHITE;
        st7305_color_t fg = inv ? ST7305_COLOR_WHITE : ST7305_COLOR_BLACK;
        fill_rect(s_lcd, x, y, x + 11, y + 11, bg);
        if (idx >= 0) {
            const uint8_t *bmp = zh_font_data[idx];
            for (int r = 0; r < 12; r++)
                for (int c = 0; c < 12; c++) {
                    bool any = false;
                    for (int dy = 0; dy < 2 && !any; dy++)
                        for (int dx = 0; dx < 2 && !any; dx++) {
                            int row = r * 2 + dy, col = c * 2 + dx;
                            if (bmp[row * bpr + (col / 8)] & (1 << (7 - (col % 8)))) any = true;
                        }
                    st7305_draw_pixel(s_lcd, x + c, y + r, any ? fg : bg);
                }
        }
        x += 12;
        str += n;
    }
    return 0;
}


/* ============ Lua: 固件字模族 ============
 * 原则: **Lua 应用一律用固件字模, 绝不自绘字体**。
 * 下面这些 API 一一对应 ui_common.c 里的固件字体函数 (各 C 版应用页面用的就是它们),
 * 于是 Lua 版与 C 版 UI 同源同形:
 *   app.texts  8×12 ASCII   (draw_ascii_small)   — 小字/提示行
 *   app.textm  12×18 ASCII  (draw_ascii_medium)  — 中字
 *   app.textsb 8×16 ASCII   (draw_ascii_sb)      — 状态栏同款
 *   app.zhsm   12×12 中文   (draw_zh_small)      — 小中文
 *   app.zhsb   16×16 中文   (draw_zh_sb, 22/24 采样)
 *   app.mix    混合 16px    (ui_text16)          — 中文 font_zh16 + ASCII 8×12 垂直居中
 *   app.mixc   混合 16px 居中 (ui_text16c)
 *   app.mixw   混合 16px 行宽 (ui_text16w)
 * 所有绘制函数返回"结束 x"(下一字符起点), 脚本可接续排版。 */
static int l_texts(lua_State *L) {
    int x = (int)gfx_i(L, 1), y = (int)gfx_i(L, 2);
    const char *s = luaL_checkstring(L, 3);
    bool inv = (lua_gettop(L) >= 4 && !lua_isnil(L, 4)) ? (lua_toboolean(L, 4) != 0) : false;
    for (size_t i = 0; s[i]; i++) draw_ascii_small(s_lcd, x + (int)i * 8, y, s[i], inv);
    lua_pushinteger(L, (lua_Integer)(x + (int)strlen(s) * 8));
    return 1;
}

static int l_textm(lua_State *L) {
    int x = (int)gfx_i(L, 1), y = (int)gfx_i(L, 2);
    const char *s = luaL_checkstring(L, 3);
    bool inv = (lua_gettop(L) >= 4 && !lua_isnil(L, 4)) ? (lua_toboolean(L, 4) != 0) : false;
    for (size_t i = 0; s[i]; i++) draw_ascii_medium(s_lcd, x + (int)i * 12, y, s[i], inv);
    lua_pushinteger(L, (lua_Integer)(x + (int)strlen(s) * 12));
    return 1;
}

static int l_textsb(lua_State *L) {
    int x = (int)gfx_i(L, 1), y = (int)gfx_i(L, 2);
    const char *s = luaL_checkstring(L, 3);
    bool inv = (lua_gettop(L) >= 4 && !lua_isnil(L, 4)) ? (lua_toboolean(L, 4) != 0) : false;
    for (size_t i = 0; s[i]; i++) draw_ascii_sb(s_lcd, x + (int)i * 8, y, s[i], inv);
    lua_pushinteger(L, (lua_Integer)(x + (int)strlen(s) * 8));
    return 1;
}

static int l_zhsm(lua_State *L) {
    int x = (int)gfx_i(L, 1), y = (int)gfx_i(L, 2);
    const char *str = luaL_checkstring(L, 3);
    bool inv = (lua_gettop(L) >= 4 && !lua_isnil(L, 4)) ? (lua_toboolean(L, 4) != 0) : false;
    while (*str) { int n = utf8_len(str); draw_zh_small(s_lcd, x, y, str, inv); x += 12; str += n; }
    lua_pushinteger(L, (lua_Integer)x);
    return 1;
}

static int l_zhsb(lua_State *L) {
    int x = (int)gfx_i(L, 1), y = (int)gfx_i(L, 2);
    const char *str = luaL_checkstring(L, 3);
    bool inv = (lua_gettop(L) >= 4 && !lua_isnil(L, 4)) ? (lua_toboolean(L, 4) != 0) : false;
    while (*str) { int n = utf8_len(str); draw_zh_sb(s_lcd, x, y, str, inv); x += 16; str += n; }
    lua_pushinteger(L, (lua_Integer)x);
    return 1;
}

static int l_mix(lua_State *L) {
    int x = (int)gfx_i(L, 1), y = (int)gfx_i(L, 2);
    const char *s = luaL_checkstring(L, 3);
    bool inv = (lua_gettop(L) >= 4 && !lua_isnil(L, 4)) ? (lua_toboolean(L, 4) != 0) : false;
    ui_text16(s_lcd, x, y, s, inv);
    lua_pushinteger(L, (lua_Integer)(x + ui_text16w(s)));
    return 1;
}

static int l_mixc(lua_State *L) {
    int cx = (int)gfx_i(L, 1), y = (int)gfx_i(L, 2);
    const char *s = luaL_checkstring(L, 3);
    bool inv = (lua_gettop(L) >= 4 && !lua_isnil(L, 4)) ? (lua_toboolean(L, 4) != 0) : false;
    ui_text16c(s_lcd, cx, y, s, inv);
    lua_pushinteger(L, (lua_Integer)(cx - ui_text16w(s) / 2));
    return 1;
}

static int l_mixw(lua_State *L) {
    const char *s = luaL_checkstring(L, 1);
    lua_pushinteger(L, (lua_Integer)ui_text16w(s));
    return 1;
}

void lua_mod_gfx_register(lua_State *L)
{
    lua_pushcfunction(L, l_clear);               lua_setfield(L, -2, "clear");
    lua_pushcfunction(L, l_px);                  lua_setfield(L, -2, "px");
    lua_pushcfunction(L, l_fill);                lua_setfield(L, -2, "fill");
    lua_pushcfunction(L, l_rect);                lua_setfield(L, -2, "store_rect");
    lua_pushcfunction(L, l_rect);                lua_setfield(L, -2, "rect");
    lua_pushcfunction(L, l_hline);               lua_setfield(L, -2, "hline");
    lua_pushcfunction(L, l_vline);               lua_setfield(L, -2, "vline");
    lua_pushcfunction(L, l_text);                lua_setfield(L, -2, "text");
    lua_pushcfunction(L, l_textc);               lua_setfield(L, -2, "textc");
    lua_pushcfunction(L, l_textw);               lua_setfield(L, -2, "textw");
    lua_pushcfunction(L, l_big);                 lua_setfield(L, -2, "big");
    lua_pushcfunction(L, l_bigc);                lua_setfield(L, -2, "bigc");
    lua_pushcfunction(L, l_zh16);                  lua_setfield(L, -2, "zh");  /* app.zh = lua_zh16 (font_zh16 真16x16) */
    lua_pushcfunction(L, l_zh24);                lua_setfield(L, -2, "zh24");
    lua_pushcfunction(L, l_bmp);                 lua_setfield(L, -2, "bmp");
    lua_pushcfunction(L, l_bmpr);                lua_setfield(L, -2, "bmpr");
    lua_pushcfunction(L, l_zh);                  lua_setfield(L, -2, "zhs");  /* app.zhs = lua_zh (12x12) */
    lua_pushcfunction(L, l_texts);               lua_setfield(L, -2, "texts");
    lua_pushcfunction(L, l_textm);               lua_setfield(L, -2, "textm");
    lua_pushcfunction(L, l_textsb);              lua_setfield(L, -2, "textsb");
    lua_pushcfunction(L, l_zhsm);                lua_setfield(L, -2, "zhsm");
    lua_pushcfunction(L, l_zhsb);                lua_setfield(L, -2, "zhsb");
    lua_pushcfunction(L, l_mix);                 lua_setfield(L, -2, "mix");
    lua_pushcfunction(L, l_mixc);                lua_setfield(L, -2, "mixc");
    lua_pushcfunction(L, l_mixw);                lua_setfield(L, -2, "mixw");
}
