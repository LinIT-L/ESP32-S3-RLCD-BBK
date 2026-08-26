/**
 * @file board_shim.c
 * @brief board_rlcd + board_speaker 兼容层实现
 *
 * 适配 gb_emu (Peanut-GB) 到当前项目的 st7305 + audio_player.
 * 直接使用参考项目 (esp32-s3-rlcd-gb-emulator) 的显示方案:
 *   - GB: 2bit 色阶 (0-3) → 2x2 抖动 → 1bit, 2x 缩放
 *
 * 音频适配:
 *   - board_speaker_write → audio_player_feed_pcm (int16_t 立体声)
 */
#pragma GCC optimize ("O3")
#include "board_rlcd.h"
#include "board_speaker.h"
#include "audio_player.h"
#include "virtual_keys.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "board_shim";

/* NES 1x 逐行绘制到指定 fb (供视频任务内部使用) */
esp_err_t board_rlcd_draw_nes_line_1x_to(uint8_t *fb, int x, int y,
                                        const uint8_t *pixels, int width);
esp_err_t board_rlcd_draw_gbc_line_2x_rgb565_be(int x, int y, const uint16_t *pixels, int width);
esp_err_t board_rlcd_draw_gbc_line_2x_rgb565_be_to(uint8_t *fb, int x, int y,
                                                   const uint16_t *pixels, int width);
static uint8_t board_rlcd_rgb565_be_to_gbc_shade(uint16_t rgb565_be);
esp_err_t board_rlcd_draw_gbc_stretch(const uint16_t *buf_le, int src_w, int src_h);
esp_err_t board_rlcd_draw_gb_line_to(uint8_t *fb, int x, int y,
                                     const uint8_t *pixels, int width);

/* V1.0.46+: GB/GBC 灰度模式三档开关:
 *   0 = 纯黑白 (简单 2x 放大, shade>=2 为黑)
 *   1 = 4档点聚灰度 (shade 0/1/2/3 -> 黑点数 0/1/2/4)
 *   2 = 5档点聚灰度 (shade 0/1/2/3 -> 黑点数 0/1/3/4, 深档用3个黑点)
 * 点聚(clustered): 黑点聚成实心块, 静态不发抖, 避免 1bit 屏幕低刷新率下抖动闪烁. */
static int g_gb_gray = 1;

/* NES 全屏缩放: 2bit 打包帧 (每字节 4 像素) + 内部 RAM 显示缓冲.
 * rowbits[local_y][py_parity][shade_pair] 预先把"2 个源像素 -> 2 个物理位"
 * 合成字节片段, 视频任务每字节只需 1 次查表 + 1 次 OR, 不再逐像素查抖动图案.
 * 定义放在抖动表之后, 这里只做前向声明. */
#define NES_FB_BYTES       (ST7305_WIDTH * ST7305_HEIGHT / 8)   /* 15000 */
#define NES_PACKED_256x224 (256 * 224 / 4)                       /* 14336 */
static uint8_t s_rowbits[4][2][16];
static void board_shim_rebuild_nes_rowbits(void);

void board_shim_set_gb_gray(int mode) {
    if (mode < 0) mode = 0;
    if (mode > 2) mode = 2;
    g_gb_gray = mode;
    board_shim_rebuild_nes_rowbits();
}

/* st7305 句柄 (由 board_shim_set_lcd 设置) */
static st7305_handle_t *s_lcd = NULL;

/* === 异步刷新任务 (V1.0.53) ===
 * GB/GBC 模拟任务 (core1, prio4) 若每 33ms 同步刷一次 12-15ms 的 LCD,
 * 刷屏期间音频生产完全停摆, I2S 持续消耗 → 环形缓冲 ~2s 耗尽 → 声音卡顿.
 * 这里把刷屏拆到 core0 独立任务: 模拟任务只画帧 + 通知, 刷新任务拷快照后
 * SPI 发送, 模拟任务每帧仅多付出一次 memcpy 级锁等待 (~60us). */
#define BOARD_VIDEO_FLUSH_INTERVAL_US  33000   /* ~30Hz, 与 gb/gbc 节流一致 */
/* V1.0.55: 1536 字 (6KB) 在加入 flush 日志/60Hz 刷新时爆栈 (spi polling transmit
 * 调用链 + ESP_LOGI 格式化), 任务栈在 PSRAM, 直接给 12KB 余量. */
#define BOARD_VIDEO_STACK_WORDS        3072
static SemaphoreHandle_t s_fb_lock = NULL;      /* 保护整帧写入 vs 快照拷贝 */
static SemaphoreHandle_t s_flush_sem = NULL;    /* 帧就绪通知 */
static uint8_t *s_flush_snapshot = NULL;        /* 15KB PSRAM 快照缓冲 */
static TaskHandle_t s_video_task = NULL;
static volatile bool s_video_task_running = false;
static volatile int s_video_task_refs = 0;   /* GB/GBC 模拟任务引用计数 */
static volatile uint32_t s_flush_interval_us = BOARD_VIDEO_FLUSH_INTERVAL_US;
static int64_t s_last_flush_us = 0;
EXT_RAM_BSS_ATTR static StackType_t s_video_stack[BOARD_VIDEO_STACK_WORDS];
static StaticTask_t s_video_tcb;

/* NES: 模拟任务输出 2bit 打包灰度帧 (PSRAM), 拷贝到内部 RAM 显示缓冲,
 * 视频任务从内部 RAM 解包缩放 -> 写 FB -> SPI (全部在 core0, 不拖模拟帧). */
static const uint8_t *s_nes_shade_src = NULL;  /* 模拟任务打包帧 (PSRAM) */
static int s_nes_shade_w = 0, s_nes_shade_h = 0;
static int s_nes_shade_mode = 2;   /* NES 显示模式: 0=点对点, 1=全屏, 2=拉伸 */
static int s_nes_packed_bytes = 0;
static uint8_t *s_nes_internal = NULL;   /* 内部 RAM 单块: [显示打包帧 | FB 暂存] */
static uint8_t *s_nes_disp = NULL;
static uint8_t *s_fb_stage = NULL;
static bool     s_gb_stage_owned = false;  /* GB (Peanut-GB) 独占的 s_fb_stage */
static int64_t s_nes_scale_us = 0;       /* 诊断: 最近一次缩放耗时 */

/* GB/GBC (gnuboy): 模拟任务只产出 RGB565 双缓冲帧, 视频任务做 2x 灰度转换+SPI.
 * s_gbc_src 指向 displayBuffer 当前帧, 双缓冲保证转换期间不会被覆盖. */
static const uint16_t *s_gbc_src = NULL;
static int s_gbc_w = 0, s_gbc_h = 0;
static int s_gbc_mode = 1;   /* 0=1x点对点, 1=2x全屏, 2=拉伸400x300 */
static int64_t s_gbc_conv_us = 0;    /* 诊断: 最近一次 GBC 转换耗时 */
static int64_t s_gbc_spi_us = 0;     /* 诊断: 最近一次 SPI 耗时 */
static int64_t s_gbc_pack_us = 0;    /* 诊断: 拉伸打包阶段耗时 */
static int64_t s_gbc_scale_us = 0;   /* 诊断: 拉伸缩放阶段耗时 */

void board_rlcd_set_nes_shade_source(const uint8_t *shade, int w, int h, int mode)
{
    if (shade && w > 0 && h > 0) {
        int packed = w * h / 4;
        if (!s_nes_internal || s_nes_packed_bytes != packed) {
            if (s_nes_internal) heap_caps_free(s_nes_internal);
            s_nes_internal = NULL;
            s_nes_disp = NULL;
            s_fb_stage = NULL;
            s_nes_packed_bytes = 0;
            s_nes_internal = heap_caps_malloc(packed + NES_FB_BYTES + 4,
                                              MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            if (!s_nes_internal) return;   /* 内部 RAM 不足时不启用 NES 缩放路径 */
            s_nes_packed_bytes = packed;
            s_nes_disp = s_nes_internal;
            s_fb_stage = s_nes_internal + ((packed + 3) & ~3);
        }
        board_shim_rebuild_nes_rowbits();
        s_nes_shade_src = shade;
        s_nes_shade_w = w;
        s_nes_shade_h = h;
        if (mode < 0) mode = 0;
        if (mode > 2) mode = 2;
        s_nes_shade_mode = mode;
    } else {
        s_nes_shade_src = NULL;
        s_nes_shade_w = 0;
        s_nes_shade_h = 0;
        /* s_fb_stage 延迟到 video_task_stop() 任务退出后再释放, 避免竞态 */
    }
}

void board_rlcd_set_gbc_frame_source(const uint16_t *buf, int w, int h, int mode)
{
    if (mode < 0) mode = 0;
    if (mode > 2) mode = 2;
    s_gbc_src = buf;
    s_gbc_w = w;
    s_gbc_h = h;
    s_gbc_mode = mode;
    if (buf && (!s_nes_internal || s_nes_packed_bytes != (w * h / 4))) {
        /* GB/GBC 转换需要内部暂存: [打包灰度 | FB 暂存].
         * 拉伸模式把 RGB565 先打包成 2bit 灰度 (内部 RAM), 再查表拉伸,
         * 避免逐像素读 PSRAM 拖垮 core0 (看门狗/蓝牙断开根因). */
        if (s_nes_internal) heap_caps_free(s_nes_internal);
        s_nes_internal = NULL;
        s_nes_disp = NULL;
        s_fb_stage = NULL;
        s_nes_packed_bytes = 0;
        int packed = w * h / 4;
        s_nes_internal = heap_caps_malloc(packed + NES_FB_BYTES + 4,
                                          MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!s_nes_internal) {
            ESP_LOGE(TAG, "GBC 内部暂存分配失败 (%d B)", packed + NES_FB_BYTES + 4);
            return;
        }
        s_nes_packed_bytes = packed;
        s_nes_disp = s_nes_internal;
        s_fb_stage = s_nes_internal + ((packed + 3) & ~3);
        ESP_LOGI(TAG, "GBC 视频暂存就绪 mode=%d packed=%d", mode, packed);
    }
}

/* 模拟任务把打包帧拷贝进内部 RAM 显示缓冲 (14336 字节, ~0.2ms) */
uint8_t *board_rlcd_nes_disp_buffer(void)
{
    return s_nes_disp;
}

/* V1.0.61 原始单缓冲设计: 模拟任务直接写显示缓冲, 发布为无操作 */
void board_rlcd_nes_disp_publish(void)
{
}

void board_rlcd_fb_lock(void)
{
    if (s_fb_lock) xSemaphoreTake(s_fb_lock, portMAX_DELAY);
}

void board_rlcd_fb_unlock(void)
{
    if (s_fb_lock) xSemaphoreGive(s_fb_lock);
}

static void board_video_flush_task(void *arg)
{
    (void)arg;
    while (s_video_task_running) {
        if (xSemaphoreTake(s_flush_sem, portMAX_DELAY) != pdTRUE)
            continue;
        if (!s_video_task_running) break;
        if (!s_lcd || !s_flush_snapshot) continue;
        /* 节流: 不到间隔的帧直接丢弃 (刷新任务始终发最新帧) */
        int64_t now = esp_timer_get_time();
        if (now - s_last_flush_us < s_flush_interval_us)
            continue;
        int64_t t0 = esp_timer_get_time();
        if (s_nes_shade_src) {
            /* NES: 灰度帧 -> 全屏缩放/点对点 -> FB, 全部在 core0 视频任务内完成 */
            board_rlcd_fb_lock();
            if (s_nes_shade_mode != 0) {
                /* 1=全屏 / 2=拉伸: 均走 scaled 拉伸到全屏 (400 宽) 归一处理 */
                board_rlcd_draw_nes_scaled(s_nes_disp, s_nes_shade_w, s_nes_shade_h);
            } else {
                memcpy(s_fb_stage, s_lcd->fb, ST7305_WIDTH * ST7305_HEIGHT / 8);
                int x = (ST7305_WIDTH - s_nes_shade_w) / 2;
                int y = (ST7305_HEIGHT - s_nes_shade_h) / 2;
                for (int yy = 0; yy < s_nes_shade_h; yy++) {
                    board_rlcd_draw_nes_line_1x_to(s_fb_stage, x, y + yy,
                                                   s_nes_disp + ((yy * s_nes_shade_w) >> 2),
                                                   s_nes_shade_w);
                }
                memcpy(s_lcd->fb, s_fb_stage, ST7305_WIDTH * ST7305_HEIGHT / 8);
            }
            virtual_keys_draw(s_lcd);
            board_rlcd_fb_unlock();
            st7305_flush_from(s_lcd, s_lcd->fb);
        } else if (s_gbc_src && s_gbc_w > 0 && s_gbc_h > 0) {
            /* GB/GBC: RGB565 双缓冲帧 -> 按显示模式转 FB (core0, 不占模拟帧预算) */
            board_rlcd_fb_lock();
            int64_t c0 = esp_timer_get_time();
            if (s_gbc_mode == 2) {
                board_rlcd_draw_gbc_stretch(s_gbc_src, s_gbc_w, s_gbc_h);
            } else if (s_gbc_mode == 1) {
                int gx = (ST7305_WIDTH - s_gbc_w * 2) / 2;
                int gy = (ST7305_HEIGHT - s_gbc_h * 2) / 2;
                uint16_t line_be[160];   /* 模拟帧为小端, 2x 绘制函数按大端解析 */
                if (s_fb_stage) {
                    /* 内部 RAM 暂存构建 2x 帧, 一次 memcpy 写回 PSRAM,
                     * 避免 9 万次逐像素 PSRAM 读改写把 core0 拖到看门狗重启 */
                    memcpy(s_fb_stage, s_lcd->fb, NES_FB_BYTES);
                    for (int yy = 0; yy < s_gbc_h; yy++) {
                        const uint16_t *row = s_gbc_src + yy * s_gbc_w;
                        for (int xx = 0; xx < s_gbc_w; xx++)
                            line_be[xx] = (uint16_t)((row[xx] << 8) | (row[xx] >> 8));
                        board_rlcd_draw_gbc_line_2x_rgb565_be_to(s_fb_stage, gx, gy + yy * 2,
                                                                 line_be, s_gbc_w);
                    }
                    memcpy(s_lcd->fb, s_fb_stage, NES_FB_BYTES);
                } else {
                    for (int yy = 0; yy < s_gbc_h; yy++) {
                        const uint16_t *row = s_gbc_src + yy * s_gbc_w;
                        for (int xx = 0; xx < s_gbc_w; xx++)
                            line_be[xx] = (uint16_t)((row[xx] << 8) | (row[xx] >> 8));
                        board_rlcd_draw_gbc_line_2x_rgb565_be(gx, gy + yy * 2,
                                                              line_be, s_gbc_w);
                    }
                }
            } else {
                /* 1x 点对点: 160x144 居中 (同样走内部暂存, 避免直写 PSRAM) */
                int gx = (ST7305_WIDTH - s_gbc_w) / 2;
                int gy = (ST7305_HEIGHT - s_gbc_h) / 2;
                uint8_t shade_row[160];
                if (s_fb_stage) {
                    memcpy(s_fb_stage, s_lcd->fb, NES_FB_BYTES);
                    for (int yy = 0; yy < s_gbc_h; yy++) {
                        const uint16_t *row = s_gbc_src + yy * s_gbc_w;
                        for (int xx = 0; xx < s_gbc_w; xx++) {
                            uint16_t be = (uint16_t)((row[xx] << 8) | (row[xx] >> 8));
                            shade_row[xx] = board_rlcd_rgb565_be_to_gbc_shade(be);
                        }
                        board_rlcd_draw_gb_line_to(s_fb_stage, gx, gy + yy,
                                                   shade_row, s_gbc_w);
                    }
                    memcpy(s_lcd->fb, s_fb_stage, NES_FB_BYTES);
                } else {
                    for (int yy = 0; yy < s_gbc_h; yy++) {
                        const uint16_t *row = s_gbc_src + yy * s_gbc_w;
                        for (int xx = 0; xx < s_gbc_w; xx++) {
                            uint16_t be = (uint16_t)((row[xx] << 8) | (row[xx] >> 8));
                            shade_row[xx] = board_rlcd_rgb565_be_to_gbc_shade(be);
                        }
                        board_rlcd_draw_gb_line(gx, gy + yy, shade_row, s_gbc_w);
                    }
                }
            }
            virtual_keys_draw(s_lcd);
            board_rlcd_fb_unlock();
            s_gbc_conv_us = esp_timer_get_time() - c0;
            int64_t s0 = esp_timer_get_time();
            st7305_flush_from(s_lcd, s_lcd->fb);
            s_gbc_spi_us = esp_timer_get_time() - s0;
        } else {
            /* GB/GBC: 快照拷贝 + SPI (模拟任务画完 FB 后通知) */
            board_rlcd_fb_lock();
            virtual_keys_draw(s_lcd);
            memcpy(s_flush_snapshot, s_lcd->fb, ST7305_WIDTH * ST7305_HEIGHT / 8);
            board_rlcd_fb_unlock();
            st7305_flush_from(s_lcd, s_flush_snapshot);
        }
        int64_t flush_us = esp_timer_get_time() - t0;
        static int64_t flush_max_us = 0;
        static uint32_t flush_cnt = 0;
        static int64_t flush_total_us = 0;
        flush_total_us += flush_us;
        if (flush_us > flush_max_us) flush_max_us = flush_us;
        if ((++flush_cnt % 120) == 0) {
            ESP_LOGI(TAG, "LCD flush: avg=%dus max=%dus interval=%uus conv=%lldus spi=%lldus pack=%lldus scale=%lldus",
                     (int)(flush_total_us / flush_cnt), (int)flush_max_us,
                     (int)s_flush_interval_us,
                     (long long)s_gbc_conv_us, (long long)s_gbc_spi_us,
                     (long long)s_gbc_pack_us, (long long)s_gbc_scale_us);
            flush_total_us = 0;
            flush_cnt = 0;
            flush_max_us = 0;
        }
        s_last_flush_us = now;
    }
    vTaskDelete(NULL);
}

/* 供各引擎设置刷新间隔 (GB/GBC=33ms, NES 可设 16.7ms) */
void board_rlcd_video_task_set_interval_us(uint32_t us)
{
    if (us >= 8000 && us <= 100000) s_flush_interval_us = us;
}

esp_err_t board_rlcd_flush_async(void)
{
    if (!s_flush_sem || !s_video_task_running) return ESP_ERR_INVALID_STATE;
    xSemaphoreGive(s_flush_sem);
    return ESP_OK;
}

void board_rlcd_video_task_start(void)
{
    if (s_video_task_running) {
        s_video_task_refs++;
        return;
    }
    if (!s_flush_sem) {
        s_flush_sem = xSemaphoreCreateBinary();
        s_fb_lock = xSemaphoreCreateMutex();
    }
    if (!s_flush_snapshot) {
        s_flush_snapshot = heap_caps_malloc(ST7305_WIDTH * ST7305_HEIGHT / 8,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_flush_snapshot) return;
    }
    s_video_task_running = true;
    s_video_task_refs = 1;
    s_last_flush_us = 0;
    s_video_task = xTaskCreateStaticPinnedToCore(board_video_flush_task, "vid_flush",
                                                 BOARD_VIDEO_STACK_WORDS, NULL, 6,
                                                 s_video_stack, &s_video_tcb, 0);
    if (!s_video_task) s_video_task_running = false;
}

void board_rlcd_video_task_stop(void)
{
    if (!s_video_task_running) return;
    if (s_video_task_refs > 1) {
        s_video_task_refs--;
        return;
    }
    s_video_task_refs = 0;
    s_video_task_running = false;
    xSemaphoreGive(s_flush_sem);
    for (int i = 0; i < 20 && s_video_task; i++) vTaskDelay(pdMS_TO_TICKS(2));
    s_video_task = NULL;
    if (!s_nes_shade_src && !s_gbc_src && s_nes_internal) {
        heap_caps_free(s_nes_internal);
        s_nes_internal = NULL;
        s_nes_disp = NULL;
        s_fb_stage = NULL;
        s_nes_packed_bytes = 0;
    }
}

void board_shim_set_lcd(st7305_handle_t *lcd) {
    s_lcd = lcd;
}

/* === GB (Peanut-GB) 2x 绘制暂存 ===
 * board_rlcd_draw_gb_line_2x 直接画到 s_fb_stage (内部 RAM), 需要显式分配.
 * NES/GBC 路径用 s_nes_internal 里的暂存, 与本函数互斥 (各引擎不会同时运行). */
esp_err_t board_shim_gb_stage_alloc(void)
{
    if (s_fb_stage) return ESP_OK;
    uint8_t *m = heap_caps_malloc(ST7305_WIDTH * ST7305_HEIGHT / 8,
                                  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!m) {
        /* 内部 RAM 不足时回退 PSRAM: Peanut-GB 单帧 <2ms, 放 PSRAM 性能余量充足 */
        m = heap_caps_malloc(ST7305_WIDTH * ST7305_HEIGHT / 8,
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!m) {
        ESP_LOGE(TAG, "GB stage 分配失败 (%d B)", ST7305_WIDTH * ST7305_HEIGHT / 8);
        return ESP_ERR_NO_MEM;
    }
    s_fb_stage = m;
    s_gb_stage_owned = true;
    return ESP_OK;
}

void board_shim_gb_stage_free(void)
{
    if (s_gb_stage_owned && s_fb_stage) {
        heap_caps_free(s_fb_stage);
        s_gb_stage_owned = false;
    }
    s_fb_stage = NULL;
}

/* 返回 GB 2x 绘制暂存指针 (供 1x 模式 board_rlcd_draw_gb_line_to 使用) */
uint8_t *board_shim_gb_stage_get(void)
{
    return s_fb_stage;
}

/* 提交 GB 暂存到 LCD FB (锁保护), 再调 board_rlcd_flush_async() 异步刷新 */
void board_shim_gb_stage_commit(void)
{
    if (!s_lcd || !s_lcd->fb || !s_fb_stage) return;
    board_rlcd_fb_lock();
    memcpy(s_lcd->fb, s_fb_stage, ST7305_WIDTH * ST7305_HEIGHT / 8);
    board_rlcd_fb_unlock();
}

/* === board_rlcd 兼容接口 === */

bool board_rlcd_is_initialized(void) {
    return s_lcd != NULL;
}

esp_err_t board_rlcd_clear(uint8_t color) {
    if (!s_lcd) return ESP_ERR_INVALID_STATE;
    st7305_clear(s_lcd, color);
    return ESP_OK;
}

esp_err_t board_rlcd_flush(void) {
    if (!s_lcd) return ESP_ERR_INVALID_STATE;
    return st7305_flush(s_lcd);
}

/* === GB 2x2 抖动 (直接移植参考项目 board_rlcd.cpp) ===
 *
 * GB shade: 0=白(lightest), 1=浅灰, 2=深灰, 3=黑(darkest)
 * 2x2 抖动模式 (模拟4级灰度):
 *   shade 0: [W W]   shade 1: [B W]   shade 2: [B W]   shade 3: [B B]
 *            [W W]            [W W]            [W B]            [B B]
 *
 * ST7305 fb: bit=1→白(屏幕亮), bit=0→黑(屏幕暗), with 0x21 inversion.
 * fb 布局: 每字节 2列×4行, index=(x>>1)*(H>>2)+((H-1-y)>>2), bit=7-(((H-1-y)&3)<<1|(x&1))
 */
#define FB_HEIGHT_DIV_4  (ST7305_HEIGHT >> 2)  /* 75 */

static inline void write_masked_pixel(uint8_t *fb, uint32_t index, uint8_t mask, uint8_t color) {
    if (color == ST7305_COLOR_WHITE) {
        fb[index] |= mask;
    } else {
        fb[index] &= (uint8_t)~mask;
    }
}

/* 点聚(clustered)半色调表: 每个游戏像素占 2x2 物理点, 用"聚在一起的黑色点数"表达灰阶.
 * 黑点聚成实心块 (非散点抖动), 静态图案 -> 低刷新率下不闪烁.
 * 每个 shade 的行: [top-left, top-right, bottom-left, bottom-right] */
static const uint8_t s_cluster_4[4][4] = {
    /* shade 0 (白): 0 黑 */
    { ST7305_COLOR_WHITE, ST7305_COLOR_WHITE, ST7305_COLOR_WHITE, ST7305_COLOR_WHITE },
    /* shade 1 (浅灰): 1 黑 (左上) */
    { ST7305_COLOR_BLACK, ST7305_COLOR_WHITE, ST7305_COLOR_WHITE, ST7305_COLOR_WHITE },
    /* shade 2 (中灰): 2 黑 (左上+右上, 聚成上排) */
    { ST7305_COLOR_BLACK, ST7305_COLOR_BLACK, ST7305_COLOR_WHITE, ST7305_COLOR_WHITE },
    /* shade 3 (深灰): 4 黑 (全黑) */
    { ST7305_COLOR_BLACK, ST7305_COLOR_BLACK, ST7305_COLOR_BLACK, ST7305_COLOR_BLACK },
};

/* 散点抖动 (Bayer 2x2): 黑点离散分布表达灰阶, 层次更细腻 (低刷新率下可能微闪) */
static const uint8_t s_scatter[4][4] = {
    /* shade 0 (白): 0 黑 */
    { ST7305_COLOR_WHITE, ST7305_COLOR_WHITE, ST7305_COLOR_WHITE, ST7305_COLOR_WHITE },
    /* shade 1 (浅灰): 1 黑 (左上) */
    { ST7305_COLOR_BLACK, ST7305_COLOR_WHITE, ST7305_COLOR_WHITE, ST7305_COLOR_WHITE },
    /* shade 2 (中灰): 2 黑 (左上+右下, 对角散点) */
    { ST7305_COLOR_BLACK, ST7305_COLOR_WHITE, ST7305_COLOR_WHITE, ST7305_COLOR_BLACK },
    /* shade 3 (深灰): 3 黑 (右下外全黑) */
    { ST7305_COLOR_BLACK, ST7305_COLOR_BLACK, ST7305_COLOR_BLACK, ST7305_COLOR_BLACK },
};

/* 纯黑白: shade>=2 为黑, 否则白 */
static const uint8_t s_cluster_bw[4][4] = {
    { ST7305_COLOR_WHITE, ST7305_COLOR_WHITE, ST7305_COLOR_WHITE, ST7305_COLOR_WHITE },
    { ST7305_COLOR_WHITE, ST7305_COLOR_WHITE, ST7305_COLOR_WHITE, ST7305_COLOR_WHITE },
    { ST7305_COLOR_BLACK, ST7305_COLOR_BLACK, ST7305_COLOR_BLACK, ST7305_COLOR_BLACK },
    { ST7305_COLOR_BLACK, ST7305_COLOR_BLACK, ST7305_COLOR_BLACK, ST7305_COLOR_BLACK },
};

/* 按当前灰度模式取 shade 对应的 2x2 图案: 0=纯黑白, 1=点聚灰度, 2=散点抖动 */
static const uint8_t *board_shim_cluster_get(uint8_t shade)
{
    shade &= 0x03;
    if (g_gb_gray == 0) return s_cluster_bw[shade];
    if (g_gb_gray == 2) return s_scatter[shade];
    return s_cluster_4[shade];
}

static void board_shim_rebuild_nes_rowbits(void)
{
    const uint8_t (*pat)[4] = (g_gb_gray == 0) ? s_cluster_bw :
                              (g_gb_gray == 2) ? s_scatter : s_cluster_4;
    for (int ly = 0; ly < 4; ly++) {
        for (int par = 0; par < 2; par++) {
            for (int pair = 0; pair < 16; pair++) {
                uint8_t s0 = (uint8_t)(pair >> 2);
                uint8_t s1 = (uint8_t)(pair & 3);
                uint8_t b = 0;
                uint8_t bit0 = (uint8_t)(7U - (ly << 1));
                uint8_t bit1 = (uint8_t)(7U - ((ly << 1) | 1U));
                if (pat[s0][(par << 1) | 0] == ST7305_COLOR_WHITE) b |= (uint8_t)(1U << bit0);
                if (pat[s1][(par << 1) | 1] == ST7305_COLOR_WHITE) b |= (uint8_t)(1U << bit1);
                s_rowbits[ly][par][pair] = b;
            }
        }
    }
}

esp_err_t board_rlcd_draw_gb_line_2x(int x, int y, const uint8_t *pixels, int width) {
    if (!s_lcd || !s_lcd->fb || !pixels) return ESP_ERR_INVALID_ARG;

    uint8_t *fb = s_fb_stage;
    const uint16_t start_byte_x = (uint16_t)(x >> 1);

    /* Row 0 (y) */
    uint16_t inv_y0 = (uint16_t)(ST7305_HEIGHT - 1 - y);
    uint16_t block_y0 = inv_y0 >> 2;
    uint8_t local_y0 = inv_y0 & 3U;
    uint8_t bit0 = (uint8_t)(7U - (local_y0 << 1));
    uint8_t left_mask0 = (uint8_t)(1U << bit0);
    uint8_t right_mask0 = (uint8_t)(1U << (bit0 - 1U));

    /* Row 1 (y+1) */
    uint16_t inv_y1 = (uint16_t)(ST7305_HEIGHT - 1 - (y + 1));
    uint16_t block_y1 = inv_y1 >> 2;
    uint8_t local_y1 = inv_y1 & 3U;
    uint8_t bit1 = (uint8_t)(7U - (local_y1 << 1));
    uint8_t left_mask1 = (uint8_t)(1U << bit1);
    uint8_t right_mask1 = (uint8_t)(1U << (bit1 - 1U));

    for (int dx = 0; dx < width; dx++) {
        uint32_t index0 = (uint32_t)(start_byte_x + dx) * FB_HEIGHT_DIV_4 + block_y0;
        uint32_t index1 = (uint32_t)(start_byte_x + dx) * FB_HEIGHT_DIV_4 + block_y1;

        uint8_t shade = pixels[dx] & 0x03;
        const uint8_t *d = board_shim_cluster_get(shade);

        /* top-left, top-right */
        write_masked_pixel(fb, index0, left_mask0, d[0]);
        write_masked_pixel(fb, index0, right_mask0, d[1]);
        /* bottom-left, bottom-right */
        write_masked_pixel(fb, index1, left_mask1, d[2]);
        write_masked_pixel(fb, index1, right_mask1, d[3]);
    }

    return ESP_OK;
}


/* V1.0.46: GB 模拟器 1x 点对点绘制 (游戏全屏关闭时用, 单行单列) */
/* === GBC RGB565 (BE) → 1bit 绘制 (直接移植参考项目 board_rlcd.cpp) ===
 *
 * gnuboy 以 GB_PIXEL_565_BE 输出 RGB565; 这里把每像素转成 4 级灰度 shade,
 * 再复用 GB 的 2x2 抖动表 (s_dither_table) 画到 1bit fb, 2x 缩放.
 */
static inline uint8_t board_rlcd_rgb565_be_to_gbc_shade(uint16_t rgb565_be)
{
    uint16_t rgb = (uint16_t)((rgb565_be >> 8) | (rgb565_be << 8));
    uint8_t r5 = (uint8_t)((rgb >> 11) & 0x1FU);
    uint8_t g6 = (uint8_t)((rgb >> 5) & 0x3FU);
    uint8_t b5 = (uint8_t)(rgb & 0x1FU);
    uint16_t gray = (uint16_t)(r5 * 38U + g6 * 75U + b5 * 15U);

    if (gray >= 4800U) return 0;
    if (gray >= 3200U) return 1;
    if (gray >= 1600U) return 2;
    return 3;
}

esp_err_t board_rlcd_draw_gbc_line_2x_rgb565_be_to(uint8_t *fb, int x, int y,
                                                   const uint16_t *pixels, int width) {
    if (!fb || !pixels) return ESP_ERR_INVALID_ARG;
    if ((x & 1U) != 0) return ESP_ERR_INVALID_ARG;

    const uint16_t start_byte_x = (uint16_t)(x >> 1);

    /* Row 0 (y) */
    uint16_t inv_y0 = (uint16_t)(ST7305_HEIGHT - 1 - y);
    uint16_t block_y0 = inv_y0 >> 2;
    uint8_t local_y0 = inv_y0 & 3U;
    uint8_t bit0 = (uint8_t)(7U - (local_y0 << 1));
    uint8_t left_mask0 = (uint8_t)(1U << bit0);
    uint8_t right_mask0 = (uint8_t)(1U << (bit0 - 1U));

    /* Row 1 (y+1) */
    uint16_t inv_y1 = (uint16_t)(ST7305_HEIGHT - 1 - (y + 1));
    uint16_t block_y1 = inv_y1 >> 2;
    uint8_t local_y1 = inv_y1 & 3U;
    uint8_t bit1 = (uint8_t)(7U - (local_y1 << 1));
    uint8_t left_mask1 = (uint8_t)(1U << bit1);
    uint8_t right_mask1 = (uint8_t)(1U << (bit1 - 1U));

    uint32_t index0 = (uint32_t)start_byte_x * FB_HEIGHT_DIV_4 + block_y0;
    uint32_t index1 = (uint32_t)start_byte_x * FB_HEIGHT_DIV_4 + block_y1;
    for (int dx = 0; dx < width; dx++) {
        uint8_t shade = board_rlcd_rgb565_be_to_gbc_shade(pixels[dx]);
        const uint8_t *d = board_shim_cluster_get(shade);

        write_masked_pixel(fb, index0, left_mask0, d[0]);
        write_masked_pixel(fb, index0, right_mask0, d[1]);
        write_masked_pixel(fb, index1, left_mask1, d[2]);
        write_masked_pixel(fb, index1, right_mask1, d[3]);
        index0 += FB_HEIGHT_DIV_4;
        index1 += FB_HEIGHT_DIV_4;
    }

    return ESP_OK;
}

esp_err_t board_rlcd_draw_gbc_line_2x_rgb565_be(int x, int y, const uint16_t *pixels, int width)
{
    if (!s_lcd || !s_lcd->fb) return ESP_ERR_INVALID_ARG;
    return board_rlcd_draw_gbc_line_2x_rgb565_be_to(s_lcd->fb, x, y, pixels, width);
}

/* GB/GBC 强制拉伸全屏: RGB565(小端) 160x144 -> 400x300 最近邻, 复用 rowbits 查表打包 */
esp_err_t board_rlcd_draw_gbc_stretch(const uint16_t *buf_le, int src_w, int src_h)
{
    if (!s_lcd || !s_lcd->fb || !buf_le || !s_fb_stage || !s_nes_disp ||
        src_w <= 0 || src_h <= 0)
        return ESP_ERR_INVALID_ARG;

    static uint8_t s_row_map[ST7305_HEIGHT];
    static uint8_t s_col_map[ST7305_WIDTH];
    static uint8_t s_scaled_row[ST7305_WIDTH];
    static int s_cached_w = -1, s_cached_h = -1;
    if (s_cached_w != src_w || s_cached_h != src_h) {
        for (int y = 0; y < ST7305_HEIGHT; y++)
            s_row_map[y] = (uint8_t)((uint32_t)y * (uint32_t)src_h / ST7305_HEIGHT);
        for (int x = 0; x < ST7305_WIDTH; x++)
            s_col_map[x] = (uint8_t)((uint32_t)x * (uint32_t)src_w / ST7305_WIDTH);
        s_cached_w = src_w;
        s_cached_h = src_h;
    }

    /* 1) 源帧 RGB565 -> 2bit 打包灰度, 放内部 RAM (逐行顺序读 PSRAM, 缓存友好) */
    int64_t t_pack0 = esp_timer_get_time();
    uint8_t *packed = s_nes_disp;
    int pw = src_w >> 2;
    for (int sy = 0; sy < src_h; sy++) {
        const uint16_t *row = buf_le + sy * src_w;
        uint8_t *pd = packed + sy * pw;
        for (int px = 0; px < src_w; px += 4) {
            uint8_t b = 0;
            for (int k = 0; k < 4; k++) {
                uint16_t be = (uint16_t)((row[px + k] << 8) | (row[px + k] >> 8));
                b |= (uint8_t)(board_rlcd_rgb565_be_to_gbc_shade(be) << (k * 2));
            }
            pd[px >> 2] = b;
        }
    }

    /* 2) 打包帧最近邻拉伸 -> 内部 FB 暂存 -> 一次写回 PSRAM */
    s_gbc_pack_us = esp_timer_get_time() - t_pack0;
    int64_t t_scale0 = esp_timer_get_time();
    uint8_t *fb = s_fb_stage;
    memset(fb, 0, NES_FB_BYTES);
    int py = 0;
    for (int sy = 0; sy < src_h; sy++) {
        while (py < ST7305_HEIGHT && s_row_map[py] < sy) py++;
        int py0 = py;
        while (py < ST7305_HEIGHT && s_row_map[py] == sy) py++;
        int py1 = py - 1;
        if (py0 > py1) continue;

        const uint8_t *prow = packed + sy * pw;
        for (int px = 0; px < ST7305_WIDTH; px++) {
            int sx = s_col_map[px];
            s_scaled_row[px] = (uint8_t)((prow[sx >> 2] >> ((sx & 3) << 1)) & 3);
        }

        for (int yy = py0; yy <= py1; yy++) {
            uint32_t inv = ST7305_HEIGHT - 1 - (uint32_t)yy;
            uint32_t block_y = inv >> 2;
            uint8_t local_y = inv & 3U;
            const uint8_t *rbt = s_rowbits[local_y][yy & 1];
            uint32_t idx = block_y;   /* 增量索引, 避免每字节乘 75 */
            for (int cp = 0; cp < ST7305_WIDTH / 2; cp++) {
                uint8_t pair = (uint8_t)((s_scaled_row[cp << 1] << 2) |
                                         s_scaled_row[(cp << 1) | 1]);
                fb[idx] |= rbt[pair];
                idx += FB_HEIGHT_DIV_4;
            }
        }
    }
    memcpy(s_lcd->fb, fb, NES_FB_BYTES);
    s_gbc_scale_us = esp_timer_get_time() - t_scale0;
    return ESP_OK;
}

esp_err_t board_rlcd_draw_gb_line_to(uint8_t *fb, int x, int y,
                                     const uint8_t *pixels, int width) {
    if (!fb || !pixels) return ESP_ERR_INVALID_ARG;

    uint16_t inv_y = (uint16_t)(ST7305_HEIGHT - 1 - y);
    uint16_t block_y = inv_y >> 2;
    uint8_t local_y = inv_y & 3U;

    for (int dx = 0; dx < width; dx++) {
        /* V1.0.53 修复: 1x 每像素占半个字节 (2列x4行布局),
         * 字节索引 = (x>>1), 位 = 7-((y_sub<<1)|(x&1)).
         * 旧实现按 dx 递增字节索引且固定左列位, 导致画面横向错位/花屏. */
        int x_abs = x + dx;
        uint32_t index = (uint32_t)(x_abs >> 1) * FB_HEIGHT_DIV_4 + block_y;
        uint8_t bit = (uint8_t)(7U - ((local_y << 1) | (x_abs & 1)));
        uint8_t mask = (uint8_t)(1U << bit);
        uint8_t shade = pixels[dx] & 0x03;
        /* 1x 点对点: 每像素仅 1 个物理点, 无法用 2x2 点聚, 直接取黑白档色 */
        uint8_t color = s_cluster_bw[shade][0];
        write_masked_pixel(fb, index, mask, color);
    }
    return ESP_OK;
}

esp_err_t board_rlcd_draw_gb_line(int x, int y, const uint8_t *pixels, int width)
{
    if (!s_lcd || !s_lcd->fb) return ESP_ERR_INVALID_ARG;
    return board_rlcd_draw_gb_line_to(s_lcd->fb, x, y, pixels, width);
}

/* NES 拉伸全屏 (输入为 2bit 打包帧, 位于内部 RAM):
 * 最近邻缩放 256x224 -> 400x300, 用 rowbits 查表把"2 像素"合成 2 个物理位,
 * 在内部 FB 暂存里 OR 出整字节, 最后一次性 memcpy 到 PSRAM FB.
 * FB 布局: index = (x>>1)*75 + ((HEIGHT-1-y)>>2), bit=1 白 / 0 黑. */
esp_err_t board_rlcd_draw_nes_scaled(const uint8_t *shade, int src_w, int src_h)
{
    if (!s_lcd || !s_lcd->fb || !shade || !s_fb_stage || src_w <= 0 || src_h <= 0)
        return ESP_ERR_INVALID_ARG;
    int64_t t_scale0 = esp_timer_get_time();

    static uint8_t s_row_map[ST7305_HEIGHT];
    static uint8_t s_col_map[ST7305_WIDTH];
    static uint8_t s_scaled_row[ST7305_WIDTH];
    static int s_cached_w = -1, s_cached_h = -1;
    if (s_cached_w != src_w || s_cached_h != src_h) {
        for (int y = 0; y < ST7305_HEIGHT; y++)
            s_row_map[y] = (uint8_t)((uint32_t)y * (uint32_t)src_h / ST7305_HEIGHT);
        for (int x = 0; x < ST7305_WIDTH; x++)
            s_col_map[x] = (uint8_t)((uint32_t)x * (uint32_t)src_w / ST7305_WIDTH);
        s_cached_w = src_w;
        s_cached_h = src_h;
    }

    uint8_t *fb = s_fb_stage;
    memset(fb, 0, NES_FB_BYTES);

    int py = 0;
    for (int sy = 0; sy < src_h; sy++) {
        while (py < ST7305_HEIGHT && s_row_map[py] < sy) py++;
        int py0 = py;
        while (py < ST7305_HEIGHT && s_row_map[py] == sy) py++;
        int py1 = py - 1;
        if (py0 > py1) continue;

        const uint8_t *src_row = shade + ((size_t)sy * src_w >> 2);
        for (int px = 0; px < ST7305_WIDTH; px++) {
            int sx = s_col_map[px];
            s_scaled_row[px] = (uint8_t)((src_row[sx >> 2] >> ((sx & 3) << 1)) & 3);
        }

        for (int yy = py0; yy <= py1; yy++) {
            uint32_t inv = ST7305_HEIGHT - 1 - (uint32_t)yy;
            uint32_t block_y = inv >> 2;
            uint8_t local_y = inv & 3U;
            const uint8_t *rbt = s_rowbits[local_y][yy & 1];
            for (int cp = 0; cp < ST7305_WIDTH / 2; cp++) {
                uint8_t pair = (uint8_t)((s_scaled_row[cp << 1] << 2) |
                                         s_scaled_row[(cp << 1) | 1]);
                fb[(uint32_t)cp * FB_HEIGHT_DIV_4 + block_y] |= rbt[pair];
            }
        }
    }

    memcpy(s_lcd->fb, fb, NES_FB_BYTES);
    s_nes_scale_us = esp_timer_get_time() - t_scale0;
    return ESP_OK;
}

/* NES 1x 点对点绘制 (非全屏): pixels 指向一行 2bit 打包数据 */
esp_err_t board_rlcd_draw_nes_line_1x_to(uint8_t *fb, int x, int y,
                                        const uint8_t *row_packed, int width)
{
    if (!fb || !row_packed) return ESP_ERR_INVALID_ARG;

    uint16_t inv_y = (uint16_t)(ST7305_HEIGHT - 1 - y);
    uint16_t block_y = inv_y >> 2;
    uint8_t local_y = inv_y & 3U;

    for (int dx = 0; dx < width; dx++) {
        int x_abs = x + dx;
        uint32_t index = (uint32_t)(x_abs >> 1) * FB_HEIGHT_DIV_4 + block_y;
        uint8_t bit = (uint8_t)(7U - ((local_y << 1) | (x_abs & 1)));
        uint8_t mask = (uint8_t)(1U << bit);
        uint8_t shade = (uint8_t)((row_packed[dx >> 2] >> ((dx & 3) << 1)) & 3);
        const uint8_t *d = board_shim_cluster_get(shade);
        uint8_t pos = (uint8_t)(((y & 1) << 1) | (x_abs & 1));
        write_masked_pixel(fb, index, mask, d[pos]);
    }
    return ESP_OK;
}

esp_err_t board_rlcd_flush_area(int x1, int y1, int x2, int y2) {
    (void)x1; (void)y1; (void)x2; (void)y2;
    if (!s_lcd) return ESP_ERR_INVALID_STATE;
    return st7305_flush(s_lcd);
}

/* === board_speaker 兼容接口 === */

esp_err_t board_speaker_init(const board_speaker_config_t *config) {
    (void)config;
    ESP_LOGI(TAG, "board_speaker_init (转发到 audio_player, 已初始化)");
    return ESP_OK;
}

esp_err_t board_speaker_write(const void *pcm, size_t bytes, size_t *bytes_written, int timeout_ms) {
    (void)timeout_ms;
    size_t frames = bytes / 4;
    size_t written = audio_player_feed_pcm((const int16_t *)pcm, frames, 24000);
    if (bytes_written) *bytes_written = written * 4;
    return ESP_OK;
}

esp_err_t board_speaker_set_volume(uint8_t volume) {
    audio_player_set_volume(volume);
    return ESP_OK;
}

esp_err_t board_speaker_self_test(void) {
    return ESP_OK;
}
