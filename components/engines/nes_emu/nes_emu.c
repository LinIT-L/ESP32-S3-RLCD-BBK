/**
 * @file nes_emu.c
 * @brief NES (nofrendo) 适配层 — esp-box-emu components/nes 移植
 *
 * 帧循环等价 esp-box-emu nes.cpp 的 init_nes/run_nes_rom:
 *   - nes_emulateframe() 每帧渲染 256x224 调色板帧 -> OSD 转 RGB565
 *   - do_audio_frame() 已在核心帧内把 APU 输出喂给 audio_player (22050Hz)
 *   - 显示: RGB565 -> 4 级灰度 1x, 经 board_rlcd_draw_gb_line 画到 1bit LCD
 */
#include "nes_emu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board_rlcd.h"
#include "audio_player.h"

#include <sys/stat.h>

#include "nes.h"
#include "nes_pal.h"
#include "nesinput.h"
#include "nes_rom.h"
#include "nes_shared_memory.h"
#include "osd.h"
#include "vid_drv.h"
#include "event.h"

static const char *TAG = "nes_emu";

/* V1.0.53: 16KB 在 nofrendo 深度调用链 (APU/mapper/FATFS) 下会溢出,
 * 破坏周边内存 -> 花屏 + 退出时 mkdir 踩坏 FATFS 锁崩溃. PSRAM 栈, 32KB 余量充足. */
#define NES_EMU_TASK_STACK_SIZE (32 * 1024)
#define NES_EMU_TASK_PRIORITY   4
#define NES_EMU_TASK_CORE       1
#define NES_EMU_FRAME_US        16667
#define NES_EMU_IDLE_DELAY_INTERVAL 30

#define NES_EMU_X_OFFSET ((BOARD_RLCD_WIDTH - NES_SCREEN_WIDTH) / 2)
#define NES_EMU_Y_OFFSET ((BOARD_RLCD_HEIGHT - NES_VISIBLE_HEIGHT) / 2)

EXT_RAM_BSS_ATTR static StackType_t s_nes_task_stack[NES_EMU_TASK_STACK_SIZE / sizeof(StackType_t)];
static StaticTask_t s_nes_task_tcb;

extern uint8_t *nes_video_shade_packed;
extern volatile bool nes_video_frame_ready;
extern volatile uint32_t nes_audio_peak;
extern uint8_t *nes_rom_data;
extern size_t nes_rom_size;
void nes_set_joypad_mask(uint8_t mask);

/* 来自 nes_shared_memory.c */
extern nes_t *nes_context;
extern nes6502_context *nes_cpu;
extern ppu_t *ppu;
volatile uint32_t nes_nmi_count = 0;
extern volatile uint32_t nes_apu_write_count;
extern volatile uint32_t nes_input_strobe_count;
extern volatile uint32_t nes_input_read_count;

typedef struct {
    volatile bool stop_requested;
    volatile bool paused;
    volatile bool pause_ack;
} nes_emu_instance_t;

static nes_emu_instance_t *s_instance = NULL;
static bool s_core_inited = false;
static bool s_task_running = false;
static int s_fullscreen = 2;   /* NES 显示模式: 0=点对点, 1=全屏, 2=拉伸 (默认拉伸) */
static uint32_t s_frame_us = NES_EMU_FRAME_US;   /* NTSC 60fps; PAL 卡带切 50fps */
/* event_init() 会向 nesinput 注册 joypad, 重复调用会累积重复条目;
 * 参考 esp-box-emu init_nes 只做一次, 这里跨会话也只做一次. */
static bool s_event_inited = false;
static volatile uint8_t s_output_volume = 80;
static nes_emu_progress_cb_t s_progress_cb = NULL;
static char s_save_path[160] = {0};   /* 电池存档路径 */
static bool s_sram_saved = false;      /* 本次会话存档已写回 */

/* === NES 电池存档 (V1.0.53) ===
 * 格式: 8 字节头 ('NESR' + uint32 size) + 原始 SRAM (0x400 * sram_banks 字节).
 * 目录: /sdcard/dict/NES/ (与步步高/GB 分开). */
#define NES_SRAM_MAGIC 0x5253454E  /* 'NESR' */
#define NES_SRAM_BANK_LENGTH 0x0400

static void nes_make_save_path(const char *rom_path)
{
    s_save_path[0] = 0;
    if (!rom_path) return;
    const char *slash = strrchr(rom_path, '/');
    const char *name = slash ? slash + 1 : rom_path;
    const char *dot = strrchr(name, '.');
    int len = dot ? (int)(dot - name) : (int)strlen(name);
    if (len <= 0 || len > 96) len = 32;
    snprintf(s_save_path, sizeof(s_save_path), "/sdcard/dict/NES/%.*s.sav", len, name);
}

static void nes_emu_load_sram(void)
{
    if (!nes_context || !nes_context->rominfo || !nes_context->rominfo->sram ||
        nes_context->rominfo->sram_banks <= 0 || s_save_path[0] == 0) return;
    size_t sram_len = NES_SRAM_BANK_LENGTH * (size_t)nes_context->rominfo->sram_banks;
    FILE *f = fopen(s_save_path, "rb");
    if (!f) return;
    uint32_t magic = 0, size = 0;
    if (fread(&magic, 1, 4, f) != 4 || fread(&size, 1, 4, f) != 4 ||
        magic != NES_SRAM_MAGIC || size > sram_len) {
        fclose(f);
        ESP_LOGW(TAG, "sram 头不匹配, 跳过加载 %s", s_save_path);
        return;
    }
    size_t rd = fread(nes_context->rominfo->sram, 1, size, f);
    fclose(f);
    if (rd == size) {
        ESP_LOGI(TAG, "sram 已加载 %s (%u 字节)", s_save_path, (unsigned)rd);
    }
}

static void nes_emu_save_sram(void)
{
    if (!nes_context || !nes_context->rominfo || !nes_context->rominfo->sram ||
        nes_context->rominfo->sram_banks <= 0 || s_save_path[0] == 0) return;
    size_t sram_len = NES_SRAM_BANK_LENGTH * (size_t)nes_context->rominfo->sram_banks;
    mkdir("/sdcard/dict", 0777);
    mkdir("/sdcard/dict/NES", 0777);
    FILE *f = fopen(s_save_path, "wb");
    if (!f) return;
    uint32_t magic = NES_SRAM_MAGIC;
    uint32_t size = (uint32_t)sram_len;
    fwrite(&magic, 1, 4, f);
    fwrite(&size, 1, 4, f);
    size_t wr = fwrite(nes_context->rominfo->sram, 1, sram_len, f);
    fclose(f);
    ESP_LOGI(TAG, "sram 已保存 %s (%u 字节)", s_save_path, (unsigned)wr);
}

void nes_emu_set_progress_cb(nes_emu_progress_cb_t cb)
{
    s_progress_cb = cb;
}

void nes_emu_set_volume(uint8_t volume)
{
    if (volume > 100) volume = 100;
    s_output_volume = volume;
    audio_player_set_volume(volume);
}

uint8_t nes_emu_get_volume(void)
{
    return s_output_volume;
}

void nes_emu_set_joypad(uint8_t joypad)
{
    nes_set_joypad_mask(joypad);
}

static void nes_emu_wait_until_us(int64_t deadline_us)
{
    while (1) {
        int64_t left = deadline_us - esp_timer_get_time();
        if (left <= 0) return;
        if (left > 1000) {
            vTaskDelay((TickType_t)(left / 1000));
        } else {
            taskYIELD();
        }
    }
}

static void nes_emu_task(void *arg)
{
    (void)arg;
    int64_t next_frame_us = esp_timer_get_time();
    uint32_t frame_count = 0;
    int64_t last_diag_us = next_frame_us;
    int64_t diag_emu_us = 0, diag_copy_us = 0;
    ESP_LOGI(TAG, "NES emulation task started (nofrendo)");

    while (1) {
        if (s_instance == NULL || s_instance->stop_requested) break;
        if (s_instance->paused) {
            s_instance->pause_ack = true;
            vTaskDelay(pdMS_TO_TICKS(16));
            continue;
        }

        int64_t t0 = esp_timer_get_time();
        nes_emulateframe(0);
        int64_t t1 = esp_timer_get_time();

        if (nes_video_frame_ready) {
            int64_t ts0 = esp_timer_get_time();
            uint8_t *disp = board_rlcd_nes_disp_buffer();
            if (disp && nes_video_shade_packed) {
                /* 只做 14KB 打包帧拷贝 (~0.2ms); 解包缩放+写 FB+SPI 全在 core0 视频任务.
                 * 20260812: 写私有缓冲后原子发布, 不再与视频任务抢锁,
                 * 消除核心帧节奏抖动 (copy 曾被锁阻塞到 ~1.8ms 导致闪烁). */
                memcpy(disp, nes_video_shade_packed,
                       NES_SCREEN_WIDTH * NES_VISIBLE_HEIGHT / 4);
                board_rlcd_nes_disp_publish();
                board_rlcd_flush_async();
            }
            nes_video_frame_ready = false;
            int64_t ts1 = esp_timer_get_time();
            diag_copy_us += (ts1 - ts0);
        }
        int64_t t2 = esp_timer_get_time();
        diag_emu_us += (t1 - t0);

        /* 诊断 (每 ~3 秒): 帧数据抽样 + 音频峰值 */
        frame_count++;
        if ((frame_count % 180) == 0) {
            int64_t now_us = esp_timer_get_time();
            uint32_t fps = (uint32_t)((uint64_t)180 * 1000000u /
                                      (uint64_t)(now_us - last_diag_us));
            uint32_t nonwhite = 0;
            if (nes_video_shade_packed) {
                int n = NES_SCREEN_WIDTH * NES_VISIBLE_HEIGHT / 4;
                for (int i = 0; i < n; i += 11)
                    if (nes_video_shade_packed[i] != 0x00) nonwhite++;
                ESP_LOGI(TAG, "NES diag: frames=%u fps=%u emu_avg=%dus copy_avg=%dus p0=%04x p1=%04x p2=%04x p3=%04x nonwhite=%u/%u peak=%u pc=%04x scan=%d ctrl0=%02x stat=%02x nmi=%u apuw=%u strobe=%u rd=%u",
                         (unsigned)frame_count,
                         (unsigned)fps,
                         (int)(diag_emu_us / 180), (int)(diag_copy_us / 180),
                         nes_video_shade_packed[0], nes_video_shade_packed[1],
                         nes_video_shade_packed[2], nes_video_shade_packed[3],
                         (unsigned)nonwhite,
                         (unsigned)(n / 11),
                         (unsigned)nes_audio_peak,
                         nes_cpu ? (unsigned)nes_cpu->pc_reg : 0u,
                         nes_context ? (int)nes_context->scanline : -1,
                         ppu ? (unsigned)ppu->ctrl0 : 0u,
                 ppu ? (unsigned)ppu->stat : 0u,
                 (unsigned)nes_nmi_count,
                 (unsigned)nes_apu_write_count,
                 (unsigned)nes_input_strobe_count,
                 (unsigned)nes_input_read_count);
            } else {
                ESP_LOGI(TAG, "NES diag: frames=%u fps=%u rgb=NULL",
                         (unsigned)frame_count, (unsigned)fps);
            }
            last_diag_us = now_us;
            diag_emu_us = 0;
            diag_copy_us = 0;
        }

        if (s_instance && s_instance->stop_requested) break;

        next_frame_us += s_frame_us;
        if (esp_timer_get_time() < next_frame_us) {
            nes_emu_wait_until_us(next_frame_us);
        } else {
            next_frame_us = esp_timer_get_time();
            if ((frame_count % NES_EMU_IDLE_DELAY_INTERVAL) == 0) {
                vTaskDelay(1);
            } else {
                taskYIELD();
            }
        }
    }

    nes_emu_set_joypad(0xFF);
    s_instance = NULL;
    board_rlcd_set_nes_shade_source(NULL, 0, 0, 0);
    /* 恢复默认 30Hz 节流, 避免影响后续 GB/GBC 引擎 */
    board_rlcd_video_task_set_interval_us(66667);   /* 15Hz */
    board_rlcd_video_task_stop();
    ESP_LOGI(TAG, "NES emulation task stopped");
    s_task_running = false;
    vTaskDelete(NULL);
}

/* 幂等初始化核心 (共享内存 + OSD + 驱动) */
esp_err_t nes_emu_background_init(void)
{
    if (s_core_inited) return ESP_OK;
    if (!board_rlcd_is_initialized()) return ESP_ERR_INVALID_STATE;

    nes_init_shared_memory();
    if (!s_event_inited) {
        event_init();
        s_event_inited = true;
    }
    if (osd_init() != 0) {
        ESP_LOGE(TAG, "osd_init 失败");
        return ESP_FAIL;
    }
    vidinfo_t video;
    osd_getvideoinfo(&video);
    if (vid_init(video.default_width, video.default_height, video.driver) != 0) {
        ESP_LOGE(TAG, "vid_init 失败");
        return ESP_FAIL;
    }
    nes_context = nes_create();
    if (!nes_context) {
        ESP_LOGE(TAG, "nes_create 失败");
        return ESP_ERR_NO_MEM;
    }
    event_set_system(system_nes);
    s_core_inited = true;
    ESP_LOGI(TAG, "nofrendo core ready (PSRAM)");
    return ESP_OK;
}

void nes_emu_unload(void)
{
    if (s_task_running) {
        if (s_instance) s_instance->stop_requested = true;
        for (int i = 0; i < 50 && s_task_running; i++) vTaskDelay(pdMS_TO_TICKS(2));
    }
    if (!s_core_inited) return;
    /* 存档在主任务上下文写回 (FATFS 安全) */
    if (!s_sram_saved && nes_context && nes_context->rominfo) {
        nes_emu_save_sram();
        s_sram_saved = true;
    }
    if (nes_context) {
        nes_poweroff();
    }
    nes_free_shared_memory();
    osd_shutdown();
    if (nes_video_shade_packed) {
        heap_caps_free(nes_video_shade_packed);
        nes_video_shade_packed = NULL;
    }
    if (nes_rom_data) {
        heap_caps_free(nes_rom_data);
        nes_rom_data = NULL;
        nes_rom_size = 0;
    }
    s_core_inited = false;
    ESP_LOGI(TAG, "nofrendo unloaded");
}

esp_err_t nes_emu_start(const char *path)
{
    if (!path) return ESP_ERR_INVALID_ARG;
    if (s_task_running) return ESP_ERR_INVALID_STATE;

    nes_make_save_path(path);
    s_sram_saved = false;

    esp_err_t r = nes_emu_background_init();
    if (r != ESP_OK) return r;

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "打开失败: %s", path);
        return ESP_FAIL;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 2 * 1024 * 1024) {
        ESP_LOGE(TAG, "ROM 大小异常: %ld", sz);
        fclose(f);
        return ESP_FAIL;
    }
    if (nes_rom_data) heap_caps_free(nes_rom_data);
    nes_rom_data = heap_caps_malloc((size_t)sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!nes_rom_data) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    size_t rd = fread(nes_rom_data, 1, (size_t)sz, f);
    fclose(f);
    if (rd != (size_t)sz) {
        heap_caps_free(nes_rom_data);
        nes_rom_data = NULL;
        ESP_LOGE(TAG, "读取不完整: %u/%ld", (unsigned)rd, sz);
        return ESP_FAIL;
    }
    nes_rom_size = (size_t)sz;
    /* 诊断: iNES 头制式字节 (byte10 bit0 / byte12 bit1=PAL), 用于判断卡带是否 PAL */
    if (sz >= 16) {
        ESP_LOGI(TAG, "ROM header: b4=%02x b5=%02x b6=%02x b7=%02x b8=%02x b9=%02x b10=%02x b11=%02x b12=%02x b13=%02x",
                 nes_rom_data[4], nes_rom_data[5], nes_rom_data[6], nes_rom_data[7],
                 nes_rom_data[8], nes_rom_data[9], nes_rom_data[10], nes_rom_data[11],
                 nes_rom_data[12], nes_rom_data[13]);
        s_frame_us = (nes_rom_data[12] & 0x02) ? 20000u : NES_EMU_FRAME_US;
    }
    /* 先清屏再画进度条, 避免 progress_cb(50) 画的边框/文件名被后续 board_rlcd_clear 冲掉 */
    board_rlcd_clear(BOARD_RLCD_COLOR_WHITE);
    if (s_progress_cb) s_progress_cb(50);

    if (nes_insertcart(path, nes_context) != 0) {
        ESP_LOGE(TAG, "nes_insertcart 失败: %s", path);
        heap_caps_free(nes_rom_data);
        nes_rom_data = NULL;
        return ESP_FAIL;
    }
    /* 注意: nofrendo 的 nes_rom_load()/rom_loadrom() 只是把 rominfo->rom/vrom
     * 指针指向 osd_getromdata() 返回的缓冲 (没有拷入共享池).
     * 因此 nes_rom_data 必须保持有效到本局结束, 由 nes_emu_unload() 统一释放.
     * 若提前释放, 后续 vid_setmode()/video_task_start()/音频环形缓冲等 PSRAM
     * 分配会复用这块内存, 覆盖 PRG 数据 -> 首局白屏/复位向量跑飞/主循环卡死.
     * 参考 esp-box-emu: mmap.cpp 的 romdata 是 4MB 常驻 PSRAM, 会话内从不释放. */
    nes_rom_size = (size_t)sz;

    vid_setmode(NES_SCREEN_WIDTH, NES_VISIBLE_HEIGHT);
    nes_prep_emulation(NULL, nes_context);
    nes_reset(SOFT_RESET);
    nes_emu_load_sram();
    if (s_progress_cb) s_progress_cb(100);

    nes_emu_instance_t *inst = heap_caps_calloc(1, sizeof(*inst), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!inst) return ESP_ERR_NO_MEM;
    s_instance = inst;

    board_rlcd_video_task_start();
    /* NES 动作游戏: 刷新节流提到 ~60Hz (仅当 SPI 传输实际够快时有效,
     * 由 board_shim 的 flush diag 输出实际耗时) */
    board_rlcd_video_task_set_interval_us(66667);   /* 15Hz */
    board_rlcd_set_nes_shade_source(nes_video_shade_packed, NES_SCREEN_WIDTH,
                                    NES_VISIBLE_HEIGHT, s_fullscreen);
    TaskHandle_t task = xTaskCreateStaticPinnedToCore(nes_emu_task, "nes_emu",
                                                      NES_EMU_TASK_STACK_SIZE / sizeof(StackType_t),
                                                      NULL, NES_EMU_TASK_PRIORITY,
                                                      s_nes_task_stack, &s_nes_task_tcb,
                                                      NES_EMU_TASK_CORE);
    if (task == NULL) {
        board_rlcd_video_task_stop();
        s_instance = NULL;
        heap_caps_free(inst);
        return ESP_ERR_NO_MEM;
    }
    s_task_running = true;
    ESP_LOGI(TAG, "启动 NES: %s (%ld bytes)", path, sz);
    return ESP_OK;
}

esp_err_t nes_emu_stop(void)
{
    if (s_instance) s_instance->stop_requested = true;
    /* V1.0.53: 同步等待任务完全退出 (含视频刷新任务停止),
     * 避免菜单重绘与游戏刷新并发操作 SPI/帧缓冲导致崩溃. */
    nes_emu_wait_stopped();
    /* 存档在任务完全退出后由主任务上下文写回 (FATFS 安全) */
    if (!s_sram_saved && nes_context && nes_context->rominfo) {
        nes_emu_save_sram();
        s_sram_saved = true;
    }
    return ESP_OK;
}

void nes_emu_wait_stopped(void)
{
    for (int i = 0; i < 50 && s_task_running; i++) vTaskDelay(pdMS_TO_TICKS(2));
}

void nes_emu_pause(void)
{
    if (s_instance) {
        s_instance->pause_ack = false;
        s_instance->paused = true;
        for (uint16_t i = 0; i < 100 && s_instance && !s_instance->pause_ack; i++)
            vTaskDelay(pdMS_TO_TICKS(2));
    }
}

void nes_emu_resume(void)
{
    if (s_instance) {
        s_instance->paused = false;
        s_instance->pause_ack = false;
    }
}

void nes_emu_set_fullscreen(int mode)
{
    if (mode < 0) mode = 0;
    if (mode > 2) mode = 2;
    s_fullscreen = mode;
    if (s_core_inited && nes_video_shade_packed) {
        board_rlcd_set_nes_shade_source(nes_video_shade_packed, NES_SCREEN_WIDTH,
                                        NES_VISIBLE_HEIGHT, s_fullscreen);
    }
}
