/**
 * @file gbc_emu.c
 * @brief GB/GBC 统一模拟核心 — esp-box-emu (gnuboy) 移植
 *
 * 替换原 peanut-gb (gb_emu) + 旧 gnuboy (gbc_emu):
 *   - 核心源码来自 esp-box-emu components/gbc/gnuboy (含 fastmem/PSRAM 共享内存优化)
 *   - 本文件实现 esp-box-emu gameboy.cpp 的等效逻辑 (C 版), 保持 gbc_emu.h 公共接口,
 *     gb_emu 组件退化为薄兼容层转发到本核心
 *   - 显示: gnuboy 输出 RGB565 小端 160x144, 转 BE 后经 board_rlcd_draw_gbc_line_2x_rgb565_be
 *     画成 4 级灰度 2x 到 1bit LCD (与旧 GBC 路径一致)
 *   - 音频: sound_mix 填 pcm 缓冲 (24000Hz 立体声), 每帧喂 audio_player
 *   - 刷新: board_rlcd_flush_async() 交给 core0 独立刷新任务, 不阻塞模拟
 */
#include "gbc_emu.h"

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

#include "gnuboy/gnuboy.h"
#include "gnuboy/defs.h"
#include "gnuboy/regs.h"
#include "gnuboy/hw.h"
#include "gnuboy/cpu.h"
#include "gnuboy/mem.h"
#include "gnuboy/lcd.h"
#include "gnuboy/fb.h"
#include "gnuboy/pcm.h"
#include "gnuboy/loader.h"
#include "gnuboy/sound.h"
#include "gnuboy/rtc.h"

#include <sys/stat.h>

static const char *TAG = "gbc_emu";

#define GBC_EMU_SCREEN_WIDTH  160
#define GBC_EMU_SCREEN_HEIGHT 144
#define GBC_EMU_DISPLAY_SCALE 2
#define GBC_EMU_DISPLAY_X_OFFSET ((BOARD_RLCD_WIDTH - GBC_EMU_SCREEN_WIDTH * GBC_EMU_DISPLAY_SCALE) / 2)
#define GBC_EMU_DISPLAY_Y_OFFSET ((BOARD_RLCD_HEIGHT - GBC_EMU_SCREEN_HEIGHT * GBC_EMU_DISPLAY_SCALE) / 2)
#define GBC_EMU_TASK_STACK_SIZE  (16 * 1024)
#define GBC_EMU_TASK_PRIORITY    4
#define GBC_EMU_TASK_CORE        1
#define GBC_EMU_FRAME_US         16667
#define GBC_EMU_IDLE_DELAY_INTERVAL 30

#define GBC_EMU_AUDIO_SAMPLE_RATE 24000  /* V1.0.61: 恢复官方 esp-box-emu gnuboy 采样率 (旧适配器同款, GB 声音正常) */
#define GBC_EMU_AUDIO_MAX_SAMPLES 8192   /* int16 数 (立体声交错), ~171ms @24kHz */

/* V1.0.53: 任务栈 PSRAM 静态, 不占内部 RAM */
EXT_RAM_BSS_ATTR static StackType_t s_gbc_task_stack[GBC_EMU_TASK_STACK_SIZE / sizeof(StackType_t)];
static StaticTask_t s_gbc_task_tcb;

/* === 核心全局 (esp-box-emu gameboy.cpp/gbc_shared_memory.cpp 定义) === */
uint32_t frame = 0;
uint16_t *displayBuffer[2] = { NULL, NULL };
struct cpu *cpu = NULL;
/* lcd/scan/bgdup 由核心 lcd.c 定义, 这里只分配内存时引用 */
extern byte *bgdup;   /* lcd.c:570 定义 */
struct fb fb;    /* esp-box-emu 在 gameboy.cpp 定义, 此处 C 版定义 */
struct pcm pcm;

/* 共享内存 (PSRAM): vram/wram 由 core 高频访问, 放 PSRAM 且用 fastmem 直映射 */
static uint8_t *s_vram = NULL;
static uint8_t *s_wram = NULL;
static int16_t *s_audio_buf = NULL;
static uint8_t *s_rom_data = NULL;   /* 当前 ROM 数据 (gnuboy 直接引用, 停止时才释放) */
static size_t   s_rom_size = 0;
static bool     s_rom_owned = false; /* true = 本组件负责释放 s_rom_data */
static int      s_current_buffer = 0;
/* V1.0.60: GB/GBC 音频按真实时间重采样.
 * sound_mix 按游戏时钟产出样本 (每模拟帧 ~735), 模拟核心只有 ~50fps 时
 * 产量低于扬声器消耗, 环形缓冲被掏空 -> 卡顿. 这里每帧喂 44100×实际耗时
 * 个样本 (线性插值拉伸/压缩), 产量恒等于消耗; 静音帧也喂静音保持水位. */

typedef struct {
    volatile bool stop_requested;
    volatile bool paused;
    volatile bool pause_ack;
} gbc_emu_instance_t;

static gbc_emu_instance_t *s_instance = NULL;
static bool s_core_mem_allocated = false;
static bool s_task_running = false;
static int  s_display_mode = 1;   /* 0=点对点(1x), 1=全屏(2x), 2=强制拉伸 */
static volatile uint8_t s_output_volume = 80;
static gbc_emu_progress_cb_t s_progress_cb = NULL;
static char s_save_path[160] = {0};   /* 电池存档路径 (派生自 ROM 路径) */
static char s_save_dir[64] = "/sdcard/dict/GB";   /* 电池存档目录 (gbc_emu_set_save_dir 可改) */

/* === 电池存档持久化 (V1.0.53) ===
 * 格式: 8 字节头 ('GBSR' + uint32 size) + 原始 SRAM (8192 * mbc.ramsize 字节).
 * 目录: /sdcard/dict/GB/ (与步步高 /sdcard/dict/ 分开, 避免同名冲突). */
#define GB_SRAM_MAGIC 0x52534247  /* 'GBSR' 小端 */

static void gbc_make_save_path(const char *rom_path)
{
    s_save_path[0] = 0;
    if (!rom_path) return;
    const char *slash = strrchr(rom_path, '/');
    const char *name = slash ? slash + 1 : rom_path;
    const char *dot = strrchr(name, '.');
    int len = dot ? (int)(dot - name) : (int)strlen(name);
    if (len <= 0 || len > 96) len = 32;
    snprintf(s_save_path, sizeof(s_save_path), "%s/%.*s.sav", s_save_dir, len, name);
}

void gbc_emu_set_save_path(const char *rom_path)
{
    gbc_make_save_path(rom_path);
}

/* 设置电池存档目录 (默认 "/sdcard/dict/GB"). gb_emu 兼容层用它对 GB/GBC 分区存档.
 * 在 load_rom / start 之前调用生效. */
void gbc_emu_set_save_dir(const char *dir)
{
    if (!dir) return;
    snprintf(s_save_dir, sizeof(s_save_dir), "%s", dir);
    size_t n = strlen(s_save_dir);
    while (n > 1 && s_save_dir[n - 1] == '/') s_save_dir[--n] = 0;   /* 去末尾斜杠 */
}

static void gbc_emu_load_sram(void)
{
    if (!mbc.batt || !ram.sbank || !mbc.ramsize || s_save_path[0] == 0) return;
    size_t sram_len = (size_t)8192 * mbc.ramsize;
    /* 有电池位即视为"已加载", 保证首次游玩退出时也会创建存档 */
    ram.loaded = 1;
    FILE *f = fopen(s_save_path, "rb");
    if (!f) return;
    uint32_t magic = 0, size = 0;
    if (fread(&magic, 1, 4, f) != 4 || fread(&size, 1, 4, f) != 4 ||
        magic != GB_SRAM_MAGIC || size > sram_len) {
        fclose(f);
        ESP_LOGW(TAG, "sram 头不匹配, 跳过加载 %s", s_save_path);
        return;
    }
    size_t rd = fread(ram.sbank, 1, size, f);
    fclose(f);
    if (rd == size) {
        ram.loaded = 1;
        ESP_LOGI(TAG, "sram 已加载 %s (%u 字节)", s_save_path, (unsigned)rd);
    }
}

static void gbc_emu_save_sram(void)
{
    if (!mbc.batt || !ram.sbank || !mbc.ramsize || !ram.loaded || s_save_path[0] == 0) return;
    size_t sram_len = (size_t)8192 * mbc.ramsize;
    mkdir("/sdcard/dict", 0777);
    mkdir(s_save_dir, 0777);
    FILE *f = fopen(s_save_path, "wb");
    if (!f) return;
    uint32_t magic = GB_SRAM_MAGIC;
    uint32_t size = (uint32_t)sram_len;
    fwrite(&magic, 1, 4, f);
    fwrite(&size, 1, 4, f);
    size_t wr = fwrite(ram.sbank, 1, sram_len, f);
    fclose(f);
    ESP_LOGI(TAG, "sram 已保存 %s (%u 字节)", s_save_path, (unsigned)wr);
}

/* 精确等到 deadline_us: 最后 2ms 用 esp_timer 忙等, 避免 1ms tick 粒度
 * 平均超调 0.5ms/帧 (实际 ~59.5fps) 导致音频生产略慢于消耗 -> 环形缓冲
 * 慢慢掏空 -> 声音卡顿. 忙等只占 <2ms, 不影响音频任务. */
static void gbc_wait_until_us(int64_t deadline_us)
{
    while (1) {
        int64_t left = deadline_us - esp_timer_get_time();
        if (left <= 0) return;
        if (left > 2000) {
            vTaskDelay((TickType_t)((left - 2000) / 1000));
        } else {
            /* 最后 2ms: 忙等, 保证 60.0fps */
        }
    }
}

void gbc_emu_set_volume(uint8_t volume)
{
    if (volume > 100) volume = 100;
    s_output_volume = volume;
    audio_player_set_volume(volume);
}

uint8_t gbc_emu_get_volume(void)
{
    return s_output_volume;
}

void gbc_emu_set_progress_cb(gbc_emu_progress_cb_t cb)
{
    s_progress_cb = cb;
}

/* 输入: GB/peanut 布局低电平有效 → gnuboy pad_set (高电平有效) */
void gbc_emu_set_joypad(uint8_t joypad)
{
    static uint8_t s_last_joy = 0xFF;
    if (joypad != s_last_joy) {
        ESP_LOGI(TAG, "GBC joypad=0x%02X", (unsigned)joypad);
        s_last_joy = joypad;
    }
    pad_set(PAD_RIGHT,  !(joypad & (1u << 4)));
    pad_set(PAD_LEFT,   !(joypad & (1u << 5)));
    pad_set(PAD_UP,     !(joypad & (1u << 6)));
    pad_set(PAD_DOWN,   !(joypad & (1u << 7)));
    pad_set(PAD_A,      !(joypad & (1u << 0)));
    pad_set(PAD_B,      !(joypad & (1u << 1)));
    pad_set(PAD_SELECT, !(joypad & (1u << 2)));
    pad_set(PAD_START,  !(joypad & (1u << 3)));
}

static void gbc_emu_feed_audio(void)
{
    /* V1.0.61: 官方 esp-box-emu 方式 — 核心原生产出多少就直喂多少,
     * 不做重采样/插值 (旧适配器同款, 验证 GB 声音正常).
     * 之前自定义的实时重采样路径引入过: 缓冲越界 (进游戏重启)、
     * 2 倍拉伸 (声音慢一半). 双速 CGB 的产量减半问题已在 cpu.c
     * sound_advance 修复, 现在 GB/GBC 都是完整产量 (24000 样本/秒),
     * 直喂即可, 环形缓冲 (16K 帧, ~680ms) 足够吸收帧率抖动. */
    int produced = (pcm.pos > 0) ? (pcm.pos / 2) : 0;
    if (produced > 0) {
        audio_player_feed_pcm(pcm.buf, (size_t)produced, GBC_EMU_AUDIO_SAMPLE_RATE);
    }
    pcm.pos = 0;
}

/* 一帧 (等价 esp-box-emu run_to_vblank) */
static void gbc_emu_run_frame(void)
{
    cpu_emulate(2280);
    while (R_LY > 0 && R_LY < GBC_EMU_SCREEN_HEIGHT)
        emu_step();

    /* 隔帧出画面 (30Hz 刷新节流), 与转换耗时匹配, 避免 core0 满载
     * (60Hz + 每帧渲染会导致转换超时 -> 看门狗/蓝牙掉线) */
    if ((frame % 2) == 0) {
        board_rlcd_set_gbc_frame_source(displayBuffer[s_current_buffer],
                                        GBC_EMU_SCREEN_WIDTH, GBC_EMU_SCREEN_HEIGHT,
                                        s_display_mode);
        s_current_buffer ^= 1;
        fb.ptr = (uint8_t *)displayBuffer[s_current_buffer];
    }

    rtc_tick();
    sound_mix();
    gbc_emu_feed_audio();

    if (!(R_LCDC & 0x80))
        /* V1.0.61: 恢复原版 gnuboy 的整帧模拟.
         * 旧代码 32832/3 只模拟 1/3 帧周期, LCDC 关闭期间 (开机动画/转场)
         * 音频产量只有 277/帧, 被重采样拉伸 2.7 倍 → 声音慢一半+发粗.
         * 整帧模拟后 LCDC 关闭时也产满 735/帧, 音速正常. */
        cpu_emulate(32832);
    while (R_LY > 0)
        emu_step();
    ++frame;
}

static void gbc_emu_task(void *arg)
{
    (void)arg;
    int64_t next_frame_us = esp_timer_get_time();
    uint32_t frame_count = 0;
    int64_t last_diag_us = next_frame_us;
    int64_t diag_frame_us = 0;
    ESP_LOGI(TAG, "GBC/GB emulation task started (esp-box-emu gnuboy)");

    while (1) {
        if (s_instance == NULL || s_instance->stop_requested) break;
        if (s_instance->paused) {
            s_instance->pause_ack = true;
            vTaskDelay(pdMS_TO_TICKS(16));
            continue;
        }

        int64_t t0 = esp_timer_get_time();
        gbc_emu_run_frame();
        int64_t t1 = esp_timer_get_time();
        diag_frame_us += (t1 - t0);
        frame_count++;

        if ((frame_count % 180) == 0) {
            int64_t now_us = esp_timer_get_time();
            uint32_t fps = (uint32_t)((uint64_t)180 * 1000000u /
                                      (uint64_t)(now_us - last_diag_us));
            ESP_LOGI(TAG, "GBC diag: frames=%u fps=%u frame_avg=%dus pcm.pos=%d",
                     (unsigned)frame_count, (unsigned)fps,
                     (int)(diag_frame_us / 180), (int)pcm.pos);
            last_diag_us = now_us;
            diag_frame_us = 0;
        }

        if (s_instance && s_instance->stop_requested) break;

        /* 帧就绪 -> 异步刷新 (core0 视频任务节流 ~30Hz) */
        board_rlcd_flush_async();

        /* 60fps 节流 */
        next_frame_us += GBC_EMU_FRAME_US;
        if (esp_timer_get_time() < next_frame_us) {
            gbc_wait_until_us(next_frame_us);
        } else {
            next_frame_us = esp_timer_get_time();
            if ((frame_count % GBC_EMU_IDLE_DELAY_INTERVAL) == 0) {
                vTaskDelay(1);
            } else {
                taskYIELD();
            }
        }
    }

    gbc_emu_set_joypad(0xFF);
    pad_refresh();
    board_rlcd_set_gbc_frame_source(NULL, 0, 0, 0);
    gbc_emu_save_sram();       /* 存档必须在 loader_unload 释放 sbank 前写回 */
    loader_unload();
    if (s_rom_owned && s_rom_data) {
        heap_caps_free(s_rom_data);
    }
    s_rom_data = NULL;
    s_rom_size = 0;
    s_rom_owned = false;
    s_instance = NULL;
    board_rlcd_video_task_stop();
    ESP_LOGI(TAG, "GBC/GB emulation task stopped");
    s_task_running = false;
    vTaskDelete(NULL);
}

/* 后台预加载: 只分配核心内存, 不启动任务 (幂等) */
esp_err_t gbc_emu_background_init(void)
{
    if (s_core_mem_allocated) return ESP_OK;
    if (!board_rlcd_is_initialized()) {
        ESP_LOGE(TAG, "RLCD is not initialized");
        return ESP_ERR_INVALID_STATE;
    }

#define GB_ALLOC(size, var, name) do { \
    var = heap_caps_malloc((size), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); \
    if (!var) { ESP_LOGE(TAG, "分配 %s 失败 (%d B)", name, (int)(size)); goto fail; } \
    memset(var, 0, (size)); \
} while (0)

    GB_ALLOC(16 * 1024, s_vram, "vram");
    /* V1.0.60: WRAM (CPU 工作内存) 是每指令随机访问的热数据, 放内部 RAM 提速
     * (32KB, 内部最大连续块 ~39KB 可容纳). PSRAM 随机访问 + 缓存抖动是
     * GB/GBC 只有 40-57fps 的主因. */
    s_wram = heap_caps_malloc(32 * 1024, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_wram) { ESP_LOGE(TAG, "分配 wram 失败 (内部 RAM 不足), 回退 PSRAM"); }
    if (!s_wram) {
        s_wram = heap_caps_malloc(32 * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!s_wram) goto fail;
    memset(s_wram, 0, 32 * 1024);
    GB_ALLOC(sizeof(struct cpu), cpu, "cpu");
    GB_ALLOC(sizeof(struct lcd), lcd, "lcd");
    GB_ALLOC(sizeof(struct gbc_scan), scan, "scan");
    GB_ALLOC(4096, gbc_filebuf, "gbc_filebuf");
    GB_ALLOC(256, bgdup, "bgdup");
    GB_ALLOC(GBC_EMU_AUDIO_MAX_SAMPLES * sizeof(int16_t), s_audio_buf, "audio");
    GB_ALLOC(GBC_EMU_SCREEN_WIDTH * GBC_EMU_SCREEN_HEIGHT * 2, displayBuffer[0], "fb0");
    GB_ALLOC(GBC_EMU_SCREEN_WIDTH * GBC_EMU_SCREEN_HEIGHT * 2, displayBuffer[1], "fb1");
#undef GB_ALLOC

    /* 接线: gnuboy 核心的内存指针 (与 esp-box-emu gbc_shared_memory.cpp 一致) */
    lcd->vbank = s_vram;
    ram.ibank = s_wram;
    pcm.buf = s_audio_buf;
    pcm.len = GBC_EMU_AUDIO_MAX_SAMPLES;
    pcm.hz = GBC_EMU_AUDIO_SAMPLE_RATE;
    pcm.stereo = 1;

    fb.w = GBC_EMU_SCREEN_WIDTH;
    fb.h = GBC_EMU_SCREEN_HEIGHT;
    fb.pelsize = 2;
    fb.pitch = fb.w * fb.pelsize;
    fb.indexed = 0;
    fb.enabled = 1;
    fb.ptr = (uint8_t *)displayBuffer[0];
    s_current_buffer = 0;
    frame = 0;

    s_core_mem_allocated = true;
    ESP_LOGI(TAG, "esp-box-emu gnuboy core memory ready (PSRAM)");
    return ESP_OK;

fail:
    gbc_emu_unload();
    return ESP_ERR_NO_MEM;
}

void gbc_emu_unload(void)
{
    if (s_task_running) {
        if (s_instance) s_instance->stop_requested = true;
        for (int i = 0; i < 50 && s_task_running; i++) vTaskDelay(pdMS_TO_TICKS(2));
    }
    if (!s_core_mem_allocated) return;

    /* 清理核心静态状态, 再释放共享内存 */
    memset(&hw, 0, sizeof(hw));
    memset(&mbc, 0, sizeof(mbc));
    memset(&rom, 0, sizeof(rom));
    memset(&ram, 0, sizeof(ram));
    memset(&pcm, 0, sizeof(pcm));
    memset(&fb, 0, sizeof(fb));
    memset(&rtc, 0, sizeof(rtc));
    board_rlcd_set_gbc_frame_source(NULL, 0, 0, 0);

    if (s_vram)        { heap_caps_free(s_vram);        s_vram = NULL; }
    if (s_wram)        { heap_caps_free(s_wram);        s_wram = NULL; }
    if (s_audio_buf)   { heap_caps_free(s_audio_buf);   s_audio_buf = NULL; }
    if (cpu)           { heap_caps_free(cpu);           cpu = NULL; }
    if (lcd)           { heap_caps_free(lcd);           lcd = NULL; }
    if (scan)          { heap_caps_free(scan);          scan = NULL; }
    if (gbc_filebuf)   { heap_caps_free(gbc_filebuf);   gbc_filebuf = NULL; }
    if (bgdup)         { heap_caps_free(bgdup);         bgdup = NULL; }
    if (displayBuffer[0]) { heap_caps_free(displayBuffer[0]); displayBuffer[0] = NULL; }
    if (displayBuffer[1]) { heap_caps_free(displayBuffer[1]); displayBuffer[1] = NULL; }
    if (s_rom_owned && s_rom_data) {
        heap_caps_free(s_rom_data);
    }
    s_rom_data = NULL;
    s_rom_size = 0;
    s_rom_owned = false;
    s_core_mem_allocated = false;
    ESP_LOGI(TAG, "esp-box-emu gnuboy unloaded");
}

/* 从内存启动 (GB 兼容层也会调用) */
esp_err_t gbc_emu_start_data(uint8_t *data, size_t size, bool owned)
{
    if (!data || size < 0x150) return ESP_ERR_INVALID_ARG;
    if (s_task_running) return ESP_ERR_INVALID_STATE;

    esp_err_t r = gbc_emu_background_init();
    if (r != ESP_OK) return r;

    /* 预清理上一局残留 */
    memset(displayBuffer[0], 0, GBC_EMU_SCREEN_WIDTH * GBC_EMU_SCREEN_HEIGHT * 2);
    memset(displayBuffer[1], 0, GBC_EMU_SCREEN_WIDTH * GBC_EMU_SCREEN_HEIGHT * 2);
    pcm.pos = 0;
    frame = 0;

    sound_reset();
    loader_init(data, size);
    emu_reset();
    gbc_emu_load_sram();

    /* 清屏: 避免菜单/上一局的残留叠在游戏画面上 (花屏) */
    board_rlcd_clear(BOARD_RLCD_COLOR_WHITE);

    s_rom_data = data;
    s_rom_size = size;
    s_rom_owned = owned;

    gbc_emu_instance_t *inst = heap_caps_calloc(1, sizeof(*inst), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!inst) return ESP_ERR_NO_MEM;
    s_instance = inst;

    board_rlcd_video_task_start();
    /* GB/GBC: 30Hz 刷新 (转换 + SPI 需在 33ms 内完成, 留足余量) */
    board_rlcd_video_task_set_interval_us(33000);
    TaskHandle_t task = xTaskCreateStaticPinnedToCore(gbc_emu_task, "gbc_emu",
                                                      GBC_EMU_TASK_STACK_SIZE / sizeof(StackType_t),
                                                      NULL, GBC_EMU_TASK_PRIORITY,
                                                      s_gbc_task_stack, &s_gbc_task_tcb,
                                                      GBC_EMU_TASK_CORE);
    if (task == NULL) {
        board_rlcd_video_task_stop();
        s_instance = NULL;
        heap_caps_free(inst);
        return ESP_ERR_NO_MEM;
    }
    s_task_running = true;
    return ESP_OK;
}

esp_err_t gbc_emu_start(const char *path)
{
    if (!path) return ESP_ERR_INVALID_ARG;

    gbc_make_save_path(path);

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "打开失败: %s", path);
        return ESP_FAIL;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 8 * 1024 * 1024) {
        ESP_LOGE(TAG, "ROM 大小异常: %ld", sz);
        fclose(f);
        return ESP_FAIL;
    }

    uint8_t *data = heap_caps_malloc((size_t)sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!data) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    size_t rd = fread(data, 1, (size_t)sz, f);
    fclose(f);
    if (rd != (size_t)sz) {
        heap_caps_free(data);
        ESP_LOGE(TAG, "读取不完整: %u/%ld", (unsigned)rd, sz);
        return ESP_FAIL;
    }
    if (s_progress_cb) s_progress_cb(100);
    ESP_LOGI(TAG, "启动 GB/GBC: %s (%ld bytes)", path, sz);
    return gbc_emu_start_data(data, (size_t)sz, true);
}

esp_err_t gbc_emu_stop(void)
{
    if (s_instance) s_instance->stop_requested = true;
    /* V1.0.53: 同步等待任务完全退出 (含视频刷新任务停止),
     * 避免菜单重绘与游戏刷新并发操作 SPI/帧缓冲导致崩溃. */
    gbc_emu_wait_stopped();
    return ESP_OK;
}

void gbc_emu_wait_stopped(void)
{
    for (int i = 0; i < 50 && s_task_running; i++) vTaskDelay(pdMS_TO_TICKS(2));
}

void gbc_emu_pause(void)
{
    if (s_instance) {
        s_instance->pause_ack = false;
        s_instance->paused = true;
        for (uint16_t i = 0; i < 100 && s_instance && !s_instance->pause_ack; i++)
            vTaskDelay(pdMS_TO_TICKS(2));
    }
}

void gbc_emu_resume(void)
{
    if (s_instance) {
        s_instance->paused = false;
        s_instance->pause_ack = false;
    }
}

void gbc_emu_set_fullscreen(int mode)
{
    if (mode < 0) mode = 0;
    if (mode > 2) mode = 2;
    s_display_mode = mode;
}
