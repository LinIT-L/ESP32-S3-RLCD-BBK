/* md_emu.c — MD/Genesis (Gwenesis) 适配层
 * 移植自 esp-box-emu components/genesis. 帧循环:
 *   - run_genesis_rom() 每帧渲染 320x224 RGB565 + 混音(内部 throttle 60fps)
 *   - 取 BoxEmu 桩当前帧 -> RGB565 转 4 级灰度 2bit 打包 -> board_rlcd NES shade 管线
 *   - 音频由 BoxEmu 桩直接喂 audio_player (53267/2 Hz 立体声) */
#include "md_emu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board_rlcd.h"
#include "audio_player.h"

static const char *TAG = "md_emu";

#define MD_EMU_TASK_STACK_SIZE (32 * 1024)
#define MD_EMU_TASK_PRIORITY   4
#define MD_EMU_TASK_CORE       1
#define MD_EMU_FRAME_US        16667

#define MD_SCREEN_WIDTH        320
#define MD_VISIBLE_HEIGHT      224
#define MD_FRAME_BYTES         (MD_SCREEN_WIDTH * MD_VISIBLE_HEIGHT * 2)
#define MD_PACKED_BYTES        (MD_SCREEN_WIDTH * MD_VISIBLE_HEIGHT / 4)

/* === Gwenesis 主控 (C++ 实现, 以 extern "C" 导出) === */
extern void init_genesis(uint8_t *romdata, size_t rom_data_size);
extern void reset_genesis(void);
extern void run_genesis_rom(void);
extern void deinit_genesis(void);
extern void md_emu_savestate_load(const char *path);
extern void md_emu_savestate_save(const char *path);
extern uint8_t *boxemu_current_frame(void);

EXT_RAM_BSS_ATTR static StackType_t s_md_task_stack[MD_EMU_TASK_STACK_SIZE / sizeof(StackType_t)];
static StaticTask_t s_md_task_tcb;

static uint8_t *s_rom_data = NULL;
static size_t   s_rom_size = 0;
static uint8_t *s_shade_packed = NULL;   /* 320*224/4 = 17920 字节 (PSRAM) */

typedef struct {
    volatile bool stop_requested;
    volatile bool paused;
    volatile bool pause_ack;
    volatile bool frame_ready;
} md_emu_instance_t;

static md_emu_instance_t *s_instance = NULL;
static bool s_core_inited = false;
static bool s_task_running = false;
static int  s_fullscreen = 2;            /* 0=点对点 1=全屏 2=拉伸 */
static volatile uint8_t s_output_volume = 80;
static md_emu_progress_cb_t s_progress_cb = NULL;
static char s_save_path[160] = {0};

static void md_emu_set_save_path(const char *rom_path)
{
    s_save_path[0] = 0;
    if (!rom_path) return;
    const char *slash = strrchr(rom_path, '/');
    const char *name = slash ? slash + 1 : rom_path;
    const char *dot = strrchr(name, '.');
    int len = dot ? (int)(dot - name) : (int)strlen(name);
    if (len <= 0 || len > 96) len = 32;
    snprintf(s_save_path, sizeof(s_save_path), "/sdcard/dict/MD/%.*s.sav", len, name);
}

void md_emu_set_progress_cb(md_emu_progress_cb_t cb) { s_progress_cb = cb; }

void md_emu_set_volume(uint8_t volume)
{
    if (volume > 100) volume = 100;
    s_output_volume = volume;
    audio_player_set_volume(volume);
}

uint8_t md_emu_get_volume(void) { return s_output_volume; }

void md_emu_set_joypad(uint8_t joypad) { (void)joypad; /* 引擎帧内自读 input */ }

void md_emu_set_fullscreen(int mode) { s_fullscreen = (mode < 0) ? 0 : (mode > 2 ? 2 : mode); }

/* === RGB565 -> 4 级灰度 2bit 打包 (参考 nes video_audio.c 阈值) === */
static uint8_t rgb565_to_level(uint16_t px)
{
    uint32_t r = (px >> 11) & 0x1F;
    uint32_t g = (px >> 5)  & 0x3F;
    uint32_t b = px & 0x1F;
    /* 5/6/5 -> 8bit */
    uint32_t r8 = (r << 3) | (r >> 2);
    uint32_t g8 = (g << 2) | (g >> 4);
    uint32_t b8 = (b << 3) | (b >> 2);
    uint32_t luma = (r8 * 77u + g8 * 150u + b8 * 29u) >> 8;   /* 0..255 */
    if (luma < 64)      return 3;
    else if (luma < 128) return 2;
    else if (luma < 192) return 1;
    return 0;
}

static void md_pack_frame(const uint8_t *rgb565, uint8_t *packed)
{
    int n = MD_SCREEN_WIDTH * MD_VISIBLE_HEIGHT;
    for (int i = 0; i < n; i += 4) {
        packed[i >> 2] = (uint8_t)(rgb565_to_level(rgb565[i] | (rgb565[i + 1] << 8)) |
                                   (rgb565_to_level(rgb565[i + 2] | (rgb565[i + 3] << 8)) << 2) |
                                   (rgb565_to_level(rgb565[i + 4] | (rgb565[i + 5] << 8)) << 4) |
                                   (rgb565_to_level(rgb565[i + 6] | (rgb565[i + 7] << 8)) << 6));
    }
}

static void md_emu_task(void *arg)
{
    (void)arg;
    int64_t next_frame_us = esp_timer_get_time();
    uint32_t frame_count = 0;
    ESP_LOGI(TAG, "MD emulation task started (Gwenesis)");

    while (1) {
        if (s_instance == NULL || s_instance->stop_requested) break;
        if (s_instance->paused) {
            s_instance->pause_ack = true;
            vTaskDelay(pdMS_TO_TICKS(16));
            continue;
        }

        run_genesis_rom();   /* 渲染+音频, 内部 throttle 60fps */

        uint8_t *fb = boxemu_current_frame();
        if (fb && s_shade_packed) {
            md_pack_frame(fb, s_shade_packed);
            uint8_t *disp = board_rlcd_nes_disp_buffer();
            if (disp) {
                memcpy(disp, s_shade_packed, MD_PACKED_BYTES);
                board_rlcd_nes_disp_publish();
                board_rlcd_flush_async();
            }
            s_instance->frame_ready = true;
        }

        /* 存档: 每 300 帧写一次 (约 5 秒), 避免频繁 IO */
        if (++frame_count % 300 == 0 && s_save_path[0]) {
            md_emu_savestate_save(s_save_path);
        }
        (void)next_frame_us;
    }

    if (s_save_path[0]) md_emu_savestate_save(s_save_path);
    ESP_LOGI(TAG, "MD emulation task exit");
    s_task_running = false;
    vTaskDelete(NULL);
}

esp_err_t md_emu_background_init(void)
{
    if (!s_shade_packed) {
        s_shade_packed = heap_caps_malloc(MD_PACKED_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_shade_packed) return ESP_ERR_NO_MEM;
    }
    memset(s_shade_packed, 0, MD_PACKED_BYTES);
    return ESP_OK;
}

void md_emu_unload(void)
{
    if (s_shade_packed) { heap_caps_free(s_shade_packed); s_shade_packed = NULL; }
}

esp_err_t md_emu_start(const char *path)
{
    if (!path || s_task_running) return ESP_ERR_INVALID_ARG;

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "ROM 打开失败: %s", path);
        return ESP_ERR_NOT_FOUND;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 8 * 1024 * 1024) {
        fclose(f);
        ESP_LOGE(TAG, "ROM 尺寸异常: %ld", sz);
        return ESP_ERR_INVALID_SIZE;
    }
    s_rom_size = (size_t)sz;
    s_rom_data = heap_caps_malloc(s_rom_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_rom_data) { fclose(f); return ESP_ERR_NO_MEM; }
    if (fread(s_rom_data, 1, s_rom_size, f) != s_rom_size) {
        fclose(f);
        heap_caps_free(s_rom_data); s_rom_data = NULL;
        return ESP_FAIL;
    }
    fclose(f);

    if (s_progress_cb) s_progress_cb(60);
    init_genesis(s_rom_data, s_rom_size);
    if (s_progress_cb) s_progress_cb(100);
    s_core_inited = true;
    md_emu_set_save_path(path);
    if (s_save_path[0]) md_emu_savestate_load(s_save_path);

    s_instance = heap_caps_malloc(sizeof(md_emu_instance_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_instance) { deinit_genesis(); s_core_inited = false; return ESP_ERR_NO_MEM; }
    memset((void *)s_instance, 0, sizeof(md_emu_instance_t));

    s_task_running = true;
    xTaskCreateStaticPinnedToCore(md_emu_task, "md_emu", MD_EMU_TASK_STACK_SIZE / sizeof(StackType_t),
                                  NULL, MD_EMU_TASK_PRIORITY, s_md_task_stack, &s_md_task_tcb,
                                  MD_EMU_TASK_CORE);
    board_rlcd_set_nes_shade_source(s_shade_packed, MD_SCREEN_WIDTH, MD_VISIBLE_HEIGHT, s_fullscreen);
    return ESP_OK;
}

esp_err_t md_emu_stop(void)
{
    if (s_instance) {
        s_instance->stop_requested = true;
        int t = 0;
        while (s_task_running && t < 500) { vTaskDelay(pdMS_TO_TICKS(10)); t++; }
        if (s_task_running) ESP_LOGW(TAG, "%s: 模拟任务 5s 未退出 (可能卡在 SD 读取), 仍将释放", __func__);
    }
    if (s_core_inited) {
        deinit_genesis();
        s_core_inited = false;
    }
    if (s_instance) { heap_caps_free((void *)s_instance); s_instance = NULL; }
    if (s_rom_data) { heap_caps_free(s_rom_data); s_rom_data = NULL; s_rom_size = 0; }
    board_rlcd_set_nes_shade_source(NULL, 0, 0, 0);
    return ESP_OK;
}

void md_emu_wait_stopped(void)
{
    int t = 0;
    while (s_task_running && t < 300) { vTaskDelay(pdMS_TO_TICKS(10)); t++; }
}

void md_emu_pause(void)
{
    if (s_instance) { s_instance->paused = true; }
}

void md_emu_resume(void)
{
    if (s_instance) { s_instance->paused = false; }
}
