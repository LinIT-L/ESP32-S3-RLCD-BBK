/**
 * @file arduboy_avr.c
 * @brief Arduboy AVR 模拟核心适配层 (based on simavr ATmega32u4)
 *
 * 精简自 UVE5 对讲机项目的 arduboy_avr.cpp:
 *   - 仅保留模拟核心 (simavr + ssd1306 虚拟 OLED)
 *   - 去掉 FX 闪存 / OpenCV / 内置 ROM / 对讲机耦合
 *   - 游戏从 TF 卡 AB 目录下的 .hex 文件加载
 *   - 按需加载/卸载: 进入菜单懒加载, 返回主菜单释放内存
 *   - 直接渲染 SSD1306 显存到 ST7305 反射屏 (无虚拟硬件)
 */
#include "arduboy_avr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_cpu.h"

#include "sim_avr.h"
#include "sim_hex.h"
#include "sim_time.h"
#include "sim_cycle_timers.h"
#include "avr_ioport.h"
#include "avr_spi.h"
#include "avr_timer.h"
#include "ssd1306_virt.h"
#include "audio_player.h"
#include "virtual_keys.h"

#define AB_AVR_W   128
#define AB_AVR_H   64

/* 模拟速度倍率: 缩短游戏定时器周期来"加速".
 * ⚠️ 诊断阶段先关闭 (=1): ×5 是强制加速 hack, 会掩盖真实吞吐瓶颈,
 *    且只缩短定时器周期, 对忙等逻辑无效, 导致"有的界面快有的界面慢".
 *    真正要解决的是提升模拟吞吐 / 修正定时器推进逻辑. */
#ifndef AB_AVR_SPEED_FACTOR
#define AB_AVR_SPEED_FACTOR 1
#endif

/* V1.0.53: 音频恢复启用. Timer1 OC1A 翻转 hook 已有观察者守卫 (无观察者时
 * 跳过引脚链), 音频喂送走 audio_player, 不阻塞模拟. */
#ifndef AB_AVR_ENABLE_AUDIO
#define AB_AVR_ENABLE_AUDIO 1
#endif

/* 串口调试: 用 kconfig/宏控制日志详细度.
 * 0=仅关键事件  1=含渲染/频率  2=含 SPI/按键细节
 * ⚠️ 保持默认 0: 每帧/每 40ms 的高频日志会阻塞串口, 严重拖慢模拟吞吐
 *    (对比 UVE5 在 ESP32 上日志为空实现). 需要调试时临时改大. */
#ifndef AB_AVR_DEBUG
#define AB_AVR_DEBUG 0
#endif

#define AB_LOG(level, ...) do { \
    if (AB_AVR_DEBUG >= (level)) ESP_LOGI(TAG, __VA_ARGS__); \
} while (0)

static const char *TAG = "arduboy_avr";

/* ============ simavr 实例状态 ============ */
static avr_t        *s_avr = NULL;
static ssd1306_t     s_disp;
static avr_irq_t    *s_btn_b4 = NULL;   /* B 按键 PORTB4 */
static avr_irq_t    *s_btn_e6 = NULL;   /* A 按键 PORTE6 */
static avr_irq_t    *s_btn_f4 = NULL;   /* 下  PORTF4 */
static avr_irq_t    *s_btn_f5 = NULL;   /* 左  PORTF5 */
static avr_irq_t    *s_btn_f6 = NULL;   /* 右  PORTF6 */
static avr_irq_t    *s_btn_f7 = NULL;   /* 上  PORTF7 */
static bool          s_initialized = false;

/* --- Arduino Arduboy 扬声器: Timer1 OC1A (COMPA) 输出 --- */
static avr_irq_t    *s_aud_oc1a = NULL;
static volatile uint32_t s_audio_toggles = 0;   /* 当前窗口内 OC1A 翻转次数 */
static bool          s_audio_enabled = AB_AVR_ENABLE_AUDIO;

/* ============ 运行状态 ============ */
static st7305_handle_t *s_lcd = NULL;
static TaskHandle_t  s_task = NULL;
static volatile bool s_stop_requested = false;
static volatile bool s_paused = false;
static volatile bool s_pause_ack = false;
static int           s_display_mode = 1;
static uint8_t       s_joypad = 0xFF;   /* 全松开 */
static uint32_t      s_last_render_ms = 0;

/* 渲染缓冲: 全屏按 2:1 原比例最大拉伸 = 400x200 1bpp (10KB) */
#define AB_AVR_FULL_W  400
#define AB_AVR_FULL_H  300
/* 放 PSRAM, 不占内部 SRAM (内部留给模拟核心/栈) */
EXT_RAM_BSS_ATTR static uint8_t s_frame[AB_AVR_FULL_W * AB_AVR_FULL_H / 8];

/* ============ 内部函数 ============ */
static void arduboy_avr_sleep(avr_t *avr, avr_cycle_count_t how_long)
{
    (void)avr;
    (void)how_long;
}

/* 更新按键: GB joypad 掩码 (低电平有效) -> Arduboy 引脚 (低电平按下) */
static void arduboy_avr_update_buttons(void)
{
    if (!s_avr) return;
    /* 各引脚 IRR 值: 1=松开, 0=按下 (与 joypad 低电平语义一致) */
    if (s_btn_b4) avr_raise_irq(s_btn_b4, (s_joypad >> 1) & 1);   /* B */
    if (s_btn_e6) avr_raise_irq(s_btn_e6, (s_joypad >> 0) & 1);   /* A */
    if (s_btn_f4) avr_raise_irq(s_btn_f4, (s_joypad >> 7) & 1);   /* 下 */
    if (s_btn_f5) avr_raise_irq(s_btn_f5, (s_joypad >> 5) & 1);   /* 左 */
    if (s_btn_f6) avr_raise_irq(s_btn_f6, (s_joypad >> 4) & 1);   /* 右 */
    if (s_btn_f7) avr_raise_irq(s_btn_f7, (s_joypad >> 6) & 1);   /* 上 */
}

/* Timer1 OC1A (COMPA) 翻转 → 计数以测定音符频率 */
static void arduboy_avr_audio_pb5_hook(avr_irq_t *irq, uint32_t value, void *param)
{
    (void)irq; (void)value; (void)param;
    s_audio_toggles++;
}

/* 根据窗口内 PB5 翻转次数测出频率, 合成方波立体声 PCM 喂给 audio_player.
 * 每 ~40ms 调用一次. 频率 = 每秒翻转数 / 2 (一个完整周期 = 两次翻转). */
#define AB_AUDIO_SAMPLE_RATE 24000
#define AB_AUDIO_WINDOW_MS   40

static void arduboy_avr_feed_audio(void)
{
    if (!s_audio_enabled) {
        s_audio_toggles = 0;
        return;
    }
    const uint32_t toggles = s_audio_toggles;
    s_audio_toggles = 0;

    /* 窗口内翻转太少 (<10) 视为无声音 */
    /* 合成缓冲放 PSRAM, 不占内部 RAM (AB 音频当前默认关闭, 保留路径) */
    EXT_RAM_BSS_ATTR static int16_t pcm[AB_AUDIO_SAMPLE_RATE * AB_AUDIO_WINDOW_MS / 1000 * 2];
    const int frames = AB_AUDIO_SAMPLE_RATE * AB_AUDIO_WINDOW_MS / 1000;
    if (toggles < 10) {
        /* 静音窗口 */
        memset(pcm, 0, (size_t)frames * 2 * sizeof(int16_t));
    } else {
        const uint32_t toggles_per_sec = (toggles * 1000u) / AB_AUDIO_WINDOW_MS; /* 每秒翻转数 */
        /* 速度倍率会等比例放大翻转频率, 除以倍率还原真实音符频率 */
        const uint32_t toggles_div = toggles_per_sec / AB_AVR_SPEED_FACTOR;
        const uint32_t freq = toggles_div / 2u; /* 一个完整周期 = 两次翻转 */
        const uint32_t half_period = (uint32_t)((AB_AUDIO_SAMPLE_RATE / 2u) / (freq ? freq : 1u));
        const int16_t amp = 3200;   /* 适中音量, 避免太吵 */
        int phase = 0;
        for (int i = 0; i < frames; i++) {
            int16_t v = (((phase / (half_period ? half_period : 1u)) & 1u) ? amp : -amp);
            pcm[i * 2] = v;       /* L */
            pcm[i * 2 + 1] = v;   /* R */
            phase++;
        }
    }
    audio_player_feed_pcm(pcm, (size_t)frames, AB_AUDIO_SAMPLE_RATE);
}

/* SSD1306 页面显存 -> ST7305 行主序 1bpp, 支持翻转/反色/缩放 */
static void arduboy_avr_render(void)
{
    if (!s_lcd || !s_avr) return;

    const bool flip_x = ssd1306_get_flag(&s_disp, SSD1306_FLAG_SEGMENT_REMAP_0);
    const bool flip_y = ssd1306_get_flag(&s_disp, SSD1306_FLAG_COM_SCAN_NORMAL);
    const bool invert = ssd1306_get_flag(&s_disp, SSD1306_FLAG_DISPLAY_INVERTED);

    int W, H;
    if (s_display_mode == 0)      { W = AB_AVR_W;      H = AB_AVR_H; }
    else if (s_display_mode == 2) { W = AB_AVR_FULL_W; H = AB_AVR_FULL_H; }
    else                          { W = AB_AVR_FULL_W; H = 200; }
    const int row_bytes = W >> 3;

    if (W > AB_AVR_FULL_W || H > AB_AVR_FULL_H) return;   /* 超出静态缓冲 */

    /* 最近邻采样表: 按原比例把 128x64 映射到目标尺寸 (宽高独立),
     * 避免每像素除法, 全屏 400x200 也能保持 ~2ms 内完成. */
    static uint8_t s_sx_map[AB_AVR_FULL_W];
    static uint8_t s_sy_map[AB_AVR_FULL_H];
    static int s_map_w = -1, s_map_h = -1;
    if (s_map_w != W || s_map_h != H) {
        for (int col = 0; col < W; col++) {
            int sx = (col * AB_AVR_W + (W >> 1)) / W;
            if (sx >= AB_AVR_W) sx = AB_AVR_W - 1;   /* 钳制, 防止最后一列越界 */
            s_sx_map[col] = (uint8_t)sx;
        }
        for (int row = 0; row < H; row++) {
            int sy = (row * AB_AVR_H + (H >> 1)) / H;
            if (sy >= AB_AVR_H) sy = AB_AVR_H - 1;   /* 钳制, 防止最后一行越界 */
            s_sy_map[row] = (uint8_t)sy;
        }
        s_map_w = W;
        s_map_h = H;
    }

    memset(s_frame, 0, sizeof(s_frame));

    /* 整屏清黑, 再写白像素: 还原真实 Arduboy 的白字黑底.
     * 之前整屏清白 + blit_1bit(位=1=黑) 导致整幅画面反色 (白底黑画),
     * 且窗口外残留白色, 这是从项目开始就存在的极性 bug. */
    st7305_clear(s_lcd, ST7305_COLOR_BLACK);

    const uint8_t (*vram)[SSD1306_VIRT_COLUMNS] = s_disp.vram;
    for (int row = 0; row < H; row++) {
        int sy = s_sy_map[row];
        if (flip_y) sy = (AB_AVR_H - 1) - sy;
        const int page = sy >> 3;
        const int bit  = sy & 7;
        for (int col = 0; col < W; col++) {
            int sx = s_sx_map[col];
            if (flip_x) sx = (AB_AVR_W - 1) - sx;
            int pix = (vram[page][sx] >> bit) & 1;
            if (invert) pix = !pix;
            if (pix) {
                s_frame[row * row_bytes + (col >> 3)] |= (uint8_t)(0x80 >> (col & 7));
            }
        }
    }

    const int x_off = (ST7305_WIDTH - W) / 2;
    const int y_off = (ST7305_HEIGHT - H) / 2;
    st7305_set_window(s_lcd, x_off, y_off, W, H);
    st7305_blit_1bit_white(s_lcd, x_off, y_off, W, H, s_frame);
    /* Arduboy 直接 st7305_flush, 绕过 board_shim 视频任务 (虚拟按键只在视频任务里画),
     * 故在此补画屏幕虚拟按键, 否则 Arduboy 游戏内永远看不到虚拟按键. */
    virtual_keys_draw(s_lcd);
    st7305_flush(s_lcd);
    ssd1306_set_flag(&s_disp, SSD1306_FLAG_DIRTY, 0);
}

static void arduboy_avr_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "arduboy AVR task started");

    /* 本任务几乎一直占用 core1, 需喂任务看门狗以免饿死 IDLE 触发复位 */
    esp_task_wdt_add(NULL);

    uint64_t tune_cycles = 0;
    int64_t  tune_start_us = 0;
    uint32_t s_last_audio_ms = 0;

    /* 让出控制但不休眠, 保持模拟吞吐 (参考 UVE5) */
    uint32_t s_last_idle_yield_ms = 0;

    /* 低频诊断 (每 2s): 观察实际吞吐 / CPU 状态 / 渲染, 开销可忽略 */
    uint32_t s_last_diag_ms = 0;
    uint64_t s_last_diag_cycle = 0;
    uint64_t s_diag_runs = 0;   /* 窗口内 avr_run() 调用次数 */
    uint64_t s_diag_host_cycles = 0;  /* 窗口内 avr_run() 主机周期 */

    while (1) {
        if (s_stop_requested) break;

        if (s_paused) {
            s_pause_ack = true;
            vTaskDelay(pdMS_TO_TICKS(16));
            continue;
        }

        /* 诊断: 每 2s 打印一次关键指标 */
        {
            const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
            if ((uint32_t)(now_ms - s_last_diag_ms) >= 2000 && s_avr) {
                s_last_diag_ms = now_ms;
                const uint64_t cyc = s_avr->cycle;
                const uint64_t dcyc = (cyc >= s_last_diag_cycle) ? (cyc - s_last_diag_cycle) : 0;
                s_last_diag_cycle = cyc;
                const uint64_t runs = s_diag_runs;
                s_diag_runs = 0;
                const uint64_t host = s_diag_host_cycles;
                s_diag_host_cycles = 0;
                ESP_LOGI(TAG, "stat state=%d pc=0x%04X cps=%llu freq=%u factor=%u dirty=%d toggles=%u",
                         (int)s_avr->state, (unsigned)s_avr->pc,
                         (unsigned long long)(dcyc / 2),
                         (unsigned)s_avr->frequency,
                         (unsigned)simavr_speed_factor,
                         ssd1306_get_flag(&s_disp, SSD1306_FLAG_DIRTY),
                         (unsigned)s_audio_toggles);
                ESP_LOGI(TAG, "diag avg_cyc/run=%llu runs=%llu host_cyc/run=%llu tproc_cyc=%llu tproc_call=%llu tproc_share=%llu%%",
                         runs ? (unsigned long long)(dcyc / runs) : 0ull,
                         (unsigned long long)runs,
                         runs ? (unsigned long long)(host / runs) : 0ull,
                         (unsigned long long)sim_prof_timer_cycles(),
                         (unsigned long long)sim_prof_timer_calls(),
                         host ? (unsigned long long)(sim_prof_timer_cycles() * 100 / host) : 0ull);
                sim_prof_timer_reset();
                avr_cycle_timer_slot_p tp = s_avr->cycle_timers.timer;
                for (int ti = 0; ti < 8 && tp; ti++, tp = tp->next) {
                    ESP_LOGI(TAG, "tmr[%d] due_in=%lld cb=%p",
                             ti, (long long)(tp->when - s_avr->cycle), (void *)tp->timer);
                }
            }
        }

        arduboy_avr_update_buttons();

        /* 按墙钟切分仿真, 避免长时间占用 CPU / 触发 WDT.
         * ⚠️ 内层循环不要每轮都调 esp_timer_get_time() (开销大, 会拖慢 AVR 吞吐),
         * 改为每 256 次迭代检测一次墙钟截止 (~与 UVE5 参考一致). */
        const int64_t deadline = esp_timer_get_time() + 8000; /* ~8ms */
        const avr_cycle_count_t cycle_start = s_avr ? s_avr->cycle : 0;
        uint32_t iter = 0;
        while ((s_avr->state == cpu_Running || s_avr->state == cpu_Sleeping) &&
               !s_stop_requested) {
            const uint32_t h0 = esp_cpu_get_cycle_count();
            avr_run(s_avr);
            s_diag_host_cycles += (uint32_t)(esp_cpu_get_cycle_count() - h0);
            s_diag_runs++;
            if (((++iter) & 0x3Fu) == 0) {
                if (esp_timer_get_time() >= deadline) break;
            }
        }
        if (s_stop_requested) break;

        if (s_avr) {
            const avr_cycle_count_t cycle_end = s_avr->cycle;
            if (cycle_end >= cycle_start) tune_cycles += (uint64_t)(cycle_end - cycle_start);

            /* 每秒根据实际吞吐自动调优模拟频率:
             * ESP32 无法实时模拟 16MHz AVR, 参考项目将频率夹在 1~2MHz,
             * 让游戏按"能跑多快跑多快"的方式推进, 避免像卡死一样极慢. */
            const int64_t now_us = esp_timer_get_time();
            if (tune_start_us == 0) tune_start_us = now_us;
            if (now_us - tune_start_us >= 1000000) {
                const uint64_t cps = (tune_cycles * 1000000ULL) / (uint64_t)(now_us - tune_start_us);
                uint32_t hz = (uint32_t)cps;
                if (hz < 800000) hz = 800000;
                if (hz > 16000000) hz = 16000000;   /* 上限放开到真实 16MHz */
                if (s_avr->frequency != hz) {
                    AB_LOG(1, "auto-tune: freq %u -> %u Hz (cps=%llu)", (unsigned)s_avr->frequency, hz, (unsigned long long)cps);
                    s_avr->frequency = hz;
                }
                tune_start_us = now_us;
                tune_cycles = 0;
            }
        }

        /* CPU 停机/崩溃: 结束运行 */
        if (s_avr->state == cpu_Done || s_avr->state == cpu_Crashed) {
            ESP_LOGW(TAG, "AVR halted (state=%d), stopping", (int)s_avr->state);
            break;
        }

        /* 有脏帧且到刷新间隔 -> 渲染 (~16 FPS, 对齐屏幕标准 SCREEN_FRAME_MS=62).
         * 渲染含全屏 SPI flush (实测 12-15ms), 会阻塞模拟核心所在 core1, 故降低刷新率
         * 以优先保证模拟吞吐 ("游戏速度 > 画面流畅", 与 UVE5 参考一致). */
        if (ssd1306_get_flag(&s_disp, SSD1306_FLAG_DIRTY)) {
            const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
            if ((uint32_t)(now_ms - s_last_render_ms) >= 62) {
                s_last_render_ms = now_ms;
                arduboy_avr_render();
            }
        }

        /* 每 ~40ms 合成并馈送一次音频 */
        {
            const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
            if ((uint32_t)(now_ms - s_last_audio_ms) >= AB_AUDIO_WINDOW_MS) {
                s_last_audio_ms = now_ms;
                arduboy_avr_feed_audio();
            }
        }

        /* 尽量不休眠以保持模拟吞吐; 每 50ms 让出一个 tick 防饿死其他任务.
         * ⚠️ 之前每轮 vTaskDelay(pdMS_TO_TICKS(1)) 在 100Hz tick 下实际睡 10ms,
         *    导致模拟器只能工作 ~44% 时间, 是游戏极慢的主因. */
        {
            const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
            if ((uint32_t)(now_ms - s_last_idle_yield_ms) >= 50) {
                s_last_idle_yield_ms = now_ms;
                esp_task_wdt_reset();   /* 喂看门狗 */
                vTaskDelay(pdMS_TO_TICKS(1));
            } else {
                taskYIELD();
            }
        }
    }

    ESP_LOGW(TAG, "arduboy AVR task exiting (state=%d)", s_avr ? (int)s_avr->state : -1);
    /* 注销任务看门狗: 任务退出后若不注销, TWDT 会每 5s 持续触发 (日志刷屏),
     * 且残留订阅会让后续误判任务仍在运行. */
    esp_task_wdt_delete(NULL);   /* NULL = 注销当前任务 */
    s_task = NULL;
    vTaskDelete(NULL);
}

/* 解析 Intel HEX 文本 (去掉 ':' 行) 并载入 flash */
static bool arduboy_avr_load_hex(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return false;

    char line_buf[560];
    uint8_t bline[272];
    uint32_t segment = 0;
    bool ok = true;
    uint32_t data_bytes = 0;
    int line_count = 0;

    while (fgets(line_buf, sizeof(line_buf), f)) {
        /* 去掉行尾 \r \n */
        char *end = line_buf + strlen(line_buf);
        while (end > line_buf && (end[-1] == '\n' || end[-1] == '\r')) *--end = '\0';
        const char *line = line_buf;
        if (*line != ':') continue;
        line++;   /* 跳过 ':' */
        line_count++;

        const int hexlen = read_hex_string(line, bline, sizeof(bline));
        if (hexlen < 5) { ok = false; break; }

        uint8_t chk = 0;
        for (int i = 0; i < hexlen - 1; i++) chk = (uint8_t)(chk + bline[i]);
        chk = (uint8_t)(0x100 - chk);
        if (chk != bline[hexlen - 1]) { ok = false; break; }

        const uint8_t count = bline[0];
        const uint16_t addr = (uint16_t)((bline[1] << 8) | bline[2]);
        const uint8_t type = bline[3];
        if (hexlen < (int)(count + 5)) { ok = false; break; }

        switch (type) {
        case 0x00:
            if (count > 0) {
                avr_loadcode(s_avr, bline + 4, count, (avr_flashaddr_t)(segment | addr));
                data_bytes += count;
            }
            break;
        case 0x01:   /* EOF */
            goto done;
        case 0x02:   /* 扩展段地址 */
            if (count >= 2) segment = ((uint32_t)(bline[4] << 8 | bline[5])) << 4;
            break;
        case 0x04:   /* 扩展线性地址 */
            if (count >= 2) segment = ((uint32_t)(bline[4] << 8 | bline[5])) << 16;
            break;
        default:
            break;
        }
    }
done:
    fclose(f);
    AB_LOG(1, "hex 解析: %s lines=%d data_bytes=%u ok=%d", path, line_count, (unsigned)data_bytes, ok);
    return ok;
}

/* 懒初始化 simavr CPU + 虚拟 OLED + 按键 IRQ */
static esp_err_t arduboy_avr_init(void)
{
    if (s_initialized) return ESP_OK;

    s_avr = avr_make_mcu_by_name("atmega32u4");
    if (!s_avr) return ESP_FAIL;
    if (avr_init(s_avr) != 0) {
        free(s_avr);
        s_avr = NULL;
        return ESP_FAIL;
    }

    s_avr->frequency = 16000000;
    s_avr->sleep = arduboy_avr_sleep;
    s_avr->log = LOG_ERROR;

    /* 全局速度倍率: 缩短定时器周期, 让游戏在 ESP32 有限 AVR 吞吐下跑得更快 */
    avr_timer_set_speed_factor(AB_AVR_SPEED_FACTOR);
    AB_LOG(1, "speed factor = %u", (unsigned)AB_AVR_SPEED_FACTOR);

    ssd1306_init(s_avr, &s_disp, SSD1306_VIRT_COLUMNS, SSD1306_VIRT_PAGES * 8);
    ssd1306_wiring_t wiring = {
        .chip_select      = { 'D', 6 },
        .data_instruction = { 'D', 4 },
        .reset            = { 'D', 7 },
    };
    ssd1306_connect(&s_disp, &wiring);

    s_btn_b4 = avr_io_getirq(s_avr, AVR_IOCTL_IOPORT_GETIRQ('B'), IOPORT_IRQ_PIN4);
    s_btn_e6 = avr_io_getirq(s_avr, AVR_IOCTL_IOPORT_GETIRQ('E'), IOPORT_IRQ_PIN6);
    s_btn_f4 = avr_io_getirq(s_avr, AVR_IOCTL_IOPORT_GETIRQ('F'), IOPORT_IRQ_PIN4);
    s_btn_f5 = avr_io_getirq(s_avr, AVR_IOCTL_IOPORT_GETIRQ('F'), IOPORT_IRQ_PIN5);
    s_btn_f6 = avr_io_getirq(s_avr, AVR_IOCTL_IOPORT_GETIRQ('F'), IOPORT_IRQ_PIN6);
    s_btn_f7 = avr_io_getirq(s_avr, AVR_IOCTL_IOPORT_GETIRQ('F'), IOPORT_IRQ_PIN7);
    if (!s_btn_b4 || !s_btn_e6 || !s_btn_f4 || !s_btn_f5 || !s_btn_f6 || !s_btn_f7) {
        avr_terminate(s_avr);
        free(s_avr);
        s_avr = NULL;
        return ESP_FAIL;
    }

    /* 音频: 监听 Timer1 OC1A (COMPA) 输出 IRQ 的翻转.
     * 诊断阶段关闭 (AB_AVR_ENABLE_AUDIO=0): 不注册 hook, 省掉每次翻转的
     * 回调开销, 排除音频对吞吐的影响. */
#if AB_AVR_ENABLE_AUDIO
    s_aud_oc1a = avr_io_getirq(s_avr, AVR_IOCTL_TIMER_GETIRQ('1'),
                               TIMER_IRQ_OUT_COMP + AVR_TIMER_COMPA);
    if (s_aud_oc1a) {
        avr_irq_register_notify(s_aud_oc1a, arduboy_avr_audio_pb5_hook, NULL);
        AB_LOG(1, "audio: Timer1 OC1A hook attached");
    } else {
        ESP_LOGW(TAG, "audio: Timer1 OC1A IRQ 不可用, 声音关闭");
        s_audio_enabled = false;
    }
#endif
    s_audio_toggles = 0;

    s_joypad = 0xFF;
    arduboy_avr_update_buttons();
    avr_reset(s_avr);
    /* 批处理预算: 太大会让单次 avr_run 占用数百 ms (看门狗触发/游戏假死),
     * 太小则批间摊销 (cycle_timer_process + 中断服务) 拖低吞吐.
     * 16000 周期 ≈ 1-4ms/批, 既摊销开销又保证外层循环及时让出/喂狗. */
    s_avr->run_cycle_limit = 16000;
    ssd1306_set_flag(&s_disp, SSD1306_FLAG_DIRTY, 1);

    s_initialized = true;
    ESP_LOGI(TAG, "simavr ATmega32u4 core initialized");
    return ESP_OK;
}

/* ============ 公共接口 ============ */

esp_err_t arduboy_avr_background_init(st7305_handle_t *lcd)
{
    if (lcd) s_lcd = lcd;
    return arduboy_avr_init();
}

void arduboy_avr_unload(void)
{
    if (s_task) {
        s_stop_requested = true;
        for (int i = 0; i < 50 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(2));
    }
    s_stop_requested = false;
    s_paused = false;

    if (s_avr) {
        avr_terminate(s_avr);
        free(s_avr);
        s_avr = NULL;
    }
    memset(&s_disp, 0, sizeof(s_disp));
    s_initialized = false;
    ESP_LOGI(TAG, "arduboy AVR unloaded (memory released)");
}

esp_err_t arduboy_avr_start(const char *path)
{
    if (!s_initialized) {
        esp_err_t r = arduboy_avr_init();
        if (r != ESP_OK) return r;
    }
    if (!path) return ESP_ERR_INVALID_ARG;

    s_stop_requested = false;
    s_paused = false;

    if (!arduboy_avr_load_hex(path)) {
        ESP_LOGE(TAG, "hex 加载失败: %s", path);
        return ESP_FAIL;
    }

    avr_reset(s_avr);
    s_avr->run_cycle_limit = 16000;     /* 大批量执行, 摊销定时器处理开销 */
    s_joypad = 0xFF;
    arduboy_avr_update_buttons();
    ssd1306_set_flag(&s_disp, SSD1306_FLAG_DIRTY, 1);
    s_last_render_ms = 0;

    if (!s_task) {
        BaseType_t r = xTaskCreatePinnedToCore(arduboy_avr_task, "arduboy_avr",
                                               16384, NULL, 3, &s_task, 1);
        if (r != pdPASS) {
            s_task = NULL;
            ESP_LOGE(TAG, "无法创建模拟任务");
            return ESP_ERR_NO_MEM;
        }
    }
    ESP_LOGI(TAG, "游戏启动: %s (freq=%u, disp dirty=%d)", path,
             (unsigned)s_avr->frequency, ssd1306_get_flag(&s_disp, SSD1306_FLAG_DIRTY));
    return ESP_OK;
}

esp_err_t arduboy_avr_stop(void)
{
    if (!s_task) return ESP_OK;
    s_stop_requested = true;
    for (int i = 0; i < 50 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(2));
    s_stop_requested = false;
    return ESP_OK;
}

void arduboy_avr_set_joypad(uint8_t joypad)
{
    s_joypad = joypad;
}

void arduboy_avr_pause(void)
{
    s_pause_ack = false;
    s_paused = true;
    for (uint16_t i = 0; i < 100 && s_task && !s_pause_ack; i++) vTaskDelay(pdMS_TO_TICKS(2));
}

void arduboy_avr_resume(void)
{
    s_paused = false;
    s_pause_ack = false;
}

void arduboy_avr_set_fullscreen(int mode)
{
    if (mode < 0) mode = 0;
    if (mode > 2) mode = 2;
    s_display_mode = mode;
}
