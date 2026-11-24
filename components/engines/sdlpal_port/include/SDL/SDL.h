/* SDL.h — SDLPal 用的 SDL 1.2 API 子集（ESP32-S3 自实现，不依赖真 SDL）
 *
 * 为什么是 1.2：SDLPal 的 video.c 同时有 1.2 / 2.x / 3.x 三条路，
 *   1.2 这条只需 "拿 Surface->pixels 直接改像素 + SetPalette + UpdateRect"，
 *   正好接我们自己的 8bpp -> 1bpp 单色上屏；2.x 那条要 Renderer/Texture/GL。
 *
 * 实现文件：sdl_shim.c（video/event/timer/rwops）
 *          sdl_audio.c（OpenAudio + 回调 -> audio_player_feed_pcm）
 */
#ifndef SDL_H
#define SDL_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================= 基础类型 ================= */
typedef uint8_t  Uint8;
typedef int8_t   Sint8;
typedef uint16_t Uint16;
typedef int16_t  Sint16;
typedef uint32_t Uint32;
typedef int32_t  Sint32;
typedef uint8_t  SDL_bool;

#define SDL_TRUE   1
#define SDL_FALSE  0
#define SDLCALL                     /* 无调用约定修饰 */

/* 声明自己是 SDL 1.2：SDLPal 的 sdl_compat.h / main.c 用这些宏分支 */
#define SDL_MAJOR_VERSION 1
#define SDL_MINOR_VERSION 2
#define SDL_PATCHLEVEL    15

/* ================= 视频 ================= */
typedef struct SDL_Rect {
    Sint16 x, y;
    Uint16 w, h;
} SDL_Rect;

typedef struct SDL_Color {
    Uint8 r, g, b, unused;
} SDL_Color;

typedef struct SDL_Palette {
    int      ncolors;
    SDL_Color colors[256];
} SDL_Palette;

typedef struct SDL_PixelFormat {
    SDL_Palette *palette;
    Uint8  BitsPerPixel;
    Uint8  BytesPerPixel;
    Uint32 Rmask, Gmask, Bmask, Amask;
} SDL_PixelFormat;

typedef struct SDL_Surface {
    Uint32 flags;
    SDL_PixelFormat *format;
    int    w, h;
    Uint16 pitch;
    void  *pixels;
    SDL_Rect clip_rect;
    int    refcount;
    /* 私有：内部像素缓冲（真 pixels 指向这里） */
    void  *_own;
} SDL_Surface;

/* 标志 */
#define SDL_SWSURFACE   0x00000000
#define SDL_HWSURFACE   0x00000001
#define SDL_ASYNCBLIT   0x00000004
#define SDL_ANYFORMAT   0x10000000
#define SDL_HWPALETTE   0x20000000
#define SDL_DOUBLEBUF   0x40000000
#define SDL_FULLSCREEN  0x80000000
#define SDL_RESIZABLE   0x00000010
#define SDL_NOFRAME     0x00000020

#define SDL_LOGPAL      0x01
#define SDL_PHYSPAL     0x02

/* ================= 事件 ================= */
typedef enum {
    SDL_NOEVENT = 0,
    SDL_ACTIVEEVENT,
    SDL_KEYDOWN,
    SDL_KEYUP,
    SDL_MOUSEMOTION,
    SDL_MOUSEBUTTONDOWN,
    SDL_MOUSEBUTTONUP,
    SDL_JOYAXISMOTION,
    SDL_JOYBALLMOTION,
    SDL_JOYHATMOTION,
    SDL_JOYBUTTONDOWN,
    SDL_JOYBUTTONUP,
    SDL_QUIT,
    SDL_SYSWMEVENT,
    SDL_VIDEORESIZE = 16,
    SDL_VIDEOEXPOSE = 17,
    SDL_USEREVENT = 24
} SDL_EventType;

typedef struct SDL_keysym {
    Uint8 scancode;
    int   sym;
    int   mod;
    Uint16 unicode;
} SDL_keysym;

typedef struct SDL_KeyboardEvent {
    Uint8 type;
    Uint8 which;
    Uint8 state;
    SDL_keysym keysym;
} SDL_KeyboardEvent;

typedef struct SDL_MouseMotionEvent {
    Uint8 type;
    Uint8 which;
    Uint8 state;
    Uint16 x, y;
    Sint16 xrel, yrel;
} SDL_MouseMotionEvent;

typedef struct SDL_MouseButtonEvent {
    Uint8 type;
    Uint8 which;
    Uint8 button;
    Uint8 state;
    Uint16 x, y;
} SDL_MouseButtonEvent;

typedef struct SDL_QuitEvent { Uint8 type; } SDL_QuitEvent;

typedef struct SDL_ResizeEvent {
    Uint8 type;
    int   w, h;
} SDL_ResizeEvent;

typedef union SDL_Event {
    Uint8 type;
    SDL_KeyboardEvent     key;
    SDL_MouseMotionEvent  motion;
    SDL_MouseButtonEvent  button;
    SDL_QuitEvent         quit;
    SDL_ResizeEvent       resize;
} SDL_Event;

/* SDL_mixer 的常量（SDLPal 的音效混音用，值同 SDL_mixer 默认） */
#define SDL_MIX_MAXVOLUME 128

/* 我们的 Surface 永远在 RAM 里，不需要加锁 */
#define SDL_MUSTLOCK(s) 0

int  SDL_SoftStretch(SDL_Surface *src, SDL_Rect *srcrect, SDL_Surface *dst, SDL_Rect *dstrect);
int  SDL_SaveBMP(SDL_Surface *surface, const char *file);

/* 修饰键 */
#define KMOD_NONE   0x0000
#define KMOD_LSHIFT 0x0001
#define KMOD_RSHIFT 0x0002
#define KMOD_LCTRL  0x0040
#define KMOD_RCTRL  0x0080
#define KMOD_LALT   0x0100
#define KMOD_RALT   0x0200
#define KMOD_ALT    (KMOD_LALT | KMOD_RALT)
#define KMOD_CTRL   (KMOD_LCTRL | KMOD_RCTRL)
#define KMOD_SHIFT  (KMOD_LSHIFT | KMOD_RSHIFT)

/* 鼠标按键 */
#define SDL_BUTTON_LEFT   1
#define SDL_BUTTON_MIDDLE 2
#define SDL_BUTTON_RIGHT  3

/* ================= 键盘符号（SDL 1.2 常用子集）================= */
#define SDLK_UNKNOWN   0
#define SDLK_FIRST     0
#define SDLK_BACKSPACE 8
#define SDLK_TAB       9
#define SDLK_CLEAR     12
#define SDLK_RETURN    13
#define SDLK_PAUSE     19
#define SDLK_ESCAPE    27
#define SDLK_SPACE     32
#define SDLK_EXCLAIM   33
#define SDLK_QUOTEDBL  34
#define SDLK_HASH      35
#define SDLK_DOLLAR    36
#define SDLK_AMPERSAND 38
#define SDLK_QUOTE     39
#define SDLK_LEFTPAREN 40
#define SDLK_RIGHTPAREN 41
#define SDLK_ASTERISK  42
#define SDLK_PLUS      43
#define SDLK_COMMA     44
#define SDLK_MINUS     45
#define SDLK_PERIOD    46
#define SDLK_SLASH     47
#define SDLK_0 48
#define SDLK_1 49
#define SDLK_2 50
#define SDLK_3 51
#define SDLK_4 52
#define SDLK_5 53
#define SDLK_6 54
#define SDLK_7 55
#define SDLK_8 56
#define SDLK_9 57
#define SDLK_COLON     58
#define SDLK_SEMICOLON 59
#define SDLK_LESS      60
#define SDLK_EQUALS    61
#define SDLK_GREATER   62
#define SDLK_QUESTION  63
#define SDLK_AT        64
#define SDLK_LEFTBRACKET  91
#define SDLK_BACKSLASH    92
#define SDLK_RIGHTBRACKET 93
#define SDLK_CARET     94
#define SDLK_UNDERSCORE 95
#define SDLK_BACKQUOTE 96
#define SDLK_a 97
#define SDLK_b 98
#define SDLK_c 99
#define SDLK_d 100
#define SDLK_e 101
#define SDLK_f 102
#define SDLK_g 103
#define SDLK_h 104
#define SDLK_i 105
#define SDLK_j 106
#define SDLK_k 107
#define SDLK_l 108
#define SDLK_m 109
#define SDLK_n 110
#define SDLK_o 111
#define SDLK_p 112
#define SDLK_q 113
#define SDLK_r 114
#define SDLK_s 115
#define SDLK_t 116
#define SDLK_u 117
#define SDLK_v 118
#define SDLK_w 119
#define SDLK_x 120
#define SDLK_y 121
#define SDLK_z 122
#define SDLK_DELETE    127
#define SDLK_KP0       256
#define SDLK_KP1       257
#define SDLK_KP2       258
#define SDLK_KP3       259
#define SDLK_KP4       260
#define SDLK_KP5       261
#define SDLK_KP6       262
#define SDLK_KP7       263
#define SDLK_KP8       264
#define SDLK_KP9       265
#define SDLK_KP_PERIOD 266
#define SDLK_KP_DIVIDE 267
#define SDLK_KP_MULTIPLY 268
#define SDLK_KP_MINUS  269
#define SDLK_KP_PLUS   270
#define SDLK_KP_ENTER  271
#define SDLK_KP_EQUALS 272
#define SDLK_UP        273
#define SDLK_DOWN      274
#define SDLK_RIGHT     275
#define SDLK_LEFT      276
#define SDLK_INSERT    277
#define SDLK_HOME      278
#define SDLK_END       279
#define SDLK_PAGEUP    280
#define SDLK_PAGEDOWN  281
#define SDLK_F1        282
#define SDLK_F2        283
#define SDLK_F3        284
#define SDLK_F4        285
#define SDLK_F5        286
#define SDLK_F6        287
#define SDLK_F7        288
#define SDLK_F8        289
#define SDLK_F9        290
#define SDLK_F10       291
#define SDLK_F11       292
#define SDLK_F12       293
#define SDLK_NUMLOCK   300
#define SDLK_CAPSLOCK  301
#define SDLK_SCROLLOCK 302
#define SDLK_RSHIFT    303
#define SDLK_LSHIFT    304
#define SDLK_RCTRL     305
#define SDLK_LCTRL     306
#define SDLK_RALT      307
#define SDLK_LALT      308
#define SDLK_LAST      512

/* ================= 初始化 ================= */
#define SDL_INIT_TIMER    0x00000001
#define SDL_INIT_AUDIO    0x00000010
#define SDL_INIT_VIDEO    0x00000020
#define SDL_INIT_EVENTS   0x00004000
#define SDL_INIT_EVERYTHING 0x0000FFFF

int  SDL_Init(Uint32 flags);
void SDL_Quit(void);
const char *SDL_GetError(void);
void SDL_SetError(const char *fmt, ...);

/* ================= 视频 API ================= */
SDL_Surface *SDL_SetVideoMode(int width, int height, int bpp, Uint32 flags);
SDL_Surface *SDL_GetVideoSurface(void);
int  SDL_LockSurface(SDL_Surface *s);
void SDL_UnlockSurface(SDL_Surface *s);
void SDL_UpdateRect(SDL_Surface *s, Sint32 x, Sint32 y, Uint32 w, Uint32 h);
void SDL_UpdateRects(SDL_Surface *s, int numrects, SDL_Rect *rects);
int  SDL_Flip(SDL_Surface *s);
int  SDL_SetPalette(SDL_Surface *s, int flags, SDL_Color *colors, int first, int n);
int  SDL_SetColors(SDL_Surface *s, SDL_Color *colors, int first, int n);
Uint32 SDL_MapRGB(const SDL_PixelFormat *fmt, Uint8 r, Uint8 g, Uint8 b);
SDL_Surface *SDL_CreateRGBSurface(Uint32 flags, int w, int h, int bpp,
                                  Uint32 Rmask, Uint32 Gmask, Uint32 Bmask, Uint32 Amask);
void SDL_FreeSurface(SDL_Surface *s);
int  SDL_FillRect(SDL_Surface *dst, SDL_Rect *dstrect, Uint32 color);
int  SDL_BlitSurface(SDL_Surface *src, SDL_Rect *srcrect, SDL_Surface *dst, SDL_Rect *dstrect);
SDL_Surface *SDL_DisplayFormat(SDL_Surface *s);
void SDL_WM_SetCaption(const char *title, const char *icon);
int  SDL_ShowCursor(int toggle);

/* ================= 事件 API ================= */
int  SDL_PollEvent(SDL_Event *e);
int  SDL_WaitEvent(SDL_Event *e);
int  SDL_PushEvent(SDL_Event *e);

/* ---- 平台侧（物理按键 / 触摸 / 虚拟手柄）注入事件 ---- */
void SDL_PushKeyEvent(int sym, int down);
void SDL_PushMouseEvent(int type, int x, int y, int button);
Uint8 *SDL_GetKeyState(int *numkeys);
int  SDL_EnableKeyRepeat(int delay, int interval);
void SDL_PumpEvents(void);

/* ================= 计时 ================= */
Uint32 SDL_GetTicks(void);
void   SDL_Delay(Uint32 ms);

/* ================= 音频 ================= */
#define AUDIO_U8     0x0008
#define AUDIO_S8     0x8008
#define AUDIO_U16LSB 0x0010
#define AUDIO_S16LSB 0x8010
#define AUDIO_U16MSB 0x1010
#define AUDIO_S16MSB 0x9010
#define AUDIO_U16    AUDIO_U16LSB
#define AUDIO_S16    AUDIO_S16LSB
#define AUDIO_S16SYS AUDIO_S16LSB

typedef void (*SDL_AudioCallback)(void *userdata, Uint8 *stream, int len);

typedef struct SDL_AudioSpec {
    int   freq;
    Uint16 format;
    Uint8  channels;
    Uint8  silence;
    Uint16 samples;
    Uint32 size;
    SDL_AudioCallback callback;
    void  *userdata;
} SDL_AudioSpec;

int  SDL_OpenAudio(SDL_AudioSpec *desired, SDL_AudioSpec *obtained);
void SDL_CloseAudio(void);
void SDL_PauseAudio(int pause_on);
void SDL_LockAudio(void);
void SDL_UnlockAudio(void);

/* ================= RWops（文件）================= */
typedef struct SDL_RWops {
    int   (SDLCALL *seek)(struct SDL_RWops *ctx, int offset, int whence);
    int   (SDLCALL *read)(struct SDL_RWops *ctx, void *ptr, int size, int maxnum);
    int   (SDLCALL *write)(struct SDL_RWops *ctx, const void *ptr, int size, int num);
    int   (SDLCALL *close)(struct SDL_RWops *ctx);
    void  *hidden;
} SDL_RWops;

#define SDL_RW_SEEK_SET 0
#define SDL_RW_SEEK_CUR 1
#define SDL_RW_SEEK_END 2
#define RW_SEEK_SET 0
#define RW_SEEK_CUR 1
#define RW_SEEK_END 2

SDL_RWops *SDL_RWFromFile(const char *file, const char *mode);
int  SDL_RWseek(SDL_RWops *ctx, int offset, int whence);
int  SDL_RWtell(SDL_RWops *ctx);
int  SDL_RWread(SDL_RWops *ctx, void *ptr, size_t size, size_t maxnum);
int  SDL_RWwrite(SDL_RWops *ctx, const void *ptr, size_t size, size_t num);
int  SDL_RWclose(SDL_RWops *ctx);

/* ================= 工具宏 ================= */
#define SDL_SwapLE16(x) (x)
#define SDL_SwapLE32(x) (x)
#define SDL_strcasecmp   strcasecmp
#define SDL_strncasecmp  strncasecmp
#define SDL_snprintf     snprintf
#define SDL_vsnprintf    vsnprintf
#define SDL_sprintf      sprintf
#define SDL_strlen       strlen
#define SDL_strcmp       strcmp
#define SDL_strncmp      strncmp
#define SDL_strcpy       strcpy
#define SDL_strncpy      strncpy
#define SDL_strcat       strcat
#define SDL_strncat      strncat
#define SDL_strchr       strchr
#define SDL_strrchr      strrchr
#define SDL_strstr       strstr
#define SDL_strdup       strdup
#define SDL_strtol       strtol
#define SDL_strtoul      strtoul
#define SDL_atoi         atoi
#define SDL_atof         atof
#define SDL_memcpy       memcpy
#define SDL_memmove      memmove
#define SDL_memset       memset
#define SDL_memcmp       memcmp
#define SDL_isspace      isspace
#define SDL_isdigit      isdigit
#define SDL_isalpha      isalpha
#define SDL_isalnum      isalnum
#define SDL_toupper      toupper
#define SDL_tolower      tolower

#define SDL_VERSION_ATLEAST(X, Y, Z)  0    /* 强制走 SDL 1.2 代码路径 */
#define SDL_TICKS_PASSED(A, B)        ((Sint32)((B) - (A)) <= 0)

#ifdef __cplusplus
}
#endif
#endif /* SDL_H */
