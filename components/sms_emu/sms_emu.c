/* sms_emu.c — SMS/GG (SMSPlus) 适配层
 * 移植自 esp-box-emu components/sms. 帧循环:
 *   - run_sms_rom() 每帧渲染(索引帧) + 混音
 *   - sms_video_rgb565() 取 RGB565 帧 -> 4 级灰度 2bit 打包 -> board_rlcd shade 管线
 *   - 音频由 BoxEmu 桩直接喂 audio_player */
#include "sms_emu.h"

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

static const char *TAG = "sms_emu";

#define SMS_EMU_TASK_STACK_SIZE (32 * 1024)
#define SMS_EMU_TASK_PRIORITY   4
#define SMS_EMU_TASK_CORE       1

#define SMS_MAX_W   256
#define SMS_MAX_H   192
#define SMS_MAX_BYTES (SMS_MAX_W * SMS_MAX_H * 2)

/* SMSPlus 主控 (C++ 实现, extern "C" 导出) */
extern void init_sms(uint8_t *romdata, size_t rom_data_size);
extern void init_gg(uint8_t *romdata, size_t rom_data_size);
extern void reset_sms(void);
extern void run_sms_rom(void);
extern void deinit_sms(void);
extern int  sms_video_rgb565(uint8_t *dst, int max_bytes, int *w, int *h);
extern void sms_emu_savestate_load(const char *path);
extern void sms_emu_savestate_save(const char *path);

EXT_RAM_BSS_ATTR static StackType_t s_sms_task_stack[SMS_EMU_TASK_STACK_SIZE / sizeof(StackType_t)];
static StaticTask_t s_sms_task_tcb;

static uint8_t *s_rom_data = NULL;
static size_t   s_rom_size = 0;
static uint8_t *s_shade_packed = NULL;   /* 256*192/4 = 12288 字节 (PSRAM) */
static uint8_t *s_rgb565_frame = NULL;   /* 256*192*2 字节 (PSRAM) */

typedef struct {
    volatile bool stop_requested;
    volatile bool paused;
    volatile bool pause_ack;
} sms_emu_instance_t;

static sms_emu_instance_t *s_instance = NULL;
static bool s_core_inited = false;
static bool s_task_running = false;
static int  s_fullscreen = 2;
static volatile uint8_t s_output_volume = 80;
static sms_emu_progress_cb_t s_progress_cb = NULL;
static char s_save_path[160] = {0};

static void sms_emu_set_save_path(const char *rom_path)
{
    s_save_path[0] = 0;
    if (!rom_path) return;
    const char *slash = strrchr(rom_path, '/');
    const char *name = slash ? slash + 1 : rom_path;
    const char *dot = strrchr(name, '.');
    int len = dot ? (int)(dot - name) : (int)strlen(name);
    if (len <= 0 || len > 96) len = 32;
    snprintf(s_save_path, sizeof(s_save_path), "/sdcard/dict/SMS/%.*s.sav", len, name);
}

void sms_emu_set_progress_cb(sms_emu_progress_cb_t cb) { s_progress_cb = cb; }

void sms_emu_set_volume(uint8_t volume)
{
    if (volume > 100) volume = 100;
    s_output_volume = volume;
    audio_player_set_volume(volume);
}

uint8_t sms_emu_get_volume(void) { return s_output_volume; }

void sms_emu_set_joypad(uint8_t joypad) { (void)joypad; }

void sms_emu_set_fullscreen(int mode) { s_fullscreen = (mode < 0) ? 0 : (mode > 2 ? 2 : mode); }

static uint8_t rgb565_to_level(uint16_t px)
{
    uint32_t r = (px >> 11) & 0x1F;
    uint32_t g = (px >> 5)  & 0x3F;
    uint32_t b = px & 0x1F;
    uint32_t r8 = (r << 3) | (r >> 2);
    uint32_t g8 = (g << 2) | (g >> 4);
    uint32_t b8 = (b << 3) | (b >> 2);
    uint32_t luma = (r8 * 77u + g8 * 150u + b8 * 29u) >> 8;
    if (luma < 64)      return 3;
    else if (luma < 128) return 2;
    else if (luma < 192) return 1;
    return 0;
}

static void sms_pack_frame(const uint8_t *rgb565, int w, int h, uint8_t *packed)
{
    int n = w * h;
    for (int i = 0; i < n; i += 4) {
        packed[i >> 2] = (uint8_t)(rgb565_to_level(rgb565[i] | (rgb565[i + 1] << 8)) |
                                   (rgb565_to_level(rgb565[i + 2] | (rgb565[i + 3] << 8)) << 2) |
                                   (rgb565_to_level(rgb565[i + 4] | (rgb565[i + 5] << 8)) << 4) |
                                   (rgb565_to_level(rgb565[i + 6] | (rgb565[i + 7] << 8)) << 6));
    }
}

static void sms_emu_task(void *arg)
{
    (void)arg;
    uint32_t frame_count = 0;
    ESP_LOGI(TAG, "SMS/GG emulation task started (SMSPlus)");

    while (1) {
        if (s_instance == NULL || s_instance->stop_requested) break;
        if (s_instance->paused) {
            s_instance->pause_ack = true;
            vTaskDelay(pdMS_TO_TICKS(16));
            continue;
        }

        run_sms_rom();

        int w = 0, h = 0;
        int n = sms_video_rgb565(s_rgb565_frame, SMS_MAX_BYTES, &w, &h);
        if (n > 0 && s_shade_packed && w > 0 && h > 0) {
            memset(s_shade_packed, 0, SMS_MAX_W * SMS_MAX_H / 4);
            sms_pack_frame(s_rgb565_frame, w, h, s_shade_packed);
            uint8_t *disp = board_rlcd_nes_disp_buffer();
            if (disp) {
                memcpy(disp, s_shade_packed, SMS_MAX_W * SMS_MAX_H / 4);
                board_rlcd_nes_disp_publish();
                board_rlcd_flush_async();
            }
        }

        if (++frame_count % 300 == 0 && s_save_path[0]) {
            sms_emu_savestate_save(s_save_path);
        }
    }

    if (s_save_path[0]) sms_emu_savestate_save(s_save_path);
    ESP_LOGI(TAG, "SMS/GG emulation task exit");
    s_task_running = false;
    vTaskDelete(NULL);
}

esp_err_t sms_emu_background_init(void)
{
    if (!s_shade_packed) {
        s_shade_packed = heap_caps_malloc(SMS_MAX_W * SMS_MAX_H / 4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_shade_packed) return ESP_ERR_NO_MEM;
    }
    if (!s_rgb565_frame) {
        s_rgb565_frame = heap_caps_malloc(SMS_MAX_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_rgb565_frame) return ESP_ERR_NO_MEM;
    }
    memset(s_shade_packed, 0, SMS_MAX_W * SMS_MAX_H / 4);
    return ESP_OK;
}

void sms_emu_unload(void)
{
    if (s_shade_packed) { heap_caps_free(s_shade_packed); s_shade_packed = NULL; }
    if (s_rgb565_frame) { heap_caps_free(s_rgb565_frame); s_rgb565_frame = NULL; }
}

esp_err_t sms_emu_start(const char *path)
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
    const char *ext = strrchr(path, '.');
    bool is_gg = ext && (strcasecmp(ext, ".gg") == 0);
    if (is_gg) init_gg(s_rom_data, s_rom_size);
    else       init_sms(s_rom_data, s_rom_size);
    if (s_progress_cb) s_progress_cb(100);
    s_core_inited = true;
    sms_emu_set_save_path(path);
    if (s_save_path[0]) sms_emu_savestate_load(s_save_path);

    s_instance = heap_caps_malloc(sizeof(sms_emu_instance_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_instance) { deinit_sms(); s_core_inited = false; return ESP_ERR_NO_MEM; }
    memset((void *)s_instance, 0, sizeof(sms_emu_instance_t));

    s_task_running = true;
    xTaskCreateStaticPinnedToCore(sms_emu_task, "sms_emu", SMS_EMU_TASK_STACK_SIZE / sizeof(StackType_t),
                                  NULL, SMS_EMU_TASK_PRIORITY, s_sms_task_stack, &s_sms_task_tcb,
                                  SMS_EMU_TASK_CORE);
    board_rlcd_set_nes_shade_source(s_shade_packed, SMS_MAX_W, SMS_MAX_H, s_fullscreen);
    return ESP_OK;
}

esp_err_t sms_emu_stop(void)
{
    if (s_instance) {
        s_instance->stop_requested = true;
        int t = 0;
        while (s_task_running && t < 500) { vTaskDelay(pdMS_TO_TICKS(10)); t++; }
        if (s_task_running) ESP_LOGW(TAG, "%s: 模拟任务 5s 未退出 (可能卡在 SD 读取), 仍将释放", __func__);
    }
    if (s_core_inited) {
        deinit_sms();
        s_core_inited = false;
    }
    if (s_instance) { heap_caps_free((void *)s_instance); s_instance = NULL; }
    if (s_rom_data) { heap_caps_free(s_rom_data); s_rom_data = NULL; s_rom_size = 0; }
    board_rlcd_set_nes_shade_source(NULL, 0, 0, 0);
    return ESP_OK;
}

void sms_emu_wait_stopped(void)
{
    int t = 0;
    while (s_task_running && t < 300) { vTaskDelay(pdMS_TO_TICKS(10)); t++; }
}

void sms_emu_pause(void)
{
    if (s_instance) { s_instance->paused = true; }
}

void sms_emu_resume(void)
{
    if (s_instance) { s_instance->paused = false; }
}
