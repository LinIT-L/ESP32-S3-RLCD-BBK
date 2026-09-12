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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* 固件显示/输入 */
#include "st7305.h"
#include "input.h"
#include "virtual_keys.h"
#include "cheat_float.h"

/* 与 lavaxvm 核心的接口 */
extern void LavaHardwareTick(unsigned int ticks);
extern void LavaHardwarePollInput(void);
extern void DmaRefresh(void);
/* 由 lavax_emu.c 提供: 检测固件退出键 (返回 true 请求退出游戏) */
extern bool lavax_check_exit(void);
/* 由 lavax_emu.c 提供: 修改机热键检测 + 呼出共享UI (每帧调用) */
extern void lavax_emu_cheat_poll(void);

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

/* 游戏速度倍速: 硬件 tick 每秒 256 次, 每次推进 lavax_game_speed 个内部 tick,
 * 使 LavaX 游戏内基于 Hz128/TickCount 的 delay/延迟/动画节流同步加快.
 * 默认 ×8 (与步步高 gam4980 的 timer_rate≈8 等效). 可被菜单/NVS 覆盖. */
int lavax_game_speed = 8;

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

static int lavax_shared_lavadata(const char *mapped, char *buffer, size_t size); /* fwd */

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
    if (!append_host_path(buffer, size, &length, relative)) return NULL;
    /* V1.x: 共用 LavaData 回退(游戏自带数据缺文件时) */
    if (lavax_shared_lavadata(buffer, buffer, size)) return buffer;
    return buffer;
}

/* 共用 LavaData 回退: 游戏自带 LavaData 缺失某数据文件时, 尝试从
 * /sdcard/lava/LavaData/<basename> 取(需先存在该共用目录). 命中返回 1 且改写 buffer. */
static int lavax_shared_lavadata(const char *mapped, char *buffer, size_t size)
{
    if (!strstr(mapped, "/LavaData/")) return 0;
    const char *base = strrchr(mapped, '/');
    if (!base || !base[1]) return 0;
    char fb[192];
    int n = snprintf(fb, sizeof fb, "/sdcard/lava/LavaData/%s", base + 1);
    if (n <= 0 || (size_t)n >= sizeof fb) return 0;
    struct stat st;
    if (stat(fb, &st) != 0) return 0;
    snprintf(buffer, size, "%s", fb);
    return 1;
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
                            const uint8_t *palette, int graph_mode)
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
            int lum;
            /* V1.0.9x 反色修复: 按 graph_mode 映射灰度(步步高反射屏).
             * gm1: 0=白/1=黑; gm4: 0白..15黑; 其余走调色板. 避免切模式后调色板
             * 未重建导致"清成0/255的白底被渲染成黑". */
            if (graph_mode == 1)
                lum = (idx == 1) ? 0 : 255;
            else if (graph_mode == 4)
                lum = 255 * (15 - (idx & 15)) / 15;
            else if (palette)
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
    present_to_gray(pixels, width, height, stride, palette, graph_mode);
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
    present_to_gray(tmp, (width > 256) ? 256 : width, (height > 192) ? 192 : height, 256, NULL, 8);
}

/* ---- 显示模式 (接引擎优化 s_edm): 0=点对点 1=全屏(等比放大铺满/裁边) 2=拉伸(变形填满400x300) ---- */
static int s_disp_mode = 1;
void lavax_platform_set_display_mode(int m){ s_disp_mode = m; }

/* ---- 把灰度缓冲画出: 浮点缩放→1bpp 打包→st7305 blit (支持非整数放大铺满) ---- */
static void render_to_lcd(void)
{
    static uint8_t *rows = NULL;
    static int cap = 0;
    int dw = 400, dh = 300;

    if (!g_lcd || !g_gray || s_cur_w <= 0 || s_cur_h <= 0)
        return;

    st7305_clear(g_lcd, ST7305_COLOR_WHITE);

    /* 输出区域 (sw x sh, 位于屏幕内) */
    float k = 1.0f;                 /* 全屏/点对点: 等比系数 */
    int   kx = 1, ky = 1;           /* 拉伸: 每轴系数 */
    int   sw = s_cur_w, sh = s_cur_h, sx = 0, sy = 0;
    if (s_disp_mode == 1) {         /* 全屏: 等比 cover 填满 (放大到最大) */
        float ca = (float)dw / s_cur_w, cb = (float)dh / s_cur_h;
        k = (ca > cb) ? ca : cb;
        sw = (int)(s_cur_w * k); sh = (int)(s_cur_h * k);
        sx = (dw - sw) / 2; sy = (dh - sh) / 2;
    } else if (s_disp_mode == 2) {  /* 拉伸: 变形填满整屏 */
        sw = dw; sh = dh; sx = 0; sy = 0;
        kx = (dw + s_cur_w - 1) / s_cur_w; ky = (dh + s_cur_h - 1) / s_cur_h;
    } else {                        /* 点对点: 1:1 居中 */
        sx = (dw - sw) / 2; sy = (dh - sh) / 2;
    }

    int rowb = (dw + 7) / 8;
    if (cap < dh * rowb) { free(rows); rows = (uint8_t*)malloc((size_t)dh * rowb); cap = dh * rowb; }
    if (!rows) return;

    for (int y = 0; y < dh; y++) {
        uint8_t *dst = rows + (size_t)y * rowb;
        for (int b = 0; b < rowb; b++) {
            uint8_t byte = 0;
            for (int kk = 0; kk < 8; kk++) {
                int x = b * 8 + kk; if (x >= dw) break;
                if (y < sy || y >= sy + sh || x < sx || x >= sx + sw) continue; /* 留白 */
                int syi, sxi;
                if (s_disp_mode == 2) { syi = (y - sy) / ky; sxi = (x - sx) / kx; }
                else                  { syi = (int)((y - sy) / k); sxi = (int)((x - sx) / k); }
                syi = syi < 0 ? 0 : (syi >= s_cur_h ? s_cur_h - 1 : syi);
                sxi = sxi < 0 ? 0 : (sxi >= s_cur_w ? s_cur_w - 1 : sxi);
                if (g_gray[syi * 256 + sxi] < 90) byte |= (0x80 >> kk);
            }
            dst[b] = byte;
        }
    }
    st7305_blit_1bit(g_lcd, 0, 0, dw, dh, rows);
}

/* ---- 轮询: 输入 / tick / 显示 / 退出 ---- */

extern bool menu_wqx_confirm_asked(void);   /* host: 文曲星退出确认浮层是否激活 */
extern void lavax_draw_exit_confirm(st7305_handle_t *lcd);   /* host: 绘制退出确认浮层 */

void lavax_platform_poll(void)
{
    /* 修改机菜单打开: 冻结本 VM (独立修改机任务在 core0 跑菜单, 这里阻塞等释放) */
    while (cheat_float_is_frozen()) {
        vTaskDelay(pdMS_TO_TICKS(16));
    }

    /* 修改机热键 (每帧): 按住 SELECT+START 呼出. 阻塞在 cheat_ui_open 期间 VM 停跑. */
    lavax_emu_cheat_poll();

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

    /* 硬件 tick (每秒 256 tick); 每 tick 推进 lavax_game_speed 次 → 游戏速度倍速 */
    if (!hardware_tick_time)
        hardware_tick_time = now;
    while (now - hardware_tick_time >= (1000000 / 256))
    {
        LavaHardwareTick(lavax_game_speed > 0 ? lavax_game_speed : 1);
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
                int64_t r0 = esp_timer_get_time();
                if (h != s_frame_hash)
                {
                    s_frame_hash = h;
                    render_to_lcd();
                }
                virtual_keys_draw(g_lcd);   /* 叠加屏幕虚拟按键 (标准布局) */
                bool takeover = cheat_float_overlay_tick(g_lcd); /* 浮标+检测呼出 */
                lavax_draw_exit_confirm(g_lcd);  /* V1.0.9x: 叠加退出确认浮层 */
                if (!takeover) st7305_flush(g_lcd);   /* 触发接管则本帧不再送屏 */
                int64_t r1 = esp_timer_get_time();
                if (r1 - r0 > 20000) /* >20ms 才打, 避免刷屏 */
                    printf("[LAVAX-RENDER] frame total=%lld ms\n", (long long)((r1 - r0) / 1000));
            }
        }
        display_refresh_time = now;

        /* V1.0.9x 黑屏诊断: 每 ~3s 打印灰度缓冲非黑像素数, 分辨是"引擎没画"还是"画了但屏黑" */
        {
            static int64_t s_diag_t = 0;
            int di, dx;
            if (!s_diag_t) s_diag_t = now;
            if (now - s_diag_t >= 3000000)
            {
                int nb = 0, sum = 0, cnt = 0;
                for (di = 0; di < s_cur_h; di++)
                    for (dx = 0; dx < s_cur_w; dx++)
                    {
                        int v = g_gray[di * 256 + dx];
                        if (v > 8) nb++;
                        sum += v;
                        cnt++;
                    }
                printf("[LAVAX-DIAG] frame=0x%08lX gray_nonblack=%d/%d avgLum=%d cur=%dx%d\n",
                       (unsigned long)s_frame_hash, nb, cnt, cnt ? sum / cnt : 0, s_cur_w, s_cur_h);
                s_diag_t = now;
            }
        }
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