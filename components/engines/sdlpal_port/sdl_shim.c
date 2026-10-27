/* sdl_shim.c — SDLPal 用的 SDL 1.2 子集实现（ESP32-S3 / LinTOS）
 *
 * 分工：
 *   · 本文件：视频 Surface / 事件队列 / 计时 / RWops 文件
 *   · sdl_audio.c：OpenAudio + 回调 -> audio_player_feed_pcm
 *   · pal_port.c：8bpp+调色板 -> 1bpp 灰度 + 百分比放大 -> ST7305 上屏
 *
 * ⛔ 不要在这里直接画屏：这里的 UpdateRect 只置"有脏帧"标志，
 *    真正转换/上屏由 pal_port_present() 在页面 poll 里做（避免锁住游戏逻辑）。
 */
#include "SDL.h"
#include "pal_port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "SDLSHIM";

/* ================= 视频 ================= */
static SDL_Surface  s_video;                    /* 唯一的"屏幕" Surface */
static SDL_PixelFormat s_fmt;
static SDL_Palette  s_pal;                      /* 256 色调色板 */
static uint8_t     *s_pixels = NULL;            /* PSRAM: 320*200 = 64KB */
static char         s_err[96] = "";
static SDL_bool     s_video_ready = SDL_FALSE;

/* 事件队列（游戏逻辑每帧 SDL_PollEvent 取） */
#define EVQ_N 32
static SDL_Event s_evq[EVQ_N];
static volatile int s_ev_head = 0, s_ev_tail = 0;

int  SDL_Init(Uint32 flags) { (void)flags; return 0; }
void SDL_Quit(void) {}

const char *SDL_GetError(void) { return s_err; }
void SDL_SetError(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    vsnprintf(s_err, sizeof(s_err), fmt, ap);
    va_end(ap);
}

SDL_Surface *SDL_SetVideoMode(int width, int height, int bpp, Uint32 flags)
{
    (void)flags;
    if (bpp != 8) {
        SDL_SetError("only 8bpp supported (got %d)", bpp);
        return NULL;
    }
    if (width <= 0 || height <= 0 || width > 640 || height > 480) {
        SDL_SetError("bad size %dx%d", width, height);
        return NULL;
    }
    /* 载入阶段: 视频就绪 (pal 的载入页用真实事件驱动, 不按时间猜) */
    { extern void pal_port_note_stage(int); pal_port_note_stage(1); }
    size_t need = (size_t)width * (size_t)height;
    if (!s_pixels) {
        /* 一次性按最大 640x480 预留，避免反复 malloc/free 造成 PSRAM 碎片 */
        s_pixels = heap_caps_malloc(640 * 480, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_pixels) { SDL_SetError("no PSRAM for video"); return NULL; }
    }
    memset(s_pixels, 0, need);

    s_fmt.palette        = &s_pal;
    s_fmt.BitsPerPixel   = 8;
    s_fmt.BytesPerPixel  = 1;

    s_video.flags        = SDL_SWSURFACE;
    s_video.format       = &s_fmt;
    s_video.w            = width;
    s_video.h            = height;
    s_video.pitch        = (Uint16)width;
    s_video.pixels       = s_pixels;
    s_video.clip_rect.x  = 0;
    s_video.clip_rect.y  = 0;
    s_video.clip_rect.w  = (Uint16)width;
    s_video.clip_rect.h  = (Uint16)height;
    s_video.refcount     = 1;

    s_video_ready = SDL_TRUE;
    pal_port_set_source(s_pixels, width, height, (uint8_t *)s_pal.colors);
    ESP_LOGI(TAG, "video mode %dx%d 8bpp, pitch=%d", width, height, s_video.pitch);
    return &s_video;
}

/* ★★★ 2026-10-25 修复 PSRAM 泄漏（实测一次进出仙剑永久少 2.59MB）:
 *   这块 640x480 视频缓冲是**进程内一次性预留**（为免反复 malloc/free 造成碎片），
 *   但从来没有释放路径 ⇒ 退出游戏后 300KB 一直挂在那里。
 *   现在由 pal_shutdown() 显式调用归还；下次进游戏 SDL_SetVideoMode 会重新分配。
 *   ⚠ 必须先 pal_port_set_source(NULL,...) 断开引用，否则渲染侧会读已释放内存。 */
void pal_shim_free_video(void)
{
    pal_port_set_source(NULL, 0, 0, NULL);
    if (s_pixels) {
        heap_caps_free(s_pixels);
        s_pixels = NULL;
    }
    memset(&s_video, 0, sizeof(s_video));
    s_video_ready = SDL_FALSE;
    ESP_LOGI(TAG, "视频缓冲已归还 (640x480 PSRAM)");
}

SDL_Surface *SDL_GetVideoSurface(void) { return s_video_ready ? &s_video : NULL; }

int  SDL_LockSurface(SDL_Surface *s)   { (void)s; return 0; }
void SDL_UnlockSurface(SDL_Surface *s) { (void)s; }

/* 只标脏：真正转换在 pal_port_present()，不阻塞游戏逻辑 */
void SDL_UpdateRect(SDL_Surface *s, Sint32 x, Sint32 y, Uint32 w, Uint32 h)
{
    (void)s; (void)x; (void)y; (void)w; (void)h;
    pal_port_mark_dirty();
}

void SDL_UpdateRects(SDL_Surface *s, int n, SDL_Rect *r)
{
    (void)s; (void)n; (void)r;
    pal_port_mark_dirty();
}

int SDL_Flip(SDL_Surface *s) { (void)s; pal_port_mark_dirty(); return 0; }

int SDL_SetPalette(SDL_Surface *s, int flags, SDL_Color *colors, int first, int n)
{
    (void)flags;
    if (!colors || first < 0 || first >= 256) return 0;
    int cnt = n;
    if (first + cnt > 256) cnt = 256 - first;
    for (int i = 0; i < cnt; i++) s_pal.colors[first + i] = colors[i];
    s_pal.ncolors = 256;
    if (s) (void)s;
    pal_port_set_palette((uint8_t *)s_pal.colors);
    pal_port_mark_dirty();          /* PAL_FORCE_UPDATE_ON_PALETTE_SET */
    return 1;
}

int SDL_SetColors(SDL_Surface *s, SDL_Color *colors, int first, int n)
{
    return SDL_SetPalette(s, SDL_LOGPAL | SDL_PHYSPAL, colors, first, n);
}

Uint32 SDL_MapRGB(const SDL_PixelFormat *fmt, Uint8 r, Uint8 g, Uint8 b)
{
    (void)fmt; (void)g; (void)b;
    return r;                        /* 8bpp 索引直通 */
}

SDL_Surface *SDL_CreateRGBSurface(Uint32 flags, int w, int h, int bpp,
                                  Uint32 Rm, Uint32 Gm, Uint32 Bm, Uint32 Am)
{
    (void)flags; (void)Rm; (void)Gm; (void)Bm; (void)Am;
    if (bpp != 8 || w <= 0 || h <= 0) return NULL;
    SDL_Surface *s = calloc(1, sizeof(SDL_Surface));
    if (!s) return NULL;
    uint8_t *px = calloc((size_t)w * h, 1);
    if (!px) { free(s); return NULL; }
    s->w = w; s->h = h; s->pitch = (Uint16)w; s->pixels = px;
    s->format = &s_fmt;
    s->clip_rect.w = (Uint16)w; s->clip_rect.h = (Uint16)h;
    s->refcount = 1;
    s->_own = px;
    return s;
}

void SDL_FreeSurface(SDL_Surface *s)
{
    if (!s) return;
    if (s == &s_video) return;                 /* 屏幕 Surface 不释放 */
    if (s->_own) free(s->_own);
    free(s);
}

int SDL_FillRect(SDL_Surface *dst, SDL_Rect *r, Uint32 color)
{
    if (!dst || !dst->pixels) return -1;
    SDL_Rect a = { 0, 0, (Uint16)dst->w, (Uint16)dst->h };
    /* ★ 2026-10-27 修堆越界: 原来只 clamp 右下, 负数 x/y 会变成"从 0 开始按原宽填",
     *   右侧一超界就写穿缓冲 → 破坏相邻堆块元数据 → 之后任意 malloc 崩在 tlsf。
     *   现在按矩形交集合成 (x0,y0)-(x1,y1), 四个方向都裁。 */
    if (r) {
        int x0 = r->x, y0 = r->y;
        int x1 = r->x + r->w, y1 = r->y + r->h;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > dst->w) x1 = dst->w;
        if (y1 > dst->h) y1 = dst->h;
        if (x1 <= x0 || y1 <= y0) return 0;          /* 完全在屏外 */
        a.x = (Uint16)x0; a.y = (Uint16)y0;
        a.w = (Uint16)(x1 - x0); a.h = (Uint16)(y1 - y0);
    }
    for (int y = 0; y < a.h; y++)
        memset((uint8_t *)dst->pixels + (size_t)(a.y + y) * dst->pitch + a.x,
               (int)color, a.w);
    if (dst == &s_video) pal_port_mark_dirty();
    return 0;
}

int SDL_BlitSurface(SDL_Surface *src, SDL_Rect *sr, SDL_Surface *dst, SDL_Rect *dr)
{
    if (!src || !dst || !src->pixels || !dst->pixels) return -1;
    int sx = sr ? sr->x : 0, sy = sr ? sr->y : 0;
    int sw = sr && sr->w ? sr->w : src->w;
    int sh = sr && sr->h ? sr->h : src->h;
    int dx = dr ? dr->x : 0, dy = dr ? dr->y : 0;
    /* ★ 2026-10-27 修堆越界(仙剑崩溃根因):
     *   原来只裁右/下边, **dx/dy 为负时直接把第一行 memcpy 到 pixels 之前** ——
     *   s_pixels 是 640x480 的 PSRAM 块, 精灵在屏幕左/上边缘露出一半时
     *   (仙剑走到地图边界极常见) 就写坏相邻堆块头 → 之后 malloc 在 tlsf 里崩。
     *   标准做法: 先按源矩形裁, 再按目标矩形裁, 两侧同步偏移。 */
    if (sx < 0) { dx -= sx; sw += sx; sx = 0; }
    if (sy < 0) { dy -= sy; sh += sy; sy = 0; }
    if (sx + sw > src->w) sw = src->w - sx;
    if (sy + sh > src->h) sh = src->h - sy;
    if (dx < 0) { sx -= dx; sw += dx; dx = 0; }
    if (dy < 0) { sy -= dy; sh += dy; dy = 0; }
    if (dx + sw > dst->w) sw = dst->w - dx;
    if (dy + sh > dst->h) sh = dst->h - dy;
    if (sw <= 0 || sh <= 0 || sx < 0 || sy < 0) return 0;
    for (int y = 0; y < sh; y++)
        memcpy((uint8_t *)dst->pixels + (size_t)(dy + y) * dst->pitch + dx,
               (uint8_t *)src->pixels + (size_t)(sy + y) * src->pitch + sx, sw);
    if (dst == &s_video) pal_port_mark_dirty();
    return 0;
}

SDL_Surface *SDL_DisplayFormat(SDL_Surface *s) { return s; }
void SDL_WM_SetCaption(const char *t, const char *i) { (void)t; (void)i; }
int  SDL_ShowCursor(int toggle) { (void)toggle; return 0; }

/* ★★★ SDLPal 的输入**完全靠这个数组轮询**（不是事件驱动）：
 *   input.c 第 43-44 行把 SDL_GetKeyboardState 宏映射到 SDL_GetKeyState、
 *   SDL_GetScancodeFromKey(x) 宏映射到 (x)，然后 PAL_UpdateKeyboardState()
 *   每帧查 keyState[SDLK_UP] 这类下标来调 PAL_KeyDown/KeyUp。
 *   而 _default_input_event_filter() 是个空return 0 —— 事件队列根本没人消费。
 *   ⇒ 只往队列推事件、不写这个数组，游戏就是"按什么都没反应"。
 *   ⚠ 下标是 SDLK_*（SDLK_UP=273…），所以数组必须 >= 512。 */
static Uint8 s_keystate[SDLK_LAST];

/* ================= 事件 ================= */
/* 由 pal_port 侧（按键/触摸）调用来投递 */
void SDL_PushKeyEvent(int sym, int down)
{
    /* ★ 同步键盘状态数组 —— 这是 SDLPal 真正读取输入的地方（见上面的说明） */
    if (sym >= 0 && sym < SDLK_LAST) s_keystate[sym] = down ? 1 : 0;

    int nt = (s_ev_head + 1) % EVQ_N;
    if (nt == s_ev_tail) return;              /* 满则丢 */
    SDL_Event *e = &s_evq[s_ev_head];
    memset(e, 0, sizeof(*e));
    e->key.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    e->key.state = down ? 1 : 0;
    e->key.keysym.sym = sym;
    s_ev_head = nt;
}

void SDL_PushMouseEvent(int type, int x, int y, int button)
{
    int nt = (s_ev_head + 1) % EVQ_N;
    if (nt == s_ev_tail) return;
    SDL_Event *e = &s_evq[s_ev_head];
    memset(e, 0, sizeof(*e));
    if (type == SDL_MOUSEMOTION) {
        e->motion.type = SDL_MOUSEMOTION;
        e->motion.x = (Uint16)x; e->motion.y = (Uint16)y;
    } else {
        e->button.type = (Uint8)type;
        e->button.x = (Uint16)x; e->button.y = (Uint16)y;
        e->button.button = (Uint8)button;
    }
    s_ev_head = nt;
}

void SDL_PumpEvents(void) {}

int SDL_PollEvent(SDL_Event *e)
{
    if (s_ev_tail == s_ev_head) return 0;
    if (e) *e = s_evq[s_ev_tail];
    s_ev_tail = (s_ev_tail + 1) % EVQ_N;
    return 1;
}

int SDL_WaitEvent(SDL_Event *e)
{
    while (!SDL_PollEvent(e)) vTaskDelay(pdMS_TO_TICKS(5));
    return 1;
}

int SDL_PushEvent(SDL_Event *e)
{
    int nt = (s_ev_head + 1) % EVQ_N;
    if (nt == s_ev_tail) return 0;
    s_evq[s_ev_head] = *e;
    s_ev_head = nt;
    return 1;
}

/* SDL1.2 的 GetKeyState 返回 256 字节数组，这里是快照 */
Uint8 *SDL_GetKeyState(int *numkeys)
{
    if (numkeys) *numkeys = SDLK_LAST;
    return s_keystate;
}

int SDL_EnableKeyRepeat(int delay, int interval) { (void)delay; (void)interval; return 0; }

/* ================= 计时 ================= */
Uint32 SDL_GetTicks(void) { return (Uint32)(esp_timer_get_time() / 1000); }
void   SDL_Delay(Uint32 ms) { if (ms) vTaskDelay(pdMS_TO_TICKS(ms)); }

/* ================= RWops ================= */
static int rw_seek(SDL_RWops *ctx, int offset, int whence)
{
    FILE *f = (FILE *)ctx->hidden;
    int w = (whence == RW_SEEK_SET) ? SEEK_SET : (whence == RW_SEEK_CUR) ? SEEK_CUR : SEEK_END;
    if (fseek(f, offset, w) != 0) return -1;
    return (int)ftell(f);
}
static int rw_read(SDL_RWops *ctx, void *ptr, int size, int maxnum)
{
    FILE *f = (FILE *)ctx->hidden;
    if (size <= 0 || maxnum <= 0) return 0;
    return (int)fread(ptr, (size_t)size, (size_t)maxnum, f);
}
static int rw_write(SDL_RWops *ctx, const void *ptr, int size, int num)
{
    FILE *f = (FILE *)ctx->hidden;
    if (size <= 0 || num <= 0) return 0;
    return (int)fwrite(ptr, (size_t)size, (size_t)num, f);
}
static int rw_close(SDL_RWops *ctx)
{
    if (!ctx) return 0;
    if (ctx->hidden) fclose((FILE *)ctx->hidden);
    free(ctx);
    return 0;
}

SDL_RWops *SDL_RWFromFile(const char *file, const char *mode)
{
    if (!file || !mode) return NULL;
    FILE *f = fopen(file, mode);
    if (!f) {
        SDL_SetError("fopen(%s) failed: %d", file, errno);
        return NULL;
    }
    SDL_RWops *r = calloc(1, sizeof(SDL_RWops));
    if (!r) { fclose(f); return NULL; }
    r->seek = rw_seek; r->read = rw_read; r->write = rw_write; r->close = rw_close;
    r->hidden = f;
    return r;
}

int SDL_RWseek(SDL_RWops *ctx, int offset, int whence) { return ctx && ctx->seek ? ctx->seek(ctx, offset, whence) : -1; }
int SDL_RWtell(SDL_RWops *ctx) { return SDL_RWseek(ctx, 0, RW_SEEK_CUR); }
int SDL_RWread(SDL_RWops *ctx, void *p, size_t sz, size_t n) { return ctx && ctx->read ? ctx->read(ctx, p, (int)sz, (int)n) : 0; }
int SDL_RWwrite(SDL_RWops *ctx, const void *p, size_t sz, size_t n) { return ctx && ctx->write ? ctx->write(ctx, p, (int)sz, (int)n) : 0; }
int SDL_RWclose(SDL_RWops *ctx) { return ctx && ctx->close ? ctx->close(ctx) : 0; }

/* ================= 软件缩放 / 截图 ================= */
/* SDL_SoftStretch：SDL 自带的最近邻缩放。SDLPal 在"窗口尺寸 > 320x200"时会调它，
 * 我们只支持 8bpp，所以实现很直接（无 alpha/无混合）。 */
int SDL_SoftStretch(SDL_Surface *src, SDL_Rect *sr, SDL_Surface *dst, SDL_Rect *dr)
{
    if (!src || !dst || !src->pixels || !dst->pixels) return -1;
    int sx = sr ? sr->x : 0, sy = sr ? sr->y : 0;
    int sw = sr && sr->w ? sr->w : src->w;
    int sh = sr && sr->h ? sr->h : src->h;
    int dx = dr ? dr->x : 0, dy = dr ? dr->y : 0;
    int dw = dr && dr->w ? dr->w : dst->w;
    int dh = dr && dr->h ? dr->h : dst->h;
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return -1;
    if (sx + sw > src->w) sw = src->w - sx;
    if (sy + sh > src->h) sh = src->h - sy;
    if (dx + dw > dst->w) dw = dst->w - dx;
    if (dy + dh > dst->h) dh = dst->h - dy;

    for (int y = 0; y < dh; y++) {
        int yy = sy + (int)((long)y * sh / dh);
        const uint8_t *srow = (const uint8_t *)src->pixels + (size_t)yy * src->pitch;
        uint8_t *drow = (uint8_t *)dst->pixels + (size_t)(dy + y) * dst->pitch;
        for (int x = 0; x < dw; x++)
            drow[dx + x] = srow[sx + (int)((long)x * sw / dw)];
    }
    if (dst == &s_video) pal_port_mark_dirty();
    return 0;
}

/* 截图：ESP32 上没意义（也没有 BMP 的需求），直接返回成功不写文件 */
int SDL_SaveBMP(SDL_Surface *s, const char *file)
{
    (void)s; (void)file;
    return 0;
}
