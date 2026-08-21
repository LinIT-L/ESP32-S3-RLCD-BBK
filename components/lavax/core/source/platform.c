/*
 * platform.c — lavaxvm 平台呈现层 (st7305 单色适配版).
 *
 * 原 retro-go 版本用 RGBA double-buffered surface + lodepng mask + RGB565 抖动。
 * 本固件屏幕是 400x300 1bpp 单色 (ST7305), 灰度/彩色最终会丢失, 因此这里简化:
 *   - 保留 lavax_host_path (GBK→UTF8 路径翻译, 供 file.c 用)
 *   - present_indexed / present_rgb555 都落到一个 256×192 的缩略灰度缓冲
 *   - poll() 每帧取 input、跑硬件 tick、刷新显示、检测退出
 *   - 释放画面: 把缩略灰度缓冲放大并做阈值, 直接写进 st7305 的 1bpp fb
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "lavaxvm.h"
#include "rg_stub.h"
#include "esp_timer.h"
#include "esp_attr.h"

/* 固件显示/输入 */
#include "st7305.h"
#include "input.h"
#include "virtual_keys.h"

/* 与 lavaxvm 核心的接口 */
extern void LavaHardwareTick(unsigned int ticks);
extern void LavaHardwarePollInput(void);
extern void DmaRefresh(void);
/* 由 lavax_emu.c 提供: 检测固件退出键 (返回 true 请求退出游戏) */
extern bool lavax_check_exit(void);

/* st7305 句柄 (由 lavax_emu.c 在 bind_display 时传入) */
static st7305_handle_t *g_lcd = NULL;

/* 缩略灰度缓冲 (256×192), 每像素 0..255. 存 PSRAM (由 init 分配). */
static uint8_t *g_gray = NULL;

/* 帧去重哈希 (画面没变就不重画, 节省 SPI) */
static uint32_t s_frame_hash = 0;

static volatile int exit_requested;
static volatile int app_exit_requested;
static int app_exit_combo_down;
static int64_t hardware_tick_time;
static int64_t display_refresh_time;
static int64_t input_poll_time;
static int64_t task_yield_time;

/* 当前源图尺寸 */
static int s_cur_w, s_cur_h;

/* ---- 路径翻译 (保留原版 GBK→UTF8) ---- */

static int append_host_path(char *buffer, size_t size, size_t *length, const char *path)
{
    while (*path)
    {
        unsigned char first = (unsigned char)*path;

        if (first < 0x80)
        {
            if (*length + 1 >= size)
                return 0;
            buffer[(*length)++] = *path++;
        }
        else
        {
            unsigned int oem;
            unsigned int unicode;
            char encoded[4];
            size_t encoded_length;

            if (!path[1])
                return 0;
            oem = ((unsigned int)first << 8) | (unsigned char)path[1];
            unicode = lavax_gbk_oem2uni(oem);
            if (!unicode)
                return 0;
            encoded_length = rg_utf8_encode(encoded, (int)unicode);
            if (!encoded_length || *length + encoded_length >= size)
                return 0;
            memcpy(buffer + *length, encoded, encoded_length);
            *length += encoded_length;
            path += 2;
        }
    }
    buffer[*length] = 0;
    return 1;
}

const char *lavax_host_path(const char *path, char *buffer, size_t size)
{
    const char *relative = path;
    size_t length = 0;

    if (!path || !buffer || size == 0)
        return NULL;
    if (strncmp(relative, "fat:", 4) == 0)
        relative += 4;
    if (*relative != '/')
        return path;
    if (size < 4)
        return NULL;
    /* 宿主机路径根: /sd/<...> 或 /sdcard/<...> 均为固件真实路径.
     * 注意: 这些路径下可能是两种来源——
     *   菜单顶层加载给的是 UTF-8 中文名(先按原样存在则直接用);
     *   VM 内部(file.c c_fopen/my_chdir 等)给的是 GBK 拼出的路径(需 GBK→UTF8 转换).
     * 两者前缀相同无法仅靠内容区分, 故"先按原样 stat 是否真实存在":
     *   存在 → 原样透传(避免把 UTF-8 中文名再按 GBK 转译致损坏);
     *   不存在 → 回落到 GBK→UTF8 转换(处理 VM 内部拼出的路径).
     * LavaX 内部逻辑路径 (fat:/LavaXOS/... 或 /LAVA/...) 需补 /sdcard 前缀映射到 TF 卡根
     * (本固件 SD FAT 挂载在 /sdcard). */
    if (strncmp(relative, "/sd/", 4) == 0 ||
        strncmp(relative, "/sdcard/", 8) == 0)
    {
        size_t n = strlen(relative);
        if (n >= size) n = size - 1;
        memcpy(buffer, relative, n);
        buffer[n] = '\0';
        struct stat st;
        if (stat(buffer, &st) == 0)
            return buffer;   /* 原生 UTF-8 路径真实存在, 直接透传 */
        length = 0;          /* 不存在: 回落到 GBK→UTF8 转换 */
    }
    else
    {
        memcpy(buffer, "/sdcard", 7);
        length = 7;
    }
    return append_host_path(buffer, size, &length, relative) ? buffer : NULL;
}

/* ---- 呈现接口 ---- */

/* 供 lavax_emu.c 注入 st7305 句柄 + 灰度缓冲 */
void lavax_platform_set_display(st7305_handle_t *lcd, uint8_t *gray_buf)
{
    g_lcd = lcd;
    g_gray = gray_buf;
}

void lavax_platform_bind_display(void *surface0, void *surface1)
{
    (void)surface0; (void)surface1; /* 占位; 句柄由 lavax_emu.c 注入 */
}

/* 设置当前运行的 .lav 程序路径 (原版会据此装载同名的 .png app mask).
 * 本固件不需要 mask, 仅需存在定义 (被 lava.c/main.c 调用). */
void lavax_platform_set_app_path(const char *path)
{
    (void)path;
}

void lavax_platform_redraw(void)
{
    s_frame_hash = 0;
}

static void present_to_gray(const uint8_t *pixels, int width, int height, int stride,
                            const uint8_t *palette)
{
    int y, x;

    if (!g_gray || !pixels || width <= 0 || height <= 0)
        return;
    memset(g_gray, 0, 256 * 192);
    s_cur_w = (width > 256) ? 256 : width;
    s_cur_h = (height > 192) ? 192 : height;
    for (y = 0; y < s_cur_h; y++)
    {
        for (x = 0; x < s_cur_w; x++)
        {
            uint8_t idx = pixels[y * stride + x];
            int lum = 0;
            if (palette)
                lum = (palette[idx * 3] * 30 + palette[idx * 3 + 1] * 59 +
                       palette[idx * 3 + 2] * 11) / 100;
            else
                lum = idx;
            g_gray[y * 256 + x] = (uint8_t)lum;
        }
    }
}

void lavax_platform_present_indexed(const uint8_t *pixels, int width, int height, int stride,
                                    const uint8_t *palette, int graph_mode,
                                    int canvas_width, int canvas_height, int display_scale,
                                    int mask_enabled)
{
    (void)canvas_width; (void)canvas_height; (void)display_scale; (void)mask_enabled;
    (void)graph_mode;
    present_to_gray(pixels, width, height, stride, palette);
}

void lavax_platform_present_rgb555(const uint16_t *pixels, int width, int height, int stride,
                                   int canvas_width, int canvas_height, int display_scale,
                                   int mask_enabled)
{
    int y, x;
    EXT_RAM_BSS_ATTR static uint8_t tmp[256 * 192];   /* 灰度转换临时缓冲 (放 PSRAM, 不占内部 SRAM) */
    (void)canvas_width; (void)canvas_height; (void)display_scale; (void)mask_enabled;
    if (!pixels || width <= 0 || height <= 0)
        return;
    for (y = 0; y < height && y < 192; y++)
        for (x = 0; x < width && x < 256; x++)
        {
            uint16_t c = pixels[y * stride + x];
            int r = (c >> 10) & 31, gc = (c >> 5) & 31, b = c & 31;
            tmp[y * 256 + x] = (uint8_t)((r * 30 + gc * 59 + b * 11) * 255 / (31 * 100));
        }
    present_to_gray(tmp, (width > 256) ? 256 : width, (height > 192) ? 192 : height, 256, NULL);
}

/* ---- 把灰度缓冲画出: 阈值→放大→1bpp 打包→st7305 blit ---- */
static void render_to_lcd(void)
{
    static uint8_t *fb_rows = NULL;   /* (400/8)*300 1bpp 行缓冲 */
    static int fb_cap = 0;
    int sw, sh, sx, sy, scale, y;

    if (!g_lcd || !g_gray || s_cur_w <= 0 || s_cur_h <= 0)
        return;

    /* 保持纵横比放大(整数倍)到 ≤400×300 内并居中 */
    scale = 1;
    while ((s_cur_w * (scale + 1)) <= 400 && (s_cur_h * (scale + 1)) <= 300)
        scale++;
    sw = s_cur_w * scale;
    sh = s_cur_h * scale;
    sx = (400 - sw) / 2;
    sy = (300 - sh) / 2;

    /* 扩充分配缓冲 (整帧 1bpp) */
    if (fb_cap < sh * ((sw + 7) / 8))
    {
        free(fb_rows);
        fb_rows = malloc((size_t)sh * ((sw + 7) / 8));
        fb_cap = (int)((size_t)sh * ((sw + 7) / 8));
    }
    if (!fb_rows)
        return;

    for (y = 0; y < sh; y++)
    {
        int syi = y / scale;
        uint8_t *dst = fb_rows + (size_t)y * ((sw + 7) / 8);
        int b;
        for (b = 0; b < (sw + 7) / 8; b++)
        {
            uint8_t byte = 0;
            int k;
            for (k = 0; k < 8; k++)
            {
                int xx = b * 8 + k;
                if (xx >= sw)
                    break;
                /* 反射屏: 暗(dark=>黑)置位, 亮(bright=>白)留0. 修 V1.0.9x: 原写反导致整幅黑屏 */
                if (g_gray[syi * 256 + (xx / scale)] < 90)
                    byte |= (0x80 >> k);
            }
            dst[b] = byte;
        }
    }
    st7305_blit_1bit(g_lcd, sx, sy, sw, sh, fb_rows);
}

/* ---- 轮询: 输入 / tick / 显示 / 退出 ---- */

extern bool menu_wqx_confirm_asked(void);   /* host: 文曲星退出确认浮层是否激活 */
extern void lavax_draw_exit_confirm(st7305_handle_t *lcd);   /* host: 绘制退出确认浮层 */

void lavax_platform_poll(void)
{
    int64_t now = (int64_t)esp_timer_get_time();
    /* V1.0.9x: 运行性能打点 — 每 5s 上报主循环迭代次数与单次耗时, 用于定位卡顿(VM vs 渲染) */
    {
        static int64_t lastlog = 0; static uint32_t nloop = 0; static int64_t t0 = 0;
        if (!t0) { t0 = now; }
        nloop++;
        if (now - lastlog >= 5000000 && nloop > 0) {
            double hertz = (double)nloop * 1e6 / (double)(now - lastlog);
            int64_t tot = now - t0;
            printf("[LAVAX] loops=%u freq=%.0f/s avg=%lldus loop\n", nloop, hertz, (long long)(nloop ? tot / nloop : 0));
            lastlog = now; nloop = 0; t0 = now;
        }
    }
    /* 输入采样 (~5ms) */
    if (!input_poll_time || now - input_poll_time >= 5000)
    {
        LavaHardwarePollInput();
        input_poll_time = now;
    }

    /* 退出: 由宿主/物理键提供 */
    if (lavax_check_exit())
        exit_requested = 1;

    /* 硬件 tick (每秒 256 tick) */
    if (!hardware_tick_time)
        hardware_tick_time = now;
    while (now - hardware_tick_time >= (1000000 / 256))
    {
        LavaHardwareTick(1);
        hardware_tick_time += (1000000 / 256);
    }

    /* 显示刷新 (~15fps) */
    if (!display_refresh_time || now - display_refresh_time >= 66000)
    {
        DmaRefresh();
        if (g_gray && s_cur_w > 0)
        {
            uint32_t h = 2166136261u;
            int i, x;
            for (i = 0; i < s_cur_h; i++)
                for (x = 0; x < s_cur_w; x++)
                    h = (h * 16777619u) ^ g_gray[i * 256 + x];
            /* V1.0.9x: 退出确认浮层激活时也强制送屏, 保证"按返回"立即显示确认框 */
            if (h != s_frame_hash || menu_wqx_confirm_asked())
            {
                if (h != s_frame_hash)
                {
                    s_frame_hash = h;
                    render_to_lcd();
                }
                virtual_keys_draw(g_lcd);   /* 叠加屏幕虚拟按键 (标准布局) */
                lavax_draw_exit_confirm(g_lcd);  /* V1.0.9x: 叠加退出确认浮层 */
                st7305_flush(g_lcd);
            }
        }
        display_refresh_time = now;
    }

    /* 退出: 由宿主/物理键提供 (hardware.c 的 lavax_platform_should_exit) */
    if (!task_yield_time || now - task_yield_time >= 2000)
    {
        rg_task_yield();
        task_yield_time = now;
    }
}

int lavax_platform_should_exit(void)
{
    return exit_requested;
}

int lavax_platform_take_app_exit_request(void)
{
    int requested = app_exit_requested;
    app_exit_requested = 0;
    return requested;
}

int lavax_platform_app_exit_combo_down(void)
{
    return app_exit_combo_down;
}

/* 供宿主调用: 请求退出 */
void lavax_platform_request_exit(void)
{
    exit_requested = 1;
}

/* 供宿主调用: 复位退出/组合状态 (开始新一轮运行前) */
void lavax_platform_reset(void)
{
    exit_requested = 0;
    app_exit_requested = 0;
    app_exit_combo_down = 0;
    hardware_tick_time = 0;
    display_refresh_time = 0;
    input_poll_time = 0;
    task_yield_time = 0;
    s_frame_hash = 0;
}