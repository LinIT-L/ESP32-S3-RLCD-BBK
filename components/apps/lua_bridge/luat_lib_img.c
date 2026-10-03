/* luat_lib_img.c — LinTOS Lua 桥 · img 模块 (1bpp 大图加载 + 窗口位块传输)
 *
 * 为什么需要它 (而不是继续用 app.bmp 的 hex)
 * ------------------------------------------
 * 复刻《英雄坛说》时数据是这个形态:
 *   · 地图 = **每张一整幅预渲染 1bpp 位图** (w*32 × h*32), 最大的 1280×1280 = 200KB
 *   · 角色/NPC = 32×32 (玩家 32×48) 单色精灵, 几百个
 * `app.bmp(x,y,w,h,hex)` 走的是"Lua 字符串传像素", 且内部解码缓冲只有 2048B (上限 128×128):
 *   ① 200KB 的地图根本传不进来;
 *   ② 就算塞进 Lua 字符串, 每帧转换/GC 也吃不消 (实测 render 会到几百 ms)。
 * 所以按本项目原则「核心能力进固件、只对脚本开放 API」, 把"解码 + 取窗口"做进固件:
 * 文件只读一次进 PSRAM, 之后每帧只做一次行块拷贝 (st7305_blit_1bit 批量写)。
 *
 * API
 * ---
 *   app.img.load(path[, opaque]) -> id | nil
 *       path   设备上的 .bmp (1bpp)。**1 位=墨(黑), 0 位=透明** —— 与本项目其余
 *              位图约定一致 (BMP 里 1=白, 这里按 ink=bit==0 反转后存, 见下)。
 *       opaque 真 = 0 位也写(当白), 用于地面/地图这种"整幅铺满"的图;
 *              假/缺省 = 0 位不写, 用于精灵 (叠在已有画面上)。
 *       返回 id (>=1), 失败 nil。
 *   app.img.blit(id, sx, sy, dx, dy, w, h)  从源图 (sx,sy) 取 w×h 贴到屏幕 (dx,dy)
 *   app.img.size(id) -> w, h
 *   app.img.free(id) / app.img.freeall()
 *
 * 说明: 加载时**一次性翻转成自上而下**并把 1bpp 行左对齐打包, 于是 blit 只需
 *       "按行 memcpy 位段", 不做逐像素判断 —— 这是能跑满帧的关键。
 *       BMP 负高度(自上而下)直接照抄; 正高度(自下而上, 本项目素材都是这种)加载时翻行。
 */
#include "lua_bridge.h"
#include "esp_heap_caps.h"

#define IMG_MAX      10       /* 同时驻留: 精灵+底版 / 玩家+底版 / 当前地图 / 备用。
                               * (精灵带底版后是 4 张常驻 + 地图 1 张, 4 个槽不够)
                               * 💥 2026-10-03 由 6 提到 10: QQ堂 冒险模式同时要驻留
                               *   bg + tile + face + item + wicon + equip 六张 ——
                               *   正好撞到旧上限, `load` 会开始顶掉最旧的槽
                               *   (表现是"某个图集忽然画不出来", 且只在真机上偶发)。 */
#define IMG_MAX_W    2048

typedef struct {
    bool     used;
    int      w, h;            /* 像素 */
    int      stride;          /* 每行字节 = (w+7)/8 */
    bool     opaque;
    uint8_t *bits;            /* PSRAM, 自上而下, 1 位 = 墨(黑) */
} img_t;

static EXT_RAM_BSS_ATTR img_t s_img[IMG_MAX];

static void img_release(img_t *im) {
    if (im->bits) { heap_caps_free(im->bits); im->bits = NULL; }
    im->used = false; im->w = im->h = im->stride = 0;
}

/* 载入一个 1bpp BMP 到 PSRAM; 1 位 = 墨。返回 true 成功。
 *
 * ⚠ 性能教训 (2026-10-03, 真机看门狗卡死):
 *   第一版是"每行 fseek 到该行起点再 fread" —— 本地文件系统这么写没事, 但在 **SPIFFS**
 *   上每次 fseek 都会 spiffs_obj_lu_find_id_and_span 扫一遍对象索引, 1280 行 =
 *   1280 次索引扫描, 主任务直接被 task_wdt 判定卡死 (实测 Backtrace 里
 *   fseek → spiffs_phys_rd → spiffs_obj_lu_find_entry_visitor 一路到底)。
 *   正确做法: **整个文件一次读进 PSRAM, 之后在内存里解包** —— 1 次读, 0 次 seek。
 */
static bool img_load_bmp(img_t *im, const char *path, bool opaque) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    long fsz = ftell(f);
    if (fsz < 64 || fsz > (4L << 20)) { fclose(f); return false; }   /* 上限 4MB, 防御 */
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return false; }
    uint8_t *raw = heap_caps_malloc((size_t)fsz, MALLOC_CAP_SPIRAM);
    if (!raw) { fclose(f); return false; }
    size_t got = fread(raw, 1, (size_t)fsz, f);
    fclose(f);
    if (got != (size_t)fsz) { heap_caps_free(raw); return false; }

    if (raw[0] != 'B' || raw[1] != 'M') { heap_caps_free(raw); return false; }
    uint32_t off = (uint32_t)raw[10] | ((uint32_t)raw[11] << 8) |
                   ((uint32_t)raw[12] << 16) | ((uint32_t)raw[13] << 24);
    int32_t  w32 = (int32_t)((uint32_t)raw[18] | ((uint32_t)raw[19] << 8) |
                             ((uint32_t)raw[20] << 16) | ((uint32_t)raw[21] << 24));
    int32_t  h32 = (int32_t)((uint32_t)raw[22] | ((uint32_t)raw[23] << 8) |
                             ((uint32_t)raw[24] << 16) | ((uint32_t)raw[25] << 24));
    uint16_t bpp = (uint16_t)(raw[28] | (raw[29] << 8));
    if (bpp != 1 || w32 <= 0 || w32 > IMG_MAX_W || h32 == 0) { heap_caps_free(raw); return false; }

    bool  flip = (h32 > 0);                 /* 正高度 = 自下而上存储 */
    int   w = (int)w32;
    int   h = (h32 > 0) ? (int)h32 : -(int)h32;
    int   ssrc = ((w + 31) / 32) * 4;       /* BMP 每行 4 字节对齐 */
    int   sdst = (w + 7) / 8;
    if ((size_t)off + (size_t)ssrc * (size_t)h > (size_t)fsz) { heap_caps_free(raw); return false; }

    size_t need = (size_t)sdst * (size_t)h;
    uint8_t *dst = heap_caps_malloc(need, MALLOC_CAP_SPIRAM);
    if (!dst) { heap_caps_free(raw); return false; }
    memset(dst, 0, need);   /* 0 = 无墨 */

    for (int fy = 0; fy < h; fy++) {
        const uint8_t *row = raw + off + (size_t)fy * ssrc;
        int y = flip ? (h - 1 - fy) : fy;   /* BMP 自下而上 → 翻成自上而下 */
        uint8_t *o = dst + (size_t)y * sdst;
        for (int x = 0; x < w; x++) {
            /* BMP 1bpp: 位=1 → 调色板[1]=白, 位=0 → 黑。我们的帧缓冲 1=黑, 故取反。 */
            if (!((row[x >> 3] >> (7 - (x & 7))) & 1))
                o[x >> 3] |= (uint8_t)(0x80 >> (x & 7));
        }
    }
    heap_caps_free(raw);

    im->bits   = dst;
    im->w      = w;
    im->h      = h;
    im->stride = sdst;
    im->opaque = opaque;
    im->used   = true;
    return true;
}

static img_t *img_get(int id) {
    if (id < 1 || id > IMG_MAX) return NULL;
    img_t *im = &s_img[id - 1];
    return im->used ? im : NULL;
}

/* ---- app.img.load(path[, opaque]) -> id | nil ---- */
static int l_img_load(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    bool opaque = (lua_gettop(L) >= 2) && lua_toboolean(L, 2);
    /* 已加载同名则复用: 避免每帧重复开文件 (脚本写错也兜得住) */
    int slot = -1;
    for (int i = 0; i < IMG_MAX; i++) if (!s_img[i].used) { slot = i; break; }
    if (slot < 0) {                              /* 满了 → 顶掉最旧的 */
        img_release(&s_img[0]);
        slot = 0;
    }
    if (!img_load_bmp(&s_img[slot], path, opaque)) { lua_pushnil(L); return 1; }
    ESP_LOGI(TAG, "img.load %s -> id=%d %dx%d%s", path, slot + 1,
             s_img[slot].w, s_img[slot].h, opaque ? " opaque" : "");
    lua_pushinteger(L, slot + 1);
    return 1;
}

/* ---- app.img.blit(id, sx, sy, dx, dy, w, h[, mode]) ----
 * mode: 0/缺省 = 位=1 画**黑**(墨); 1 = 位=1 画**白**。
 *   为什么需要"白"模式: 精灵要像原站那样"先按 mask 刷白底、再压墨点", 否则人物身上
 *   会透出背景 (mask 内的白像素没被画掉)。mask 单独存成一张图 → 用它以 mode=1 刷一遍。
 * 逐行组装成"行内紧凑"的临时位图再批量写:
 *   st7305_blit_1bit 的契约是 bitmap 的每行字节数必须 == (w+7)/8,
 *   而源图的一行是 im->stride (按整幅宽对齐)。所以不能直接把源行指针递进去
 *   (宽度对不上时会一行行错位)。sx 也不必是 8 的倍数 —— 这里统一重排, 脚本随便传。
 * 不透明图 (地图/地面): 再补一趟"写白", 把非墨位刷成白, 才是一幅完整铺满的画面。 */
#define IMG_ROW_MAX 64          /* 上限 512 像素宽 (我们的屏 400) */

static int l_img_blit(lua_State *L) {
    img_t *im = img_get((int)gfx_i(L, 1));
    if (!im) return 0;
    int sx = (int)gfx_i(L, 2), sy = (int)gfx_i(L, 3);
    int dx = (int)gfx_i(L, 4), dy = (int)gfx_i(L, 5);
    int w  = (int)gfx_i(L, 6), h = (int)gfx_i(L, 7);
    int mode = (lua_gettop(L) >= 8 && !lua_isnil(L, 8)) ? (int)gfx_i(L, 8) : 0;
    if (w <= 0 || h <= 0) return 0;
    /* 夹紧到源图范围 (脚本传越界不该崩) */
    if (sx < 0) { dx -= sx; w += sx; sx = 0; }
    if (sy < 0) { dy -= sy; h += sy; sy = 0; }
    if (sx + w > im->w) w = im->w - sx;
    if (sy + h > im->h) h = im->h - sy;
    if (w <= 0 || h <= 0) return 0;
    if (w > IMG_ROW_MAX * 8) w = IMG_ROW_MAX * 8;
    /* 屏内裁剪: blit_1bit 不做安全边检, 这里必须自己保证 x+w<=屏宽, y+h<=屏高 */
    if (dx < 0) { sx -= dx; w += dx; dx = 0; }
    if (dy < 0) { sy -= dy; h += dy; dy = 0; }
    if (dx + w > ui_screen_w()) w = ui_screen_w() - dx;
    if (dy + h > ui_screen_h()) h = ui_screen_h() - dy;
    if (w <= 0 || h <= 0) return 0;

    st7305_handle_t *lcd = s_lcd;
    if (!lcd) return 0;
    /* 诊断 (排查"背景画得出、瓦片画不出"): 前 24 次 blit 全打出来, 之后静音。 */
    {
        static int dbg_n = 0;
        if (dbg_n < 24) {
            dbg_n++;
            ESP_LOGW(TAG, "blit#%d src=%dx%d sx=%d sy=%d dx=%d dy=%d w=%d h=%d mode=%d strd=%d",
                     dbg_n, im->w, im->h, sx, sy, dx, dy, w, h, mode, im->stride);
        }
    }
    static uint8_t s_row[IMG_ROW_MAX], s_inv[IMG_ROW_MAX];
    int rb = (w + 7) / 8;
    int sh = sx & 7;                    /* 源起点在字节内的偏移 */

    for (int y = 0; y < h; y++) {
        const uint8_t *src = im->bits + (size_t)(sy + y) * im->stride;
        const uint8_t *rowp;
        int first = sx >> 3;
        if (sh == 0) {
            /* ★ 快路径 (最常见): 源起点字节对齐 → 直接指进源行, 连重排都省掉。
             * 前提 first + rb <= stride —— 由上面的源图夹紧 + sx 是 8 的倍数保证。 */
            rowp = src + first;
        } else {
            /* 通用路径: 一次取两字节拼 16 位再右移, 而不是逐位测试。
             * 逐位版每行 400 次位判断, 这里只有 rb(≈50) 次字节运算。 */
            for (int j = 0; j < rb; j++) {
                int bi = first + j;
                unsigned v = ((unsigned)src[bi] << 8) |
                             ((bi + 1 < im->stride) ? src[bi + 1] : 0u);
                s_row[j] = (uint8_t)((v >> (8 - sh)) & 0xFFu);
            }
            rowp = s_row;
        }
        if (mode == 1) {
            st7305_blit_1bit_white(lcd, dx, dy + y, w, 1, rowp);   /* 刷白版: 位=1 写白 */
        } else {
            st7305_blit_1bit(lcd, dx, dy + y, w, 1, rowp);
            if (im->opaque) {                       /* 补白: 非墨位刷白 */
                for (int i = 0; i < rb; i++) s_inv[i] = (uint8_t)~rowp[i];
                st7305_blit_1bit_white(lcd, dx, dy + y, w, 1, s_inv);
            }
        }
    }
    return 0;
}

/* ---- app.img.size(id) -> w, h ---- */
static int l_img_size(lua_State *L) {
    img_t *im = img_get((int)gfx_i(L, 1));
    if (!im) { lua_pushnil(L); return 1; }
    lua_pushinteger(L, im->w);
    lua_pushinteger(L, im->h);
    return 2;
}

/* ---- app.img.free(id) / app.img.freeall() ---- */
static int l_img_free(lua_State *L) {
    img_t *im = img_get((int)gfx_i(L, 1));
    if (im) img_release(im);
    return 0;
}
static int l_img_freeall(lua_State *L) {
    (void)L;
    for (int i = 0; i < IMG_MAX; i++) img_release(&s_img[i]);
    return 0;
}

void lua_mod_img_register(lua_State *L) {
    lua_newtable(L);
    lua_pushcfunction(L, l_img_load);    lua_setfield(L, -2, "load");
    lua_pushcfunction(L, l_img_blit);    lua_setfield(L, -2, "blit");
    lua_pushcfunction(L, l_img_size);    lua_setfield(L, -2, "size");
    lua_pushcfunction(L, l_img_free);    lua_setfield(L, -2, "free");
    lua_pushcfunction(L, l_img_freeall); lua_setfield(L, -2, "freeall");
    lua_setfield(L, -2, "img");
}
