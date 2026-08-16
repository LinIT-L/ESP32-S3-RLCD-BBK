/* MP3 音频播放器
 * - I2S 输出到 ES8311 codec
 * - minimp3 解码 MP3 文件
 * - 后台 FreeRTOS 任务持续解码播放
 *
 * 硬件引脚 (微雪 ESP32-S3-RLCD-4.2):
 *   I2S_BCLK = GPIO9, I2S_WS = GPIO45, I2S_DOUT = GPIO8
 *   I2S_MCLK = GPIO16, PA_ENABLE = GPIO46
 *   I2C_SDA = GPIO13, I2C_SCL = GPIO14
 */

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3

#include "audio_player.h"
#include "es8311.h"
#include "es7210.h"
#include "minimp3.h"
#include "driver/i2s_std.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

static const char *TAG = "AUDIO";

/* 硬件引脚 */
#define I2S_BCLK    GPIO_NUM_9
#define I2S_WS      GPIO_NUM_45
#define I2S_DOUT    GPIO_NUM_8
#define I2S_DIN     GPIO_NUM_10   /* 双麦克风阵列 (ES7210 ADC) */
#define I2S_MCLK    GPIO_NUM_16
#define PA_PIN      GPIO_NUM_46
#define I2C_SDA     13
#define I2C_SCL     14

/* 解码缓冲区 (PSRAM) */
#define MP3_FRAME_BUF_SIZE  4096        /* MP3 帧数据读取缓冲 */

/* 状态 */
static audio_state_t s_state = AUDIO_STATE_IDLE;
static int  s_volume = 100;             /* 默认音量 100%, ES8311 映射到 0xC0(0dB) 不削波 */
static bool s_muted = false;            /* V1.0.68: 全局强制静音 */
static char s_filename[256] = "";
static FILE *s_mp3_file = NULL;
static TaskHandle_t s_task_handle = NULL;
static SemaphoreHandle_t s_mutex = NULL;
static bool s_hw_initialized = false;
static bool s_stop_requested = false;
static int  s_progress = 0;
static int64_t s_file_size = 0;
static int64_t s_elapsed_samples = 0;
static int64_t s_total_samples = 0;
static int  s_mp3_sample_rate = 0;

/* minimp3 解码器
 * V1.0.44: 移到 PSRAM, 释放 6.6KB 内部 DRAM.
 * MP3 解码非实时高频访问, PSRAM 带宽足够. */
EXT_RAM_BSS_ATTR static mp3dec_t s_mp3dec;
static uint8_t *s_mp3_buf = NULL;       /* PSRAM 分配 */
static int  s_mp3_buf_pos = 0;    /* 已填充位置 */
static int  s_mp3_buf_len = 0;    /* 有效数据长度 */

/* I2S 通道句柄 */
static i2s_chan_handle_t s_tx_chan = NULL;
static i2s_chan_handle_t s_rx_chan = NULL;
static bool s_mic_active = false;

/* === 环形缓冲区 (游戏模拟器音频输出) === */
/* 64KB = 32768 个 int16 = 16384 帧, 约 682ms @24000Hz stereo.
 * 游戏音频是突发式生成(每帧末一次性喂入), 缓冲越大越能吸收负载抖动/欠载,
 * 避免 audio_out_task 掏空缓冲导致的"哒哒哒"小缝隙. */
#define PCM_RING_SIZE 32768
/* 首次播放前先让环形缓冲攒够 ~373ms 音频再开始掏空,
 * 吸收模拟器帧抖动, 避免声音一卡一卡 (生产/消耗速率几乎相等时尤其重要).
 * 20260812: 4096 帧在 GB/GBC 45fps 抖动下仍偶发欠载, 翻倍到 8192. */
#define PCM_RING_PRIME_FRAMES 8192
static int16_t *s_pcm_ring = NULL;
static volatile uint32_t s_ring_head = 0;  /* 生产者写入位置 */
static volatile uint32_t s_ring_tail = 0;  /* 消费者读取位置 */
static TaskHandle_t s_audio_out_task = NULL;
static volatile bool s_audio_out_running = false;
static volatile bool s_pcm_flush_requested = false;   /* 按键音效释放时立即断音 */
static uint32_t s_last_underrun_ms = 0;   /* 欠载日志: 上次欠载时刻 (区分偶发/持续) */

/* V1.0.53: PCM 输出任务栈改 PSRAM 静态, 不再占内部 RAM.
 * 6KB 在 44.1kHz 冷启动 (大帧数据 + 音量缩放 + I2S 驱动调用链) 仍会溢出,
 * 栈在 PSRAM, 直接给 12KB 余量. */
/* V1.0.55: 3072 字 (12KB) 在加入播放速率日志 (ESP_LOGI 格式化) 时爆栈,
 * 任务栈在 PSRAM, 直接给 24KB 余量. */
#define PCM_OUT_STACK_WORDS  6144   /* 24576 字节 */
EXT_RAM_BSS_ATTR static StackType_t s_pcm_out_stack[PCM_OUT_STACK_WORDS];
static StaticTask_t s_pcm_out_tcb;
/* V1.0.45: 共享采样率 — feed_pcm 设置, audio_out_task 读取.
 * 之前 audio_out_task 硬编码 44100 并每轮调用 i2s_init(44100),
 * 与 feed_pcm 的 i2s_init(24000) 冲突, 导致每帧都在重配 I2S+ES8311,
 * 音频持续卡顿. */
static volatile int s_desired_sample_rate = 44100;
static int s_current_i2s_rate = 0;   /* i2s_init_locked 维护的实际 I2S 采样率 */

/* V1.0.46: I2S/ES8311 硬件配置互斥锁.
 * feed_pcm (gb_emu_task, prio 4) 与 audio_out_task (prio 8) 都会调用 i2s_init:
 * audio_out_task 优先级更高, 可能在 feed_pcm 的 i2s_init 中途抢占
 * (disable → reconfig → enable 序列被打断), 两个任务并发操作同一 I2S 通道
 * 会损坏驱动状态 → 崩溃重启. 所有 I2S/ES8311 配置操作必须持有此锁.
 * 在 audio_player_init() 中创建, 创建前调用方需容忍 NULL (跳过加锁). */
static SemaphoreHandle_t s_i2s_mutex = NULL;

/* 功放控制 + I2S 初始化 (前向声明, 供 audio_out_task 调用) */
static void pa_enable(bool on);
static int i2s_init(int sample_rate);

/* CPU1 音频输出任务: 从环形缓冲读 PCM → I2S */
static void audio_out_task(void *arg) {
    (void)arg;
    int16_t local_buf[512];  /* 栈缓冲, 一次读取最多 256 帧 */
    bool primed = false;
    uint32_t underruns = 0;
    uint32_t iter = 0;
    uint32_t frames_total = 0;
    uint32_t last_frames = 0;
    uint32_t last_log_ms = 0;

    while (s_audio_out_running) {
        uint32_t head = s_ring_head;
        uint32_t tail = s_ring_tail;

        if (s_pcm_flush_requested) {
            s_pcm_flush_requested = false;
            s_ring_head = 0;
            s_ring_tail = 0;
            primed = true;   /* 清空后立即播放, 不等预填充 */
            head = 0;
            tail = 0;
        }

        /* 计算可读帧数 */
        uint32_t avail;
        if (head >= tail) {
            avail = (head - tail) / 2;  /* 每帧 2 个 sample */
        } else {
            avail = (PCM_RING_SIZE / 2 - tail + head) / 2;
        }

        if (avail == 0) {
            /* 环形缓冲短暂空: 项目 tick 为 1kHz, vTaskDelay(1)=1ms 且会让出 CPU.
             * 不能忙等 (优先级 8 会饿死同核 IDLE, 触发任务看门狗). */
            underruns++;
            uint32_t unow = esp_log_timestamp();
            if (unow - s_last_underrun_ms > 100) {
                /* 只记录"新一轮"欠载, 避免连续欠载刷屏;
                 * gap 大 = 偶发欠载, gap 小 = 持续欠载. */
                ESP_LOGW(TAG, "I2S underrun: 距上次欠载 %u ms (avail=0)",
                         (unsigned)(unow - s_last_underrun_ms));
            }
            s_last_underrun_ms = unow;
            vTaskDelay(1);
            continue;
        }
        if (!primed) {
            if (avail < PCM_RING_PRIME_FRAMES) {
                vTaskDelay(1);
                continue;
            }
            primed = true;
            ESP_LOGI(TAG, "PCM 环形缓冲已预填充 %u 帧, 开始播放", (unsigned)avail);
        }

        if (avail > 256) avail = 256;
        frames_total += avail;

        if ((++iter % 400) == 0) {
            uint32_t now_ms = esp_log_timestamp();
            uint32_t delta_ms = now_ms - last_log_ms;
            uint32_t play_hz = (delta_ms > 0) ? ((frames_total - last_frames) * 1000u / delta_ms) : 0;
            ESP_LOGI(TAG, "PCM ring: avail=%u underruns=%u want=%dHz i2s=%dHz play=%uHz",
                     (unsigned)avail, (unsigned)underruns,
                     (int)s_desired_sample_rate, audio_player_get_i2s_rate(),
                     (unsigned)play_hz);
            last_frames = frames_total;
            last_log_ms = now_ms;
            underruns = 0;
        }

        /* 从环形缓冲复制到本地缓冲 */
        for (uint32_t i = 0; i < avail * 2; i++) {
            local_buf[i] = s_pcm_ring[(tail + i) % PCM_RING_SIZE];
        }
        tail = (tail + avail * 2) % PCM_RING_SIZE;
        s_ring_tail = tail;

        /* 确保 I2S 初始化, 使用 feed_pcm 设置的采样率 (不再硬编码 44100) */
        if (!s_hw_initialized) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        if (i2s_init(s_desired_sample_rate) != 0) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        
        /* 应用音量 */
        float vol_scale = s_volume / 100.0f;
        for (uint32_t i = 0; i < avail * 2; i++) {
            local_buf[i] = (int16_t)(local_buf[i] * vol_scale);
        }
        
        size_t bytes = avail * 2 * sizeof(int16_t);
        size_t written = 0;
        i2s_channel_write(s_tx_chan, local_buf, bytes, &written, pdMS_TO_TICKS(20));
    }
    
    s_audio_out_task = NULL;
    vTaskDelete(NULL);
}

/* 启动音频输出任务 (游戏开始时调用) */
static void audio_out_start(void) {
    if (s_audio_out_running) return;
    if (!s_pcm_ring) {
        s_pcm_ring = (int16_t *)heap_caps_calloc(PCM_RING_SIZE, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_pcm_ring) {
            /* V1.0.46: 之前 calloc(PCM_RING_SIZE,1) 只分配 16KB=8192 个 int16,
             * 但代码按 16384 个元素索引 (head 最大 16382), 缓冲写满时越界 16KB
             * → 破坏相邻堆块 → 按键时 BT 栈 malloc 撞到损坏块崩溃. */
            s_pcm_ring = (int16_t *)calloc(PCM_RING_SIZE, sizeof(int16_t));
        }
        if (!s_pcm_ring) return;
    }
    s_ring_head = 0;
    s_ring_tail = 0;
    s_audio_out_running = true;
    
    /* 确保 ES8311 编解码器已启动并开启功放 (游戏音频路径此前缺失此步骤) */
    if (s_i2s_mutex) xSemaphoreTake(s_i2s_mutex, portMAX_DELAY);
    es8311_start();
    es8311_set_sample_rate(s_desired_sample_rate);
    pa_enable(true);
    if (s_i2s_mutex) xSemaphoreGive(s_i2s_mutex);
    
    /* V1.0.45: 栈从 4096 → 6144, 冷启动时大帧数据 + 音量缩放 + I2S 写入容易栈溢出 panic */
    s_audio_out_task = xTaskCreateStaticPinnedToCore(audio_out_task, "audio_out",
                                                     PCM_OUT_STACK_WORDS, NULL, 8,
                                                     s_pcm_out_stack, &s_pcm_out_tcb, 1);
    if (s_audio_out_task == NULL) {
        ESP_LOGE(TAG, "audio_out 静态任务创建失败!");
        s_audio_out_running = false;
    }
}

/* 停止音频输出任务 */
static void audio_out_stop(void) {
    if (!s_audio_out_running) return;
    s_audio_out_running = false;
    /* 等待任务退出 (最多 100ms) */
    for (int i = 0; i < 10 && s_audio_out_task; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    pa_enable(false);
}

/* ============ I2S 初始化 ============ */
/* 实际配置逻辑 (调用方必须已持有 s_i2s_mutex) */
static int i2s_init_locked(int sample_rate) {
    if (s_tx_chan) {
        /* I2S 已存在且采样率相同, 无需重配 */
        if (s_current_i2s_rate == sample_rate) {
            return 0;
        }
        /* 采样率不同: 先停用再重配时钟 */
        i2s_channel_disable(s_tx_chan);
        i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
        esp_err_t ret = i2s_channel_reconfig_std_clock(s_tx_chan, &clk_cfg);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "I2S 重设采样率失败: %s", esp_err_to_name(ret));
        }
        i2s_channel_enable(s_tx_chan);
        if (ret == ESP_OK) {
            s_current_i2s_rate = sample_rate;
            /* V1.0.44: 同步 ES8311 codec 采样率 (之前只重配 I2S 不重配 ES8311,
             * 导致 GB 24kHz 数据通过 44.1kHz 的 ES8311 播放, 音频卡顿). */
            es8311_set_sample_rate(sample_rate);
            ESP_LOGI(TAG, "I2S+ES8311 采样率切换: %d Hz", sample_rate);
        }
        return (ret == ESP_OK) ? 0 : -1;
    }

    s_current_i2s_rate = sample_rate;

    /* 创建 I2S 通道
     * V1.0.67: DMA 降到 6×576 (13.5KB), 给 WiFi 腾 DMA 内存 (蓝牙+音频已占 113KB).
     * 6×576=3456 帧 ≈ 72ms@24kHz, 足够吸收模拟器帧抖动不欠载. */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = 576;
    chan_cfg.auto_clear = true;
    esp_err_t ret = i2s_new_channel(&chan_cfg, &s_tx_chan, &s_rx_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S 创建通道失败: %s", esp_err_to_name(ret));
        s_tx_chan = NULL;
        s_rx_chan = NULL;
        return -1;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .bclk = I2S_BCLK,
            .ws   = I2S_WS,
            .dout = I2S_DOUT,
            .mclk = I2S_MCLK,
            .din  = I2S_DIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    ret = i2s_channel_init_std_mode(s_tx_chan, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S 初始化失败: %s", esp_err_to_name(ret));
        i2s_del_channel(s_tx_chan);
        i2s_del_channel(s_rx_chan);
        s_tx_chan = NULL;
        s_rx_chan = NULL;
        return -1;
    }

    ret = i2s_channel_init_std_mode(s_rx_chan, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S RX 初始化失败: %s", esp_err_to_name(ret));
        i2s_del_channel(s_tx_chan);
        i2s_del_channel(s_rx_chan);
        s_tx_chan = NULL;
        s_rx_chan = NULL;
        return -1;
    }

    ret = i2s_channel_enable(s_tx_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S 使能失败: %s", esp_err_to_name(ret));
        return -1;
    }

    return 0;
}

/* 诊断: 当前实际配置的 I2S 采样率 */
int audio_player_get_i2s_rate(void) {
    return s_current_i2s_rate;
}

/* 互斥包装: 串行化 feed_pcm / audio_out_task / audio_play_task 的 I2S 配置,
 * 避免高优先级 audio_out_task 抢占打断配置序列导致 I2S 状态损坏. */
static int i2s_init(int sample_rate) {
    if (s_i2s_mutex) xSemaphoreTake(s_i2s_mutex, portMAX_DELAY);
    int rc = i2s_init_locked(sample_rate);
    if (s_i2s_mutex) xSemaphoreGive(s_i2s_mutex);
    return rc;
}

static void i2s_deinit(void) {
    if (s_i2s_mutex) xSemaphoreTake(s_i2s_mutex, portMAX_DELAY);
    if (s_tx_chan) {
        i2s_channel_disable(s_tx_chan);
        i2s_del_channel(s_tx_chan);
        s_tx_chan = NULL;
    }
    if (s_rx_chan) {
        i2s_channel_disable(s_rx_chan);
        i2s_del_channel(s_rx_chan);
        s_rx_chan = NULL;
    }
    if (s_i2s_mutex) xSemaphoreGive(s_i2s_mutex);
}

/* ============ 麦克风输入 (ES7210 双麦克风阵列) ============ */

int audio_player_mic_start(int sample_rate) {
    if (s_mic_active) return 0;
    if (s_i2s_mutex) xSemaphoreTake(s_i2s_mutex, portMAX_DELAY);
    int rc = i2s_init_locked(sample_rate);
    if (rc == 0 && s_rx_chan) {
        esp_err_t ret = i2s_channel_enable(s_rx_chan);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "I2S RX 使能失败: %s", esp_err_to_name(ret));
            rc = -1;
        }
    } else {
        rc = -1;
    }
    if (s_i2s_mutex) xSemaphoreGive(s_i2s_mutex);
    if (rc != 0) return -1;

    if (es7210_init(I2C_NUM_0) != 0) {
        return -1;
    }
    s_mic_active = true;
    ESP_LOGI(TAG, "麦克风已启动 (%d Hz)", sample_rate);
    return 0;
}

void audio_player_mic_stop(void) {
    if (!s_mic_active) return;
    es7210_deinit();
    if (s_i2s_mutex) xSemaphoreTake(s_i2s_mutex, portMAX_DELAY);
    if (s_rx_chan) {
        i2s_channel_disable(s_rx_chan);
    }
    if (s_i2s_mutex) xSemaphoreGive(s_i2s_mutex);
    s_mic_active = false;
    ESP_LOGI(TAG, "麦克风已停止");
}

int audio_player_mic_read(int16_t *buf, int frames, int timeout_ms) {
    if (!s_mic_active || !s_rx_chan || !buf || frames <= 0) return -1;
    size_t bytes = (size_t)frames * 2 * sizeof(int16_t);   /* 立体声 L/R */
    size_t got = 0;
    esp_err_t ret = i2s_channel_read(s_rx_chan, buf, bytes, &got, timeout_ms);
    if (ret != ESP_OK) return -1;
    return (int)(got / (2 * sizeof(int16_t)));
}

bool audio_player_mic_active(void) {
    return s_mic_active;
}

/* ============ 功放控制 ============ */
static void pa_enable(bool on) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PA_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = false,
        .pull_down_en = false,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(PA_PIN, on ? 1 : 0);
}

/* ============ 多帧扫描估算 ============ */
static int64_t estimate_total_samples(FILE *f, int64_t file_size) {
    uint8_t *buf = heap_caps_malloc(16384, MALLOC_CAP_SPIRAM);
    if (!buf) buf = malloc(16384);
    if (!buf) return 0;

    fseek(f, 0, SEEK_SET);
    int n = fread(buf, 1, 16384, f);
    if (n < 100) { free(buf); return 0; }

    int meta_size = 0;
    if (n >= 10 && buf[0] == 'I' && buf[1] == 'D' && buf[2] == '3') {
        uint32_t id3_sz = ((uint32_t)buf[6] << 21) | ((uint32_t)buf[7] << 14) |
                          ((uint32_t)buf[8] << 7) | buf[9];
        meta_size = 10 + id3_sz;
    }

    mp3dec_t *decoder = heap_caps_malloc(sizeof(mp3dec_t), MALLOC_CAP_SPIRAM);
    if (!decoder) decoder = malloc(sizeof(mp3dec_t));
    if (!decoder) { free(buf); return 0; }
    mp3dec_init(decoder);

    int64_t total_frame_samples = 0;
    int64_t total_frame_bytes = 0;
    int frame_count = 0;
    int sample_rate = 0;
    int first_frame_off = -1;
    int pos = meta_size;

    while (pos < n - 4 && frame_count < 16) {
        if (buf[pos] != 0xFF || (buf[pos+1] & 0xE0) != 0xE0) {
            pos++;
            continue;
        }

        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame(decoder, buf + pos, n - pos, NULL, &info);

        if (info.frame_bytes > 0 && samples > 0) {
            if (first_frame_off < 0) {
                first_frame_off = pos;
                sample_rate = info.hz;
            }
            total_frame_samples += samples;
            total_frame_bytes += info.frame_bytes;
            frame_count++;
            pos += info.frame_bytes;
        } else {
            pos++;
        }
    }

    free(buf);

    if (frame_count < 1 || sample_rate <= 0) {
        ESP_LOGW(TAG, "\u591A\u5E27\u626B\u63CF\u672A\u627E\u5230\u6709\u6548\u5E27");
        free(decoder);
        return 0;
    }

    int64_t audio_data_offset = (first_frame_off < 0) ? 0 : first_frame_off;
    int64_t audio_data_size = file_size - audio_data_offset;
    if (audio_data_size <= 0) { free(decoder); return 0; }

    int64_t avg_frame_size = total_frame_bytes / frame_count;
    int avg_samples = (int)(total_frame_samples / frame_count);

    int64_t total_frames = audio_data_size / avg_frame_size;
    int64_t total_samples = total_frames * avg_samples;

    ESP_LOGI(TAG, "\u591A\u5E27\u4F30\u7B97: \u5E27\u6570=%d, \u5E73\u5747\u5E27=%lld\u5B57\u8282/%d\u91C7\u6837, "
             "\u97F3\u9891\u504F\u79FB=%lld, \u97F3\u9891\u5927\u5C0F=%lld, \u4F30\u8BA1\u5E27\u6570=%lld, \u91C7\u6837\u7387=%d, \u65F6\u957F=%.1f\u79D2",
             frame_count, (long long)avg_frame_size, avg_samples,
             (long long)audio_data_offset, (long long)audio_data_size,
             (long long)total_frames, sample_rate,
             (double)total_samples / sample_rate);

    s_mp3_sample_rate = sample_rate;
    free(decoder);
    return total_samples;
}

/* ============ 播放任务 ============ */
/* 任务栈: minimp3解码+FAT文件系统+I2S驱动调用实测峰值~52KB, 给 64KB 余量.
 * ESP32-S3 的 FreeRTOS StackType_t = uint32_t (4字节),
 * xTaskCreateStaticPinnedToCore 的 ulStackDepth 以"字数"计.
 * 栈缓冲放在 PSRAM (EXT_RAM_BSS_ATTR), 与 bt_manager 的连接/初始化任务同款做法:
 * 开启 CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM 后, PSRAM 栈通过
 * portVALID_STACK_MEM 断言, 不会重启. 内部 DRAM 最大连续块仅 ~31KB,
 * 无法满足 52KB+ 需求, 故必须用 PSRAM; PSRAM 有 8MB, 64KB 永远充足。 */
#define AUDIO_TASK_STACK_BYTES  65536   /* 64KB */
#define AUDIO_TASK_STACK_WORDS  ((AUDIO_TASK_STACK_BYTES + sizeof(StackType_t) - 1) / sizeof(StackType_t))
#define I2S_WRITE_TIMEOUT_MS   500     /* I2S 写入超时 */
EXT_RAM_BSS_ATTR static StackType_t s_audio_stack[AUDIO_TASK_STACK_WORDS];
static StaticTask_t s_audio_tcb;
static uint32_t s_task_stack_words = AUDIO_TASK_STACK_WORDS; /* 用于日志 */

static void audio_play_task(void *arg) {
    ESP_LOGI(TAG, ">>> \u97F3\u9891\u4EFB\u52A1\u5DF2\u542F\u52A8 (\u6808 %u \u5B57\u8282) <<<",
             (unsigned)(s_task_stack_words * sizeof(StackType_t)));
    mp3d_sample_t *pcm = NULL;
    int16_t *pcm_out = NULL;

    ESP_LOGI(TAG, "[1/6] \u6CE8\u518C TWDT...");
    esp_task_wdt_add(NULL);

    ESP_LOGI(TAG, "[2/6] \u521D\u59CB\u5316 I2S...");
    int sample_rate = 44100;
    int i2s_ret = i2s_init(sample_rate);
    if (i2s_ret != 0) {
        ESP_LOGE(TAG, "I2S \u521D\u59CB\u5316\u5931\u8D25, \u505C\u6B62\u64AD\u653E");
        goto cleanup;
    }
    ESP_LOGI(TAG, "[3/6] \u542F\u52A8 ES8311...");
    vTaskDelay(pdMS_TO_TICKS(20));
    es8311_start();

    ESP_LOGI(TAG, "[4/6] \u5F00\u542F\u529F\u653E...");
    pa_enable(true);
    es8311_set_volume(s_volume);

    ESP_LOGI(TAG, "[5/6] \u5206\u914D\u89E3\u7801\u7F13\u51B2\u533A...");
    mp3dec_init(&s_mp3dec);
    s_mp3_buf_pos = 0;
    s_mp3_buf_len = 0;
    int current_sample_rate = sample_rate;
    bool first_frame = true;
    pcm = heap_caps_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(mp3d_sample_t), MALLOC_CAP_SPIRAM);
    pcm_out = heap_caps_malloc(2304 * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!pcm || !pcm_out) {
        ESP_LOGE(TAG, "PSRAM \u5206\u914D\u5931\u8D25");
        goto cleanup;
    }

    ESP_LOGI(TAG, "[6/6] \u8FDB\u5165\u89E3\u7801\u5FAA\u73AF...");
    while (!s_stop_requested) {
        /* \u5582\u72D7: \u9632\u6B62 TWDT \u8D85\u65F6\u91CD\u542F */
        esp_task_wdt_reset();

        if (s_state == AUDIO_STATE_PAUSED) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        /* \u586B\u5145 MP3 \u6570\u636E\u7F13\u51B2 */
        if (s_mp3_buf_len < MP3_FRAME_BUF_SIZE) {
            int to_read = MP3_FRAME_BUF_SIZE - s_mp3_buf_len;
            int n = fread(s_mp3_buf + s_mp3_buf_len, 1, to_read, s_mp3_file);
            if (n > 0) {
                s_mp3_buf_len += n;
            } else if (n == 0 && s_mp3_buf_len == 0 && feof(s_mp3_file)) {
                ESP_LOGI(TAG, "\u6587\u4EF6\u8BFB\u53D6\u5B8C\u6BD5 (\u603B\u5927\u5C0F=%lld)", (long long)s_file_size);
            }
        }

        /* \u89E3\u7801\u4E00\u5E27 MP3 */
        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame(&s_mp3dec,
            s_mp3_buf + s_mp3_buf_pos,
            s_mp3_buf_len - s_mp3_buf_pos,
            pcm, &info);

        if (info.frame_bytes > 0) {
            s_mp3_buf_pos += info.frame_bytes;
            if (s_mp3_buf_pos > 0) {
                int remaining = s_mp3_buf_len - s_mp3_buf_pos;
                if (remaining > 0) {
                    memmove(s_mp3_buf, s_mp3_buf + s_mp3_buf_pos, remaining);
                }
                s_mp3_buf_len = remaining;
                s_mp3_buf_pos = 0;
            }
        } else {
            if (s_mp3_buf_len == 0 || feof(s_mp3_file)) {
                ESP_LOGI(TAG, "\u64AD\u653E\u7ED3\u675F: %s", s_filename);
                break;
            }
            s_mp3_buf_pos++;
            if (s_mp3_buf_pos >= s_mp3_buf_len) {
                s_mp3_buf_len = 0;
                s_mp3_buf_pos = 0;
            }
            continue;
        }

        if (samples > 0) {
            if (info.hz != current_sample_rate || first_frame) {
                /* MP3 任务的重配序列也要与 audio_out_task/feed_pcm 互斥 */
                if (s_i2s_mutex) xSemaphoreTake(s_i2s_mutex, portMAX_DELAY);
                if (!first_frame) {
                    i2s_channel_disable(s_tx_chan);
                }
                current_sample_rate = info.hz;
                ESP_LOGI(TAG, "\u91C7\u6837\u7387: %d Hz, \u58F0\u9053: %d, \u5E27\u5B57\u8282: %d", info.hz, info.channels, info.frame_bytes);
                if (first_frame) {
                    first_frame = false;
                    ESP_LOGI(TAG, ">>> \u5F00\u59CB\u89E3\u7801MP3\u97F3\u9891\u6570\u636E <<<");
                } else {
                    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(current_sample_rate);
                    i2s_channel_reconfig_std_clock(s_tx_chan, &clk_cfg);
                    i2s_channel_enable(s_tx_chan);
                }
                es8311_set_sample_rate(current_sample_rate);
                if (s_i2s_mutex) xSemaphoreGive(s_i2s_mutex);
            }

            /* mono \u2192 stereo */
            int16_t *src = (int16_t *)pcm;
            if (info.channels == 1) {
                for (int i = 0; i < samples; i++) {
                    pcm_out[i * 2]     = src[i];
                    pcm_out[i * 2 + 1] = src[i];
                }
            } else {
                memcpy(pcm_out, src, samples * 2 * sizeof(int16_t));
            }

            /* \u97F3\u91CF\u8C03\u8282 */
            if (s_volume < 100) {
                float vol = (float)s_volume / 100.0f;
                int total = samples * 2;
                for (int i = 0; i < total; i++) {
                    pcm_out[i] = (int16_t)(pcm_out[i] * vol);
                }
            }

            /* \u5199\u5165 I2S */
            size_t bytes_written = 0;
            size_t to_write = samples * 2 * sizeof(int16_t);
            esp_err_t wret = i2s_channel_write(s_tx_chan, pcm_out, to_write,
                                               &bytes_written,
                                               pdMS_TO_TICKS(I2S_WRITE_TIMEOUT_MS));
            if (wret != ESP_OK) {
                ESP_LOGW(TAG, "I2S \u5199\u5165\u5931\u8D25: %s (\u5DF2\u5199 %u/%u)",
                         esp_err_to_name(wret), (unsigned)bytes_written, (unsigned)to_write);
                vTaskDelay(pdMS_TO_TICKS(10));
            }

            s_elapsed_samples += samples;
            if (s_total_samples > 0) {
                s_progress = (int)((s_elapsed_samples * 1000) / s_total_samples);
                if (s_progress > 1000) s_progress = 1000;
            }
        }
    }

cleanup:
    pa_enable(false);
    if (pcm) free(pcm);
    if (pcm_out) free(pcm_out);
    s_state = AUDIO_STATE_STOPPED;
    if (s_mp3_file) {
        fclose(s_mp3_file);
        s_mp3_file = NULL;
    }
    /* 注销 TWDT: 注册后不注销, 任务删除后 TWDT 仍持有句柄, 5s 后触发误报警告 */
    esp_task_wdt_delete(NULL);
    ESP_LOGI(TAG, "\u64AD\u653E\u4EFB\u52A1\u7ED3\u675F, \u6808\u9AD8\u6C34\u4F4D\u5269\u4F59=%u \u5B57 (\u603B %u \u5B57)",
             (unsigned)uxTaskGetStackHighWaterMark(NULL), (unsigned)s_task_stack_words);
    s_task_handle = NULL;
    vTaskDelete(NULL);
}

/* ============ \u516C\u5171 API ============ */
int audio_player_init(void) {
    if (s_hw_initialized) return 0;

    ESP_LOGI(TAG, "初始化音频硬件...");
    if (!s_i2s_mutex) {
        s_i2s_mutex = xSemaphoreCreateMutex();
    }
    s_mutex = xSemaphoreCreateMutex();

    /* MP3 缓冲区延迟到 audio_player_play() 分配, 节省 PSRAM */

    /* 先 I2S/MCLK 再 ES8311 */
    if (i2s_init(44100) != 0) {
        ESP_LOGE(TAG, "I2S/MCLK \u521D\u59CB\u5316\u5931\u8D25");
        return -1;
    }
    vTaskDelay(pdMS_TO_TICKS(20));

    if (es8311_init(I2C_SDA, I2C_SCL) != 0) {
        ESP_LOGE(TAG, "ES8311 \u521D\u59CB\u5316\u5931\u8D25");
        return -1;
    }

    /* 探测板载双麦克风 ADC (ES7210), 仅日志, 实际使能由电子书敲击翻页触发 */
    int mic_addr = es7210_probe(I2C_NUM_0);
    if (mic_addr) {
        ESP_LOGI(TAG, "ES7210 麦克风 ADC 探测成功: 0x%02X", mic_addr);
    } else {
        ESP_LOGW(TAG, "ES7210 麦克风 ADC 未探测到 (0x40-0x43 无响应, 敲击翻页将不可用)");
    }

    pa_enable(false);

    s_hw_initialized = true;
    ESP_LOGI(TAG, "\u97F3\u9891\u786C\u4EF6\u521D\u59CB\u5316\u5B8C\u6210");
    return 0;
}

int audio_player_play(const char *filepath) {
    /* V1.0.68: 全局静音时禁止播放音乐 */
    if (s_muted) return -1;

    /* V1.0.60: 解耦 MP3 与游戏音频.
     * 游戏用的 PCM 环形缓冲输出任务 (audio_out) 与 MP3 任务都在写同一个 I2S,
     * 若上一局游戏结束未停, MP3 开始时会双任务并发写 I2S -> 崩溃/卡死.
     * 这里先停掉 audio_out, 保证同一时间只有一个 I2S 写者. */
    audio_out_stop();

    if (!s_hw_initialized) {
        if (audio_player_init() != 0) return -1;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (s_task_handle) {
        s_stop_requested = true;
        xSemaphoreGive(s_mutex);
        while (s_task_handle) vTaskDelay(pdMS_TO_TICKS(10));
        xSemaphoreTake(s_mutex, portMAX_DELAY);
    }
    s_stop_requested = false;

    const char *name = strrchr(filepath, '/');
    if (name) name++; else name = filepath;
    strncpy(s_filename, name, sizeof(s_filename) - 1);
    s_filename[sizeof(s_filename) - 1] = '\0';

    s_mp3_file = fopen(filepath, "rb");
    if (!s_mp3_file) {
        ESP_LOGE(TAG, "\u65E0\u6CD5\u6253\u5F00: %s", filepath);
        xSemaphoreGive(s_mutex);
        return -1;
    }

    fseek(s_mp3_file, 0, SEEK_END);
    s_file_size = ftell(s_mp3_file);
    fseek(s_mp3_file, 0, SEEK_SET);
    s_progress = 0;
    s_elapsed_samples = 0;

    s_total_samples = estimate_total_samples(s_mp3_file, s_file_size);
    fseek(s_mp3_file, 0, SEEK_SET);

    s_state = AUDIO_STATE_PLAYING;

    /* 延迟分配 MP3 缓冲区 (只在播放时才占内存) */
    if (!s_mp3_buf) {
        s_mp3_buf = heap_caps_malloc(MP3_FRAME_BUF_SIZE, MALLOC_CAP_SPIRAM);
        if (!s_mp3_buf) s_mp3_buf = malloc(MP3_FRAME_BUF_SIZE);
        if (!s_mp3_buf) {
            ESP_LOGE(TAG, "MP3 缓冲区分配失败");
            fclose(s_mp3_file);
            s_mp3_file = NULL;
            s_state = AUDIO_STATE_STOPPED;
            xSemaphoreGive(s_mutex);
            return -1;
        }
    }

    /* 创建播放任务: 静态栈缓冲在 PSRAM (EXT_RAM_BSS_ATTR),
     * 开 CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM 后通过栈内存断言, 不会重启。 */
    s_task_stack_words = AUDIO_TASK_STACK_WORDS;
    s_task_handle = xTaskCreateStaticPinnedToCore(audio_play_task, "audio_play",
                                                   AUDIO_TASK_STACK_WORDS, NULL, 1,
                                                   s_audio_stack, &s_audio_tcb, 1);
    if (s_task_handle == NULL) {
        ESP_LOGE(TAG, "音频静态任务创建失败!");
        if (s_mp3_buf) { free(s_mp3_buf); s_mp3_buf = NULL; }
        fclose(s_mp3_file);
        s_mp3_file = NULL;
        s_state = AUDIO_STATE_STOPPED;
        xSemaphoreGive(s_mutex);
        return -1;
    }
    ESP_LOGI(TAG, "音频任务已创建 (栈 %u 字 = %u 字节, PSRAM)",
             (unsigned)s_task_stack_words,
             (unsigned)(s_task_stack_words * sizeof(StackType_t)));

    ESP_LOGI(TAG, "\u5F00\u59CB\u64AD\u653E: %s (%lld bytes)", s_filename, (long long)s_file_size);
    xSemaphoreGive(s_mutex);
    return 0;
}

void audio_player_pause(void) {
    if (s_state == AUDIO_STATE_PLAYING) {
        s_state = AUDIO_STATE_PAUSED;
        pa_enable(false);
        ESP_LOGI(TAG, "\u6682\u505C");
    }
}

void audio_player_resume(void) {
    if (s_state == AUDIO_STATE_PAUSED) {
        s_state = AUDIO_STATE_PLAYING;
        pa_enable(true);
        ESP_LOGI(TAG, "\u6062\u590D");
    }
}

void audio_player_stop(void) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_task_handle) {
        s_stop_requested = true;
    }
    pa_enable(false);
    xSemaphoreGive(s_mutex);

    while (s_task_handle) vTaskDelay(pdMS_TO_TICKS(10));
    s_state = AUDIO_STATE_STOPPED;
    audio_out_stop();   /* 解耦: 一并停掉游戏 PCM 输出任务 */
    ESP_LOGI(TAG, "\u505C\u6B62\u64AD\u653E");
}

void audio_player_set_volume(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    s_volume = percent;
    es8311_set_volume(s_muted ? 0 : percent);
}

int audio_player_get_volume(void) {
    return s_volume;
}

void audio_player_set_muted(bool muted) {
    if (s_muted == muted) return;
    s_muted = muted;
    if (muted) {
        audio_player_stop();          /* 停掉正在播放的音乐 */
        es8311_set_volume(0);         /* 编解码器音量拉到 0, 彻底静音 */
    } else {
        es8311_set_volume(s_volume);  /* 恢复之前音量 */
    }
    ESP_LOGI(TAG, "音频强制静音: %s", muted ? "开" : "关");
}

bool audio_player_is_muted(void) {
    return s_muted;
}

audio_state_t audio_player_get_state(void) {
    return s_state;
}

const char *audio_player_get_filename(void) {
    return s_filename;
}

int audio_player_get_progress(void) {
    return s_progress;
}

bool audio_player_is_running(void) {
    return s_task_handle != NULL;
}

void audio_player_deinit(void) {
    audio_player_stop();
    i2s_deinit();
    es8311_shutdown();
    pa_enable(false);
    s_hw_initialized = false;
}

/* 直接馈入 PCM 数据到环形缓冲 (非阻塞, 供游戏模拟器调用) */
size_t audio_player_feed_pcm(const int16_t *data, size_t frames, int sample_rate) {
    /* V1.0.68: 全局静音时直接丢弃 PCM, 不启动音频输出任务 */
    if (s_muted) return 0;
    if (!data || frames == 0) return 0;

    /* V1.0.60: 解耦: 游戏/引擎开始前停掉可能还在跑的 MP3 播放任务,
     * 避免双任务并发写 I2S. */
    if (s_task_handle) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_stop_requested = true;
        xSemaphoreGive(s_mutex);
        while (s_task_handle) vTaskDelay(pdMS_TO_TICKS(10));
        s_state = AUDIO_STATE_STOPPED;
    }

    /* V1.0.45: 更新共享采样率, audio_out_task 会读取此值.
     * 必须在 i2s_init 之前设置, 确保 audio_out_task 不会用旧值重配. */
    if (sample_rate > 0) {
        s_desired_sample_rate = sample_rate;
    }

    /* V1.0.44: 根据传入采样率重配 I2S 时钟 (之前忽略 sample_rate 导致
     * GB 24kHz 数据被当 44.1kHz 播放, 音调变高且卡顿). */
    if (s_hw_initialized && sample_rate > 0) {
        i2s_init(sample_rate);  /* i2s_init 内部会判断采样率是否相同 */
    }

    /* 确保音频硬件已初始化 (用户可能未经过 MP3 播放器直接进入游戏) */
    if (!s_hw_initialized) {
        ESP_LOGI(TAG, "feed_pcm: 音频硬件未初始化, 正在初始化...");
        if (audio_player_init() != 0) {
            ESP_LOGE(TAG, "feed_pcm: 音频硬件初始化失败, 无法输出音频");
            return 0;
        }
        if (sample_rate > 0) i2s_init(sample_rate);
        ESP_LOGI(TAG, "feed_pcm: 音频硬件初始化完成 (sr=%d)", sample_rate);
    }

    /* 首次调用时启动音频输出任务 */
    if (!s_audio_out_running) {
        audio_out_start();
    }

    if (!s_pcm_ring) return 0;

    size_t written = 0;
    for (size_t i = 0; i < frames; i++) {
        /* 检查环形缓冲是否满 */
        uint32_t next_head = (s_ring_head + 2) % PCM_RING_SIZE;
        if (next_head == s_ring_tail) break;  /* 缓冲满, 丢弃剩余帧 */

        s_pcm_ring[s_ring_head] = data[i * 2];      /* L */
        s_pcm_ring[s_ring_head + 1] = data[i * 2 + 1]; /* R */
        s_ring_head = next_head;
        written++;
    }
    return written;
}

/* 立即清空 PCM 环形缓冲 (按键音效释放时切断尾音) */
void audio_player_flush_pcm(void)
{
    s_pcm_flush_requested = true;
}

/* 按键反馈音选项已移除 (见 menu_system 游戏设置): 不再提供蜂鸣声. */
