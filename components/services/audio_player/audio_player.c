/* MP3 音频播放器
 * - I2S 输出: 官方板 → ES8311 codec → AAXS2005 功放; 自制板 → NS4168 I2S 数字功放直驱
 * - minimp3 解码 MP3 文件
 * - 后台 FreeRTOS 任务持续解码播放
 *
 * 两板一固件 (V1.0.70):
 *   官方板 (微雪 ESP32-S3-RLCD-4.2): I2S → ES8311 (I2C 配置) → 功放. GPIO46=PA_EN(高=开).
 *   自制板 (参考官方重打 PCB): 无 ES8311 → I2S 直接喂 NS4168 (I2S 数字输入功放, 零配置).
 *       GPIO46=NS4168 CTRL (高=工作/右声道, 低=关断静音). 数据路径同一路 I2S.
 *   平台自动探测: es8311_is_present() 有芯片=官方板, 无=NS4168 平台.
 *
 * 硬件引脚:
 *   s_bclk = GPIO9, s_ws = GPIO45, s_dout = GPIO8
 *   s_mclk = GPIO16, PA_ENABLE/NS_CTRL = GPIO46
 *   s_i2c_sda = GPIO13, s_i2c_scl = GPIO14
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
/* ★ 2026-11-22: xTaskCreatePinnedToCoreWithCaps —— 动态分配 TCB+栈并指定内存能力。
 *   用它替代 xTaskCreateStaticPinnedToCore 是本轮 MP3 崩溃的根治点, 详见
 *   audio_play_task 上方"静态 TCB 复用"长注释。 */
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"   /* 音量/静音持久化 (本服务是唯一归属) */
#include "esp_timer.h"   /* 音量落盘防抖 */
#include "esp_task_wdt.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

static const char *TAG = "AUDIO";

/* 硬件引脚
 * ★★ 2026-10-05 小智模块板 (分支 board/xiaozhi-s3-core) —— **全部按模块内部原理图定案**
 *   (依据: `ESP32-S3-AI-2E_v1.0.pdf` 模块内部原理图, 用矢量线追踪 + 坐标为证)
 *
 *   模块**内置** ES8311(播放) + ES7210(双麦) + LPA2103A(功放): 喇叭接 VOP/VON, 耳机接 OUTP/OUTN,
 *   咪头接 MIC1P/N·MIC2P/N。**MCU↔codec 的 I2S/I2C 全在模块内部走线**，
 *   而且**故意占用了那些"外部没引出"的焊盘位置对应的 GPIO** —— 这就是为什么它们没引出来：
 *
 *   信号             ESP32 GPIO   模块焊盘(该位置外部的名字)
 *   s_mclk         GPIO40      pad 33  (MIC2N)
 *   I2S_SCLK(BCLK)   GPIO39      pad 32  (MIC2P)
 *   I2S_LRCK(WS)     GPIO45      pad 26  (VBUS_5V)
 *   I2S_DSDIN (ESP→codec)  GPIO48  pad 25  (VSYS_OUT)
 *   I2S_ASDOUT(codec→ESP)  GPIO21  pad 23  (VBAT)
 *   s_i2c_sda          GPIO38      pad 31  (MIC1N)
 *   s_i2c_scl          GPIO47      pad 24  (VSYS_IN)
 *   CTRL(功放 SD)     GPIO46      pad 16  (VON)
 *
 *   ⚠ **绝不能拿这些脚做别的用途**(会搅 codec/功放)。尤其 GPIO45/46/47/48、39/40、21、38
 *     全都被模块内部占了 —— 外部只需接喇叭/耳机/咪头。
 *   ✅ 视频规格书"codec I2C = IO38/IO47"被实机证实(ES8311@0x18 应答)。 */
#define I2S_BCLK    GPIO_NUM_39   /* 原 9  —— 模块内部 I2S_SCLK */
#define I2S_WS      GPIO_NUM_45   /* 原 45 —— 模块内部 I2S_LRCK (两板巧合同脚) */
#define I2S_DOUT    GPIO_NUM_48   /* 原 8  —— 模块内部 I2S_DSDIN */
#define I2S_DIN     GPIO_NUM_21   /* 原 10 —— 模块内部 I2S_ASDOUT (codec→ESP) */
#define I2S_MCLK    GPIO_NUM_40   /* 原 16 —— 模块内部 I2S_MCLK */
/* 功放使能/静音脚 = 模块内部 CTRL → LPA2103A 的 SD(1 脚), 高=工作 */
/* ---- 本板音频引脚 (★ 2026-10-16: 由 app_board 从板型表下发, 驱动不猜板型) ----
 * 默认值 = 小智模块+新载板; 微雪原板由 audio_player_apply_pins() 覆盖。 */
/* ⛔ 默认值改成 0/-1 = "未配置": 原来是 40/39/45/48/21(小智的), 而 40 正好压在微雪 LCD CS 上。
 *   现在必须由 app_board 用板型表下发 (audio_player_apply_pins) 才会生效。 */
static int s_mclk = -1, s_bclk = -1, s_ws = -1, s_dout = -1, s_din = -1;
static int s_i2c_sda = -1, s_i2c_scl = -1, s_pa = 46;   /* pa 两板同脚(46); I2C 由上层下发 */

void audio_player_apply_pins(int mclk, int bclk, int ws, int dout, int din,
                             int codec_sda, int codec_scl, int pa) {
    s_mclk = mclk; s_bclk = bclk; s_ws = ws; s_dout = dout; s_din = din;
    s_i2c_sda = codec_sda; s_i2c_scl = codec_scl; s_pa = pa;
}

#define PA_PIN      GPIO_NUM_46

/* NS4168 CTRL / 音频静音控制脚 = GPIO46 (对齐新板 netlist: NS4168 CTRL=GPIO46).
 * 46 旧板 = PA 功放使能, 新板(NS4168) = CTRL 静音. 仅 NS4168 平台由本驱动当 CTRL 用;
 * ES8311 平台 46 归功放使能, 静音走软静音. */
#define NS_CTRL_PIN GPIO_NUM_46
/* ★ 2026-10-05 小智模块板 (分支 board/xiaozhi-s3-core): codec 控制 I2C 从 13/14 改到 **38/47**。
 *   依据: 厂商规格书明写 `ES8311/ES7210 的 s_i2c_sda/SCL 连接到 ESP32 的 IO38/IO47`，
 *   而且这两个脚**没有引出到焊盘**(模块内部走线) —— 固件直接用即可。
 *   ⛔ **必须改**: 13/14 现在是 **TF 卡的数据线**(TF CLK=13 / D0=14)，
 *     音频 I2C 再去驱动这两个脚 = **和 TF 卡抢线**，会把卡读挂/读错。 */
#define I2C_SDA     38
#define I2C_SCL     47

/* 解码缓冲区 (PSRAM) */
/* ★ 2026-10-25 性能优化: 4K → 32K. 4K 时几乎每解码 1-2 帧就要 fread 一次,
 *   FATFS+TF 卡小读延迟直接吃进解码循环; 32K 让 TF 大块读摊薄延迟.
 *   播放时才分配(PSRAM), 停止即释放, 不常驻. */
#define MP3_FRAME_BUF_SIZE  32768       /* MP3 帧数据读取缓冲 */

/* 状态 */
static audio_state_t s_state = AUDIO_STATE_IDLE;
static int  s_volume = 100;             /* 默认音量 100%, ES8311 映射到 0xC0(0dB) 不削波 */
static bool s_muted = false;            /* V1.0.68: 全局强制静音 */
static bool s_use_ns4168_ctrl = false;  /* NS4168 平台(无 ES8311) 才用 GPIO46 作 CTRL 静音 */
EXT_RAM_BSS_ATTR static char s_filename[256];   /* 只在播放/录音时用 */
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
/* V9.1: 跳转请求 (0..1000 千分比, -1=无). 由 OS 线程置位, 解码任务循环内安全执行,
 * 避免与解码任务并发操作 s_mp3_file/解码缓冲导致竞态. */
static int  s_seek_req_progress = -1;
/* V9.1: 上一曲是否"自然播完" (EOF), 区别于用户主动 stop. 供页面实现循环自动切歌. */
static bool s_natural_end = false;

/* minimp3 解码器
 * V1.0.44: 移到 PSRAM, 释放 6.6KB 内部 DRAM.
 * MP3 解码非实时高频访问, PSRAM 带宽足够. */
/* ★ 2026-10-25 性能优化: 原来解码器热数据(mdicct ring/scratch ~10KB)静态放 PSRAM,
 *   解码主循环每次访问都走 cache → 慢. 改为播放开始时分配在内部 RAM,
 *   播放结束(cleanup)即释放 —— 不播放时不占内部 RAM, 符合工程约定. */
static mp3dec_t *s_mp3dec;
static uint8_t *s_mp3_buf = NULL;       /* PSRAM 分配 */
static int  s_mp3_buf_pos = 0;    /* 已填充位置 */
static int  s_mp3_buf_len = 0;    /* 有效数据长度 */

/* I2S 通道句柄 */
static i2s_chan_handle_t s_tx_chan = NULL;
static i2s_chan_handle_t s_rx_chan = NULL;
static bool s_mic_active = false;

/* ★ 2026-11-22: 通道 enable 状态做**唯一真相源**, 不再各处直接调 i2s_channel_enable/disable。
 *   原因: IDF 的 i2s_channel_disable() 对"没 enable 过"的通道会打
 *     E i2s_common: i2s_channel_disable(1370): the channel has not been enabled yet
 *   （本机每次开机必现一条: 初始化末尾统一关时钟时 RX 从未 enable 过）；
 *   反过来 i2s_channel_enable() 对已 enable 的通道同样报错, 且**不改变**时钟状态。
 *   这两条以前靠"调用点自己判断"来避, 但调用点有 10 处, 判断条件各不相同
 *   (有的看 s_mic_active, 有的看 s_tx_chan)。现在状态只在这里维护。 */
static bool s_tx_on = false;
static bool s_rx_on = false;

static esp_err_t i2s_tx_enable(void)
{
    if (!s_tx_chan) return ESP_ERR_INVALID_STATE;
    if (s_tx_on) return ESP_OK;
    esp_err_t r = i2s_channel_enable(s_tx_chan);
    if (r == ESP_OK) s_tx_on = true;
    return r;
}
static void i2s_tx_disable(void)
{
    if (!s_tx_chan || !s_tx_on) return;
    i2s_channel_disable(s_tx_chan);
    s_tx_on = false;
}
static esp_err_t i2s_rx_enable(void)
{
    if (!s_rx_chan) return ESP_ERR_INVALID_STATE;
    if (s_rx_on) return ESP_OK;
    esp_err_t r = i2s_channel_enable(s_rx_chan);
    if (r == ESP_OK) s_rx_on = true;
    return r;
}
static void i2s_rx_disable(void)
{
    if (!s_rx_chan || !s_rx_on) return;
    i2s_channel_disable(s_rx_chan);
    s_rx_on = false;
}

/* === 环形缓冲区 (游戏模拟器音频输出) === */
/* ★ 2026-10-25 加大: 64KB(682ms) → 256KB(131072 个 int16 = 65536 帧 ≈ 1486ms @44100Hz).
 *   实测游戏内出现过 avail=256 / underruns=142872 / play=705Hz 的严重欠载,
 *   64KB 在 SD 读取/换曲/场景加载这类几百 ms 的抖动面前不够厚, 一掏空就断音。
 *   缓冲全在 PSRAM(见 s_pcm_ring 分配), 不占内部 RAM。 */
/* ★ 诊断探针 (统一命名 LIN_DIAG_<模块>, 登记于 docs/diagnostics/probes.md)
 *   LIN_DIAG_AUDIO: 环形缓冲水位探针 —— 每 400 轮打印
 *     "PCM ring: avail=.. underruns=.. want=..Hz i2s=..Hz play=..Hz"。
 *     用途: 排查游戏音频断续/欠载/采样率不符。
 *     默认关: 游戏中约每数秒一行, 长期占用串口; 关闭后连统计变量一起省掉。 */
#define LIN_DIAG_AUDIO 0

#define PCM_RING_SIZE 131072
/* 首次播放前先让环形缓冲攒够 ~186ms 音频再开始掏空,
 * 吸收模拟器帧抖动, 避免声音一卡一卡 (生产/消耗速率几乎相等时尤其重要).
 * 20260812: 4096 帧在 GB/GBC 45fps 抖动下仍偶发欠载, 翻倍到 8192. */
#define PCM_RING_PRIME_FRAMES 8192
static int16_t *s_pcm_ring = NULL;
static volatile uint32_t s_ring_head = 0;  /* 生产者写入位置 */
static volatile uint32_t s_ring_tail = 0;  /* 消费者读取位置 */
static TaskHandle_t s_audio_out_task = NULL;
static volatile bool s_audio_out_running = false;
static volatile bool s_pcm_flush_requested = false;   /* 按键音效释放时立即断音 */

/* V1.0.71: 音频振幅表 (峰值 0..1000, 千分比).
 * MP3 解码任务与 audio_out_task 两条 I2S 写入路径都喂数据,
 * attack 立即跟上 / decay 每帧回落 10%. 供"随音乐震动"实时采样. */
static volatile int32_t s_meter_peak = 0;
static void audio_meter_feed(const int16_t *buf, uint32_t n) {
    if (!buf || n == 0) return;
    int32_t pk = 0;
    for (uint32_t i = 0; i < n; i++) {
        int32_t v = buf[i];
        if (v < 0) v = -v;
        if (v > pk) pk = v;
    }
    pk = (pk * 1000) / 32768;
    int32_t cur = s_meter_peak;
    if (pk > cur) s_meter_peak = pk;             /* attack: 立即跟上 */
    else          s_meter_peak = (cur * 9 + pk) / 10;  /* decay: 10% 回落 */
}
int32_t audio_player_get_meter(void) { return s_meter_peak; }
static uint32_t s_last_underrun_ms = 0;   /* 欠载日志: 上次欠载时刻 (区分偶发/持续) */

/* V1.7.x (2026-09-28): 空闲静音状态 —— 治"喇叭老是莎莎莎"底噪.
 * 真因: TX 通道一直使能而 DMA 被饿死时, I2S 会**反复重发上一段 DMA 缓冲**,
 *       而功放自第一次出声后就一直通电 → 喇叭持续响底噪. */
static volatile bool     s_amp_on = false;         /* 功放/时钟当前是否开着 */
static volatile uint32_t s_last_data_ms = 0;       /* 上次真正有 PCM 数据的时刻 */

/* V1.0.53: PCM 输出任务栈改 PSRAM 静态, 不再占内部 RAM.
 * 6KB 在 44.1kHz 冷启动 (大帧数据 + 音量缩放 + I2S 驱动调用链) 仍会溢出,
 * 栈在 PSRAM, 直接给 12KB 余量. */
/* V1.0.55: 3072 字 (12KB) 在加入播放速率日志 (ESP_LOGI 格式化) 时爆栈,
 * 任务栈在 PSRAM, 直接给 24KB 余量. */
#define PCM_OUT_STACK_WORDS  6144   /* 24576 字节 */
/* ★ LINTOS 2026-10-25: 任务栈原来静态常驻 PSRAM, 改成首次用到才分配 (不播放就不占) */
/* ★ 2026-11-22: 删掉 s_pcm_out_stack / s_pcm_out_tcb 两个文件作用域静态缓冲。
 *   它们原来被 xTaskCreateStaticPinnedToCore 反复复用 —— 游戏里 audio_out
 *   频繁 start/stop，旧 TCB 还在 terminated 链表时新任务就地重写它，
 *   会把 FreeRTOS 链表串坏。现在 TCB/栈全部动态分配（见 audio_out_start）。 */
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
static void ensure_out_active(void);
static void audio_output_begin(void);   /* ★ 2026-11-22 音频输出唯一收敛点 */
static int i2s_init(int sample_rate);

/* ★★★ 2026-11-07 linit「音频空闲必须彻底休眠」★★★
 * 无 PCM 数据满 AUDIO_IDLE_OFF_MS 就：关功放 → disable I2S 通道(DMA 停、MCLK/BCLK 停)。
 * 下次 feed_pcm 一进来就 notify 唤醒本任务, 由 ensure_out_active() 重新打开,
 * 不需要重启任务 ⇒ 不会出现"关了就再也开不回来"的老问题(2026-10-25 的坑)。 */
#define AUDIO_IDLE_OFF_MS  2500u

static void audio_out_idle_off(void)
{
    pa_enable(false);
    s_amp_on = false;
    if (s_i2s_mutex && xSemaphoreTake(s_i2s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        /* ★★★ 2026-11-23 修「录音第二次起 全程无信号 / 0 字节」★★★
         *   原来只保护了 RX：`if (!s_mic_active) i2s_rx_disable();`，TX 是**无条件**停的。
         *   但本板 I2S 是 full-duplex：TX 作 master 出 BCLK/WS，RX 被 IDF 切成 slave
         *   ⇒ TX 一 disable，RX 就没有位时钟，i2s_channel_read() 永远拿不到 DMA 块。
         *   实测指纹：录音 900ms 后打印「音频空闲 2500 ms: 已关功放 + 停 I2S 时钟」，
         *   随后 rec_task 每次读都返回 0，文件只有 44 字节头、日志「0 B / 全程无信号」。
         *   因此录音期间**两个通道都不能停**（TX 是时钟源，比 RX 更不能停）。 */
        if (!s_mic_active) {
            i2s_tx_disable();
            i2s_rx_disable();
        }
        xSemaphoreGive(s_i2s_mutex);
    }
    ESP_LOGI(TAG, "音频空闲 %u ms: 已关功放 + 停 I2S 时钟%s",
             (unsigned)AUDIO_IDLE_OFF_MS, s_mic_active ? " (录音中: 保留 I2S 时钟)" : "");
}

/* CPU1 音频输出任务: 从环形缓冲读 PCM → I2S */
static void audio_out_task(void *arg) {
    (void)arg;
    /* ★ 2026-10-25: 每次最多取 1024 帧(≈23ms) —— 与 sdl_aud 的回调块长对齐。
     *   原来只取 256 帧(5.8ms), 意味着每秒要成功调度 172 次才能喂满,
     *   任务被抢占几次就直接欠载(实测 play 掉到 705Hz)。
     *   local_buf 从栈上 512 改成静态 2048, 避免撑爆 audio_out 的栈。 */
    /* ★ 2026-11-01 linit 内存规则：4KB 中间缓冲放 PSRAM。
     *   它只是 CPU memcpy 的源缓冲（拷进 s_pcm_ring），不参与 DMA 描述符，可以进 PSRAM。 */
    static EXT_RAM_BSS_ATTR int16_t local_buf[2048];
    bool primed = false;
    uint32_t underruns = 0;
    uint32_t iter = 0;
    uint32_t frames_total = 0;
#if LIN_DIAG_AUDIO
    uint32_t last_frames = 0;
    uint32_t last_log_ms = 0;
#endif

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
            /* ★ 2026-10-29 修: 这里原来写的是 `PCM_RING_SIZE / 2 - tail + head`,
             *   比写端 feed_pcm 的 `PCM_RING_SIZE - tail + head` 少算一半 —— 
             *   一旦 head 回绕到 tail 之前, 读端就只认为有一半数据可读,
             *   实际可读量被腰斩 ⇒ 频繁欠载/断音 (MP3 不走这条环形缓冲, 故不受影响).
             *   两端口径必须完全一致, 都按样本(int16)计、以 PCM_RING_SIZE 为模. */
            avail = (PCM_RING_SIZE - tail + head) / 2;
        }

        if (avail == 0) {
            /* 环形缓冲短暂空: 项目 tick 为 1kHz, vTaskDelay(1)=1ms 且会让出 CPU.
             * 不能忙等 (优先级 8 会饿死同核 IDLE, 触发任务看门狗). */
            underruns++;
            uint32_t unow = esp_log_timestamp();
            if (unow - s_last_underrun_ms > 100) {
                /* 只记录"新一轮"欠载, 避免连续欠载刷屏;
                 * gap 大 = 偶发欠载, gap 小 = 持续欠载. */
                ESP_LOGD(TAG, "I2S underrun: 距上次欠载 %u ms (avail=0)",
                         (unsigned)(unow - s_last_underrun_ms));
            }
            s_last_underrun_ms = unow;
            /* ★★★ 2026-11-07 linit「音频空闲彻底休眠」★★★
             * 这里原来是 `vTaskDelay(1)` 无限空转: 只要有人 feed_pcm 过一次,
             * 任务就以 prio=8/CPU1 每秒被唤醒 1000 次, 并且功放与 I2S 位时钟一直开着
             * (旧注释自己也写了"空闲时 I2S 位时钟继续跑, 多几 mA")。
             * 实测桌面静置时 audio_out 任务不存在(没放过声音), 但**玩过一次游戏之后
             * 它就永久留着** —— 这正是"用一会儿就烫"的一大来源。
             * 现在: 静音满 2.5 s 就关功放 + 停 I2S; 之后用**通知**等待而不是 1ms 轮询,
             * 有数据时 feed_pcm 立刻 notify 唤醒, 由 ensure_out_active() 原路恢复。 */
            if (s_amp_on && (unow - s_last_data_ms) >= AUDIO_IDLE_OFF_MS)
                audio_out_idle_off();
            if (!s_amp_on) {
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
                continue;
            }
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

        if (avail > 1024) avail = 1024;
        frames_total += avail;
        /* V1.7.x: 有数据 → 打时间戳; 若刚被"空闲静音"关过, 这里恢复功放 + I2S 时钟.
         * (必须在写 I2S 之前恢复, 否则通道禁用时 i2s_channel_write 会超时报错) */
        s_last_data_ms = esp_log_timestamp();
        if (!s_amp_on) {
            ensure_out_active();      /* 启 I2S 时钟 → 再开功放 */
            ESP_LOGI(TAG, "恢复出声: 开功放 + 启 I2S 时钟");
        }

        if ((++iter % 400) == 0) {
#if LIN_DIAG_AUDIO
            uint32_t now_ms = esp_log_timestamp();
            uint32_t delta_ms = now_ms - last_log_ms;
            uint32_t play_hz = (delta_ms > 0) ? ((frames_total - last_frames) * 1000u / delta_ms) : 0;
            ESP_LOGI(TAG, "PCM ring: avail=%u underruns=%u want=%dHz i2s=%dHz play=%uHz",
                     (unsigned)avail, (unsigned)underruns,
                     (int)s_desired_sample_rate, audio_player_get_i2s_rate(),
                     (unsigned)play_hz);
            last_frames = frames_total;
            last_log_ms = now_ms;
#endif
            underruns = 0;
        }

        /* 从环形缓冲复制到本地缓冲.
         * 两平台统一立体声: 每帧 L+R 两个样本. NS4168 虽然是单声道功放,
         * 但 I2S 配置为 STEREO, 数据复制到 L/R, 功放内部按 CTRL 取一路. */
        uint32_t sample_count = avail * 2;
        /* ★ 2026-10-25 性能: 原来每样本一次 % 取模(软件除法) → 最多两段 memcpy.
         *   环形缓冲跨尾部时拆成"尾部→末尾" + "开头→剩余" 两段, 语义完全等价。 */
        {
            uint32_t first = PCM_RING_SIZE - tail;
            if (first > sample_count) first = sample_count;
            memcpy(local_buf, &s_pcm_ring[tail], (size_t)first * sizeof(int16_t));
            if (sample_count > first) {
                memcpy(&local_buf[first], &s_pcm_ring[0],
                       (size_t)(sample_count - first) * sizeof(int16_t));
            }
        }
        tail = (tail + sample_count) % PCM_RING_SIZE;
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
        
        /* 应用音量 (★ 2026-10-25 性能: 改 Q15 定点乘移位, 免每样本 int16↔float 转换) */
        int vol_q15 = s_volume * 32768 / 100;
        /* [V2.x] 最小缩放下限: 非静音时最低 10%, 防极低百分比仍无声 */
        if (s_volume > 0 && vol_q15 < 3277) vol_q15 = 3277;
        if (vol_q15 < 32768) {
            for (uint32_t i = 0; i < sample_count; i++) {
                local_buf[i] = (int16_t)(((int32_t)local_buf[i] * vol_q15) >> 15);
            }
        }
        
        /* V1.0.71: 喂振幅表 (游戏/音效 PCM 路径) */
        audio_meter_feed(local_buf, sample_count);
        
        /* ★★★ 2026-10-30 修「录音回放快约 2 倍 / 语音被快进」：
         *   原来这里是**一次** i2s_channel_write(…, 20ms) 且**完全不看返回值**。
         *   DMA 只有 3×576 = 1728 帧，而一次要写 1024 帧：稳态下经常写不满，
         *   超时后只接受了 written 字节，**剩下的被直接丢弃**，但 tail 已经前移 ⇒
         *   音频被静默快进，总时长明显短于文件长度（实测 2016ms 只跑了 1151ms）。
         *   正确做法：分段把剩余字节写完，写不动才放弃这一块（并记警告）。 */
        size_t bytes = sample_count * sizeof(int16_t);
        size_t sent_bytes = 0;
        int guard = 0;
        while (sent_bytes < bytes && guard++ < 12) {
            size_t written = 0;
            esp_err_t wr = i2s_channel_write(s_tx_chan, (const uint8_t *)local_buf + sent_bytes,
                                             bytes - sent_bytes, &written, pdMS_TO_TICKS(100));
            sent_bytes += written;
            if (wr != ESP_OK && written == 0) break;   /* 真写不动了 */
        }
        if (sent_bytes < bytes) {
            static uint32_t drop_log_ms = 0;
            uint32_t nowm = esp_log_timestamp();
            if (nowm - drop_log_ms > 1000) {           /* 限流, 避免刷屏 */
                drop_log_ms = nowm;
                ESP_LOGW(TAG, "I2S 写入不完整: 丢弃 %u/%u 字节",
                         (unsigned)(bytes - sent_bytes), (unsigned)bytes);
            }
        }
    }
    
    s_audio_out_task = NULL;
    /* ★★★ 2026-11-22【真·根因之一】必须 vTaskDeleteWithCaps，不能 vTaskDelete ★★★
     * 本任务由 xTaskCreatePinnedToCoreWithCaps 创建，它内部走的是
     * xTaskCreateStaticPinnedToCore（TCB/栈是用户提供的 static 内存）。
     * FreeRTOS 的 prvDeleteTCB 只在 ucStaticallyAllocated == DYNAMIC 时才 free；
     * 对 STATIC_STACK_AND_TCB 只做一句 configASSERT，**一个字节都不释放**。
     * ⇒ 用普通 vTaskDelete(NULL) 自杀 = **每次 audio_out 启停泄漏 24KB PSRAM + TCB(内部 RAM)**，
     *   永远不还。IDF 源码 idf_additions.c:163-168 明确写了"this task will never be
     *   rescheduled ... Therefore, it will leak memory"。
     * ⛔ 不要再改回 vTaskDelete(NULL)。项目内 sdl_audio.c/pal_entry.c 已是同样写法。 */
    vTaskDeleteWithCaps(NULL);
}

/* ★★★ 2026-10-27 并发修复:
 *   原来 audio_out_start/stop 完全没有互斥, 且 start 只看 s_audio_out_running 标志。
 *   而任务当年用的是**静态 TCB + 静态栈**(那两个静态缓冲已在 2026-11-22 删除):
 *     · 上一个任务 500ms 内没退完(stop 的超时) ⇒ 句柄仍非 NULL
 *     · 此时另一核上的 sdl_aud 调 feed_pcm → audio_out_start → 再创建一次
 *     ⇒ 同一个 TCB 被插入 ready list 两次 ⇒ FreeRTOS 链表/堆被写坏
 *   实测崩溃: audio_task → feed_pcm → audio_out_start → xTaskCreateStaticPinnedToCore
 *             → prvAddNewTaskToReadyList  StoreProhibited(EXCVADDR 0x4)
 *   修法: 1) start/stop 用互斥锁串行化;  2) 句柄非空就绝不再建。 */
static SemaphoreHandle_t s_ao_mutex = NULL;
/* ★ 必须用**超时**取锁: audio_out_stop 持锁期间要等任务退出(最长 500ms),
 *   而另一核的 feed_pcm 若同时持着 I2S 锁进来等这把锁 ⇒ 两边互等会卡死主循环
 *   (实测: 日志在 33s 处直接停止, 无 panic、无重启 = 死锁)。
 *   拿不到锁就跳过本次 start/stop, 下一帧再来, 绝不阻塞调用方。 */
static bool ao_lock(void)
{
    if (!s_ao_mutex) s_ao_mutex = xSemaphoreCreateMutex();
    if (!s_ao_mutex) return true;                     /* 建不出锁就别卡死功能 */
    return xSemaphoreTake(s_ao_mutex, pdMS_TO_TICKS(100)) == pdTRUE;
}
static void ao_unlock(void)
{
    if (s_ao_mutex) xSemaphoreGive(s_ao_mutex);
}

/* 启动音频输出任务 (游戏开始时调用) */
static void audio_out_start(void) {
    if (!ao_lock()) return;
    /* 句柄还在 ⇒ 任务仍然存活(哪怕 running 标志被清过): 绝不能用同一个静态 TCB 重建 */
    if (s_audio_out_running || s_audio_out_task) {
        s_audio_out_running = true;
        ao_unlock();
        return;
    }
    if (!s_pcm_ring) {
        s_pcm_ring = (int16_t *)heap_caps_calloc(PCM_RING_SIZE, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_pcm_ring) {
            /* V1.0.46: 之前 calloc(PCM_RING_SIZE,1) 只分配 16KB=8192 个 int16,
             * 但代码按 16384 个元素索引 (head 最大 16382), 缓冲写满时越界 16KB
             * → 破坏相邻堆块 → 按键时 BT 栈 malloc 撞到损坏块崩溃. */
            s_pcm_ring = (int16_t *)calloc(PCM_RING_SIZE, sizeof(int16_t));
        }
        if (!s_pcm_ring) { ao_unlock(); return; }
    }
    s_ring_head = 0;
    s_ring_tail = 0;
    s_audio_out_running = true;
    
    /* 采样率先行(游戏 PCM 的采样率由引擎决定), 之后统一开输出。
     * ★ 2026-11-22: 原来这里是 es8311_start() + pa_enable(true) 手拼,
     *   **漏掉音量下发** ⇒ 一次关断之后再进游戏/放音效就没声音。
     *   现在交给 audio_output_begin(): codec 自检上电 + 下发音量 + 最后开功放。 */
    if (s_i2s_mutex) xSemaphoreTake(s_i2s_mutex, portMAX_DELAY);
    es8311_set_sample_rate(s_desired_sample_rate);
    if (s_i2s_mutex) xSemaphoreGive(s_i2s_mutex);
    audio_output_begin();
    s_last_data_ms = esp_log_timestamp();
    
    /* ★ 2026-11-22: 栈不再由本函数静态分配 —— 交给 xTaskCreatePinnedToCoreWithCaps
     *   连同 TCB 一起从堆动态申请（下面）。这样每次 start 都是新栈，
     *   旧任务即使还没被 idle 回收也绝不重叠。 */
    /* ★ 2026-11-22 根治：动态创建（原为 xTaskCreateStaticPinnedToCore +
     *   文件作用域 static TCB/栈）。游戏里 audio_out 会反复 start/stop，
     *   静态复用 TCB 就是"旧 TCB 还在 terminated 链表、新任务把它就地重写"
     *   ⇒ FreeRTOS 链表串坏 ⇒ 之后任意 malloc/printf 撞坏内存 abort。
     *   动态创建后新旧任务物理隔离，任何时序都安全。
     *   栈优先 PSRAM（24KB，内部 DRAM 连续块紧张）。 */
    BaseType_t aocr = xTaskCreatePinnedToCoreWithCaps(
                        audio_out_task, "audio_out", PCM_OUT_STACK_WORDS, NULL, 8,
                        &s_audio_out_task, 1,
                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (aocr != pdPASS) {
        aocr = xTaskCreatePinnedToCoreWithCaps(
                        audio_out_task, "audio_out", PCM_OUT_STACK_WORDS, NULL, 8,
                        &s_audio_out_task, 1, MALLOC_CAP_8BIT);
    }
    if (aocr != pdPASS || s_audio_out_task == NULL) {
        ESP_LOGE(TAG, "audio_out 任务创建失败(%d) ⇒ 本局无游戏音频", (int)aocr);
        s_audio_out_task = NULL;
        pa_enable(false);
        s_amp_on = false;
        s_audio_out_running = false;
    }
    ao_unlock();
}

/* 停止音频输出任务。
 * ★★★ 2026-11-22【真·根因之二】取消强制删除 ★★★
 * 原来超时后走 `vTaskDelete(s_audio_out_task)`，并且紧跟一句
 *     if (xSemaphoreGetMutexHolder(s_i2s_mutex) == s_audio_out_task) xSemaphoreGive(s_i2s_mutex);
 * 这两句组合起来会破坏内部堆，机制如下（FreeRTOS 源码级）：
 *   1) audio_out_task 在 ESP_LOGW/ESP_LOGI 期间持有 **全局 stdio 递归锁**；
 *      在 i2s_channel_write 期间持有 s_i2s_mutex。被 vTaskDelete 硬切 ⇒ 两把锁的
 *      xMutexHolder 指向一个已被删除的 TCB，且**永远不会再归还**。
 *   2) 从**另一个任务**去 xSemaphoreGive() 别人的 mutex ⇒ 走
 *      prvCopyDataToQueue → vTaskPriorityDisinherit(xMutexHolder)，
 *      对那个悬空/已删的 TCB 写 uxPriority / 遍历 xEventListItem
 *      ⇒ 写坏内部堆里的 TCB 块。
 *   3) 内部堆一坏，newlib 的 stdio 递归锁（也是 heap 上的 Queue_t）就会被当成
 *      队列复用，于是下一次 printf 命中
 *      assert(pxQueue->uxItemSize == 0) → panic_abort(IllegalInstruction)。
 *   这正是"崩在 main 的 vprintf、而 main 什么都没干"的指纹。
 * ⛔ 任何情况下都不再强制删除 audio_out；等不到就返回 false，让调用方放弃本次操作。
 * ⛔ 也绝不再从外部 give 别人的 mutex。
 * 返回 true = 任务已消失（I2S 可以安全交给别人）；false = 还活着，调用方不得抢 I2S。 */
static bool audio_out_stop(void) {
    if (!ao_lock()) return false;
    if (!s_audio_out_running && !s_audio_out_task) { ao_unlock(); return true; }
    s_audio_out_running = false;
    /* 任务每轮 while(s_audio_out_running) 都会检查：最坏情况卡在
     * i2s_channel_write(100ms×最多12段) + ulTaskNotifyTake(200ms) ⇒ 2s 足够。
     * 100ms 太短、500ms 也偏紧，这里给 2000ms。 */
    for (int i = 0; i < 200 && s_audio_out_task; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_audio_out_task) {
        /* 不删、不抢锁、不动 I2S：它还在写同一个通道，此时 disable 通道就是野指针。 */
        ESP_LOGE(TAG, "audio_out 2000ms 未退 ⇒ 本次不做 I2S 交接（绝不强杀，"
                      "强杀会把 stdio 递归锁/I2S 锁的持有者留成悬空 TCB 并写坏内部堆）");
        ao_unlock();
        return false;
    }
    pa_enable(false);
    s_amp_on = false;   /* V1.7.x: 与空闲静音状态同步 (避免下一轮误判已开) */

    /* ★★★ 2026-11-02 linit「音频全部关闭」：输出停掉后把 I2S 通道也 disable。
     *   原来只关功放、通道一直 enable ⇒ MCLK/BCLK 持续输出、ES8311/ES7210 一直"有钟
     *   无数据"地挂着（实测空闲时 tx=1 rx=1），白白耗十几 mA。
     *   通道本身保留（不 del），下次播放由 ensure_out_active() 重新 enable，
     *   不影响采样率重配与 MP3 直写路径。 */
    if (s_i2s_mutex && xSemaphoreTake(s_i2s_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        /* ★ 2026-11-23: 与 audio_out_idle_off 同因同修 —— 录音中 TX 是 RX(slave) 的
         *   时钟源，停 TX 等于让麦克风采集直接死掉（实测「第二次录音 0 字节」）。 */
        if (!s_mic_active) {
            i2s_tx_disable();
            i2s_rx_disable();
        }
        xSemaphoreGive(s_i2s_mutex);
        ESP_LOGD(TAG, "输出已停: I2S 通道 disable (编解码器不再有时钟)");
    }
    ao_unlock();
    return true;
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
        i2s_tx_disable();
        i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
        esp_err_t ret = i2s_channel_reconfig_std_clock(s_tx_chan, &clk_cfg);
        if (ret != ESP_OK) {
            ESP_LOGD(TAG, "I2S 重设采样率失败: %s", esp_err_to_name(ret));
        }
        i2s_tx_enable();
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
    chan_cfg.dma_desc_num = 3;   /* 2026-10-16: 6→4 (省 9.2KB); 2026-10-24: 4→3 (再省 4.6KB)
                                  * 3×576=1728 帧 ≈ 36ms@24kHz, 仍能吸收模拟器帧抖动 */
    chan_cfg.dma_frame_num = 576;
    chan_cfg.auto_clear = true;
    /* ★ 2026-10-31 修「标准版/无麦克风板播 MP3 没声音」：
     *   原来无条件建 RX 通道。标准扩展版 din=-1（无麦克风），建 RX 会让 I2S0
     *   进入 full-duplex，ESP-IDF 随即把 RX 从 master 切成 slave 并打印
     *   "the rx channel on I2S0 is switched from master to slave for full-duplex mode"，
     *   紧接着首帧重配时钟时 i2s_channel_enable() 报
     *   "the channel has already enabled or not initialized" ⇒ BCLK/WS 时钟状态被切乱，
     *   表现为：解码在跑、写入不报错、功放也开着，但喇叭一点声音都没有。
     *   无麦克风的板子（din<0）只建 TX 单工，时钟不会被 RX 抢走。
     *   有麦克风的板（小智 din=21 / 微雪 din=10）行为完全不变。 */
    esp_err_t ret = i2s_new_channel(&chan_cfg, &s_tx_chan,
                                    (s_din >= 0) ? &s_rx_chan : NULL);
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "I2S 创建通道失败: %s", esp_err_to_name(ret));
        s_tx_chan = NULL;
        s_rx_chan = NULL;
        return -1;
    }
    if (s_rx_chan == NULL) {
        ESP_LOGI(TAG, "无麦克风板型(din=%d): I2S 走单 TX, 不进 full-duplex", (int)s_din);
    }

    /* 槽配置按平台区分:
     *  - NS4168 (数字单声道功放): 使用 STEREO 双槽, 单声道数据复制到 L/R.
     *    [关键] ESP-IDF 5 在 ESP32-S3 上 MONO 模式有已知 bug: 采样会变成
     *    [有效,0,有效,0...] 零交错, 产生金属/雪花噪声 (GitHub: mcandongo/ns4168-esp32s3-idf5-speaker-fix).
     *    正确做法是用 STEREO 模式 + Philips 格式 + 16-bit slot, 数据复制到左右声道.
     *  - ES8311 (官方板): 标准双声道. */
    i2s_std_slot_config_t slot_cfg;
    if (s_use_ns4168_ctrl) {
        /* 用 Philips 标准 I2S 配置 (16-bit, STEREO), 与社区验证方案一致 */
        slot_cfg = (i2s_std_slot_config_t)I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    } else {
        slot_cfg = (i2s_std_slot_config_t)I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    }
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = slot_cfg,
        .gpio_cfg = {
            .bclk = s_bclk,
            .ws   = s_ws,
            .dout = s_dout,
            /* NS4168 不需要 MCLK(芯片无此脚): 关闭 256fs(≈11MHz)高频输出,
             * 消除空闲高频时钟对 D类功放的 EMI 辐射. ES8311 平台保留 MCLK. */
            .mclk = s_use_ns4168_ctrl ? I2S_GPIO_UNUSED : s_mclk,
            .din  = s_din,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    ret = i2s_channel_init_std_mode(s_tx_chan, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "I2S 初始化失败: %s", esp_err_to_name(ret));
        i2s_del_channel(s_tx_chan);
        if (s_rx_chan) i2s_del_channel(s_rx_chan);   /* 无麦克风板型这里是 NULL */
        s_tx_chan = NULL;
        s_rx_chan = NULL;
        return -1;
    }

    if (s_rx_chan) {                      /* 无麦克风板型没有 RX 通道, 跳过 */
        ret = i2s_channel_init_std_mode(s_rx_chan, &std_cfg);
        if (ret != ESP_OK) {
            ESP_LOGD(TAG, "I2S RX 初始化失败: %s", esp_err_to_name(ret));
            i2s_del_channel(s_tx_chan);
            i2s_del_channel(s_rx_chan);
            s_tx_chan = NULL;
            s_rx_chan = NULL;
            return -1;
        }
    }

    ret = i2s_tx_enable();
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "I2S 使能失败: %s", esp_err_to_name(ret));
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
        i2s_tx_disable();
        i2s_del_channel(s_tx_chan);
        s_tx_chan = NULL;
        s_tx_on   = false;
    }
    if (s_rx_chan) {
        i2s_rx_disable();
        i2s_del_channel(s_rx_chan);
        s_rx_chan = NULL;
        s_rx_on   = false;
    }
    if (s_i2s_mutex) xSemaphoreGive(s_i2s_mutex);
}

/* ============ 麦克风输入 (ES7210 双麦克风阵列) ============ */

int audio_player_mic_start(int sample_rate) {
    if (s_mic_active) return 0;
    if (s_i2s_mutex) xSemaphoreTake(s_i2s_mutex, portMAX_DELAY);
    int rc = i2s_init_locked(sample_rate);
    if (rc == 0 && s_rx_chan) {
        /* ★ 2026-11-23: 采样率已一致时 i2s_init_locked() 直接 return 0、**不碰通道状态**，
         *   而空闲省电(audio_out_idle_off)可能刚把 TX 停了。full-duplex 下 RX 是 slave，
         *   它的 BCLK/WS 由 TX(master) 提供 ⇒ 不把 TX 拉回来就一个样点都收不到。
         *   这里显式补齐：先 TX(时钟源) 再 RX。 */
        (void)i2s_tx_enable();
        esp_err_t ret = i2s_rx_enable();
        if (ret != ESP_OK) {
            ESP_LOGD(TAG, "I2S RX 使能失败: %s", esp_err_to_name(ret));
            rc = -1;
        }
    } else {
        rc = -1;
    }
    if (s_i2s_mutex) xSemaphoreGive(s_i2s_mutex);
    if (rc != 0) return -1;

    /* V1.1.0: 麦克风支持两种 ADC:
     *  - ES7210(I2C 四通道) 存在 → 走 ES7210 配置
     *  - 无 ES7210 → INMP441 数字麦克风直读 (纯 I2S, 无需 I2C). 均可用. */
    if (es7210_init(I2C_NUM_0) != 0) {
        ESP_LOGI(TAG, "无 ES7210, 麦克风按 INMP441 直读 I2S(GPIO10)");
    }
    s_mic_active = true;
    ESP_LOGI(TAG, "麦克风已启动 (%d Hz)", sample_rate);
    return 0;
}

void audio_player_mic_stop(void) {
    if (!s_mic_active) return;
    es7210_deinit();
    if (s_i2s_mutex) xSemaphoreTake(s_i2s_mutex, portMAX_DELAY);
    i2s_rx_disable();
    if (s_i2s_mutex) xSemaphoreGive(s_i2s_mutex);
    s_mic_active = false;
    ESP_LOGI(TAG, "麦克风已停止");
}

int audio_player_mic_read(int16_t *buf, int frames, int timeout_ms) {
    if (!s_mic_active || !s_rx_chan || !buf || frames <= 0) return -1;
    size_t bytes = (size_t)frames * 2 * sizeof(int16_t);   /* 立体声 L/R */
    size_t got = 0;
    esp_err_t ret = i2s_channel_read(s_rx_chan, buf, bytes, &got, timeout_ms);
    /* ⛔ 2026-10-14 关键修复: 不能把非 ESP_OK 一律当"没读到"。
     *   IDF 的 i2s_channel_read() 是**按整块 DMA 缓冲**交付的: 它循环取 msg_queue 里的
     *   DMA 块往 dest 里 memcpy, 只有凑满 size 才继续; **凑不满就带着已拷贝的字节数
     *   返回 ESP_ERR_TIMEOUT**(见 esp_driver_i2s/i2s_common.c:1413-1442)。
     *   旧写法 `if (ret != ESP_OK) return -1;` 会把已经拷进 buf 的数据**丢掉**,
     *   于是 2048 帧(128ms)这种超过单块 DMA 缓冲(dma_frame_num=576)的请求**永远读不满**
     *   ⇒ 频谱仪"音频"页一直没数据 / 显示"未就绪"。 */
    if (ret != ESP_OK && got == 0 && ret != ESP_ERR_TIMEOUT) return -1;
    return (int)(got / (2 * sizeof(int16_t)));
}

bool audio_player_mic_active(void) {
    return s_mic_active;
}

/* ============ 功放芯片使能控制 (PA_EN / 模块 CTRL) ============
 * 小智 ESP32-S3-AI-2E 模块: 功放 LPA2103A 的 SD(使能) 脚在**模块内部**接 GPIO46,
 * 高=工作 / 低=关断。官方板 AAXS2005 的 PA_EN、自制板 NS4168 的 CTRL 也在同一根脚
 * (同为高=开) ⇒ **GPIO46 = 喇叭出声的唯一总闸**。
 *
 * ⛔ GPIO46 是 ESP32-S3 的 **strapping 脚**(与 GPIO0 一起决定启动模式)。
 *   模块靠外部弱下拉保证开机瞬间为低 ⇒ 这里**绝不允许开内部上拉**, 只能推挽输出、
 *   由固件显式拉低; 拉高必须发生在 ROM 采样完 strapping 之后(即 app 阶段, 天然满足)。
 *
 * ★ 消爆音时序 —— D 类功放的通病: 输入还没稳定就开使能 → "噗"一声。
 *     开: I2S 时钟先跑起来 → 等 PA_ON_DELAY_MS(编解码器输出建立) → 拉高使能
 *     关: 先拉低使能(静音功放) → 等 PA_OFF_DELAY_MS(功放真的关断) → 再由调用方停 I2S
 *   ⚠ 这两个等待**绝不能拿在 s_i2s_mutex 里做** —— 那把锁是所有音频路径(MP3/游戏 PCM/
 *     按键音)共用的, 卡 20~30ms 会让别的路径一起等。所有调用点都已把 pa 操作挪到锁外。
 */
#define PA_ON_DELAY_MS   20
#define PA_OFF_DELAY_MS  30

static bool s_pa_on    = false;   /* 软件认定的状态 (GPIO 实际电平的镜像) */
static bool s_pa_ready = false;   /* 引脚是否已配置 */

void audio_pa_init(void)
{
    if (s_pa_ready) return;
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << s_pa),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,     /* ⛔ strapping 脚, 绝不能上拉 */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_hold_dis(s_pa);            /* 深睡可能 hold 过 → 先解, 否则写不动电平 */
    gpio_set_level(s_pa, 0);
    /* ★★★ 2026-11-07 linit「自动浅睡后仍发热」★★★
     *   CONFIG_PM_SLP_DISABLE_GPIO=y 会在**每次自动浅睡**时释放所有 GPIO 焊盘
     *   (省 200~300µA)。GPIO46 是 NS4168 的 CTRL(VON), 悬空后可能被模块上拉成
     *   "工作"态 —— 于是每 16ms 一次浅睡周期里功放被反复点亮, 表现为"没放声音也烫"。
     *   用官方 gpio_sleep_sel_dis() 让这一脚在睡眠期间**保持当前电平(低=关断)**,
     *   其余引脚照常释放, 不影响 GPIO 省电。 */
    gpio_sleep_sel_dis((gpio_num_t)s_pa);
    s_pa_on    = false;
    s_pa_ready = true;
    ESP_LOGI(TAG, "功放使能脚就绪: GPIO%d = 输出已拉低 (功放关)", (int)s_pa);
}

void audio_pa_set(bool on)
{
    if (!s_pa_ready) audio_pa_init();
    if (on == s_pa_on) return;        /* 同状态是空操作 (也避免重复 20/30ms 等待) */
    gpio_hold_dis(s_pa);            /* 万一上一轮 force_off 锁过 */
    if (on) {
        vTaskDelay(pdMS_TO_TICKS(PA_ON_DELAY_MS));   /* 等编解码器输出建立 */
        gpio_set_level(s_pa, 1);
        s_pa_on = true;
        ESP_LOGI(TAG, "功放使能: GPIO%d=1 (出声)", (int)s_pa);
    } else {
        gpio_set_level(s_pa, 0);                   /* 先静音功放 */
        s_pa_on = false;
        /* ★ 2026-10-31 修「播 MP3 没声音」：s_amp_on 表示"输出通路活跃"，
         *   必须与功放实际状态同步。原来只有 2 处 pa_enable(false) 顺带清了它，
         *   其余 4 处（MP3 结束 / 任务收尾 / init / 关机流程）只拉低引脚不清标志
         *   ⇒ ensure_out_active() 第一行 `if (s_amp_on) return;` 永久短路，
         *   功放再也不会被打开：表现就是 MP3 解码/进度全正常、就是没声，
         *   而游戏走 PCM 路径会无条件 pa_enable(true)，所以"游戏有声、MP3 无声"。
         *   在这里统一同步，任何调用点都不用再操心态。 */
        s_amp_on = false;
        vTaskDelay(pdMS_TO_TICKS(PA_OFF_DELAY_MS));  /* 等它真的关断, 调用方再停 I2S */
        ESP_LOGI(TAG, "功放使能: GPIO%d=0 (静音)", (int)s_pa);
    }
}

bool audio_pa_is_on(void)
{
    return s_pa_on;
}

void audio_pa_force_off(void)
{
    audio_pa_init();
    gpio_set_level(s_pa, 0);
    gpio_hold_en(s_pa);              /* ★ 深睡时 GPIO 默认变高阻; 若模块侧 SD 有外部上拉,
                                        *   功放会在整个睡眠期间保持工作 → 白耗几十 mA。
                                        *   hold 住低电平才真正"关机后不耗电"。 */
    gpio_deep_sleep_hold_en();
    s_pa_on = false;
    ESP_LOGI(TAG, "功放已强制关断并锁定低电平 (深睡不耗电)");
}

/* 老调用点统一走 audio_pa_set (带消爆音时序) */
static void pa_enable(bool on) { audio_pa_set(on); }

/* ★ 2026-10-20: 空闲静音会把 I2S 通道也关掉(消底噪), 但 MP3 是**自己直接写 I2S**,
 *   不走 PCM 环形缓冲 ⇒ 播放 300ms 后通道被关, 之后每次 i2s_channel_write 都报
 *   "The channel is not enabled", 表现就是无声 + 每隔一个缓冲"咔"一下。
 *   写之前统一走这个函数把 通道+功放 拉起来。 */

/* ★★★ 2026-11-22 linit「音频控制归纳为一个模块」★★★
 *   【问题】"出声"这件事原来散在 4 个调用点各自拼装:
 *     audio_out_start():  es8311_start() + set_sample_rate() + pa_enable(true)
 *     audio_play_task():  i2s_init() + es8311_start() + pa_enable(true) + es8311_set_volume()
 *     ensure_out_active(): i2s_tx_enable() + pa_enable(true)
 *     feed_pcm():          audio_player_init() (+ 什么都不写)
 *   而**音量寄存器(REG32)只有 MP3 那条路径写**。其余三条一旦走到, codec 的音量
 *   就停在上一次 es8311_shutdown() 写进去的 0x00(= -95.5dB 静音),
 *   于是"初始化全成功、I2S 有数据、功放也开着, 就是没声音",
 *   用户把音量条动一下(触发 es8311_set_volume)立刻恢复 —— 这就是群里反馈的现象。
 *
 *   【改法】收敛成一个函数: 谁要出声就调它, 别自己拼。它负责
 *     ① I2S TX 上钟  ② codec 上电自检/重配  ③ **下发音量**  ④ 最后开功放(消爆音)
 *   顺序固定为"先有信号、再开功放", 报错/失败也不留半开状态。
 *   幂等: 已经全开则立即返回(MP3 每帧都会调, 不能每次走 I2C)。 */
static void audio_output_begin(void)
{
    /* 判定"通路已就绪":
     *   正常态 = 功放开 + 通道开;
     *   静音态 = 通道开 + 功放**故意关**(s_muted 时不开功放, 免得白白耗电+底噪)。
     * ★ MP3 每帧都会调本函数, 早退必须覆盖静音态, 否则每帧都走一遍 I2C。 */
    if (s_amp_on && s_tx_on && (s_pa_on || s_muted)) return;

    if (s_i2s_mutex) xSemaphoreTake(s_i2s_mutex, portMAX_DELAY);
    esp_err_t tx_err = i2s_tx_enable();
    if (s_i2s_mutex) xSemaphoreGive(s_i2s_mutex);

    /* ★ 2026-11-24 容错: TX 没真正启起来 ⇒ 立刻退出, **不许**走到下面置 s_amp_on。
     *   原来丢返回值 + 无条件 s_amp_on=true + 先开功放, 一旦 i2s_channel_enable()
     *   失败(驱动异常/句柄失效), 就变成"软件说通路就绪、硬件没时钟":
     *   功放白开(底噪+耗电)却无声, 且 s_amp_on 为真会让各调用点误判已恢复。
     *   现在 s_amp_on 保持 false ⇒ 下一次写帧/喂数据自然重试, 无需额外状态机自愈。
     *   顺手把功放关断: 没时钟时开功放只会放大底噪, 也保持 s_amp_on 与 s_pa_on 一致。 */
    if (tx_err != ESP_OK) {
        ESP_LOGW(TAG, "I2S TX 启用失败: %s ⇒ 不标记通路就绪, 保持功放关断",
                 esp_err_to_name(tx_err));
        pa_enable(false);
        return;
    }

    /* codec: 掉电就重配, 已就绪是空操作 */
    es8311_ensure_running();
    /* ★ 唯一音量下发点: 静音写 0, 否则写当前百分比。
     *   ⛔ 任何"开输出"的路径都必须经过这里, 不许再只 start 不下发音量。 */
    es8311_set_volume(s_muted ? 0 : s_volume);

    if (!s_muted) {
        pa_enable(true);    /* 最后开功放: 此刻信号链已稳定, 不炸音 */
        s_amp_on = true;
    } else {
        /* 静音播放: 通道照开(否则 i2s_channel_write 会报错丢帧、进度乱),
         * 功放保持关断 —— 完全无声且不耗功放电流。 */
        s_amp_on = true;
        s_pa_on  = false;
    }
}

/* 兼容旧名: 语义完全等价(FORCE 时代留下的名字), 新代码请用 audio_output_begin */
static void ensure_out_active(void) { audio_output_begin(); }

/* ============ 多帧扫描估算 ============ */
/* ⚠ 只能在 audio_play_task（64KB 栈）里调用：内部经 mp3dec_decode_frame() 引入
 *   16608B 栈帧，main 任务栈只有 10240B，调用即越界。参见 audio_play_task 开头注释。 */
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
        ESP_LOGD(TAG, "\u591A\u5E27\u626B\u63CF\u672A\u627E\u5230\u6709\u6548\u5E27");
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
/* ★ 2026-11-22: 等播放任务**自己**退出的上限。play / stop / feed_pcm 三处共用。
 *   ⛔ 这是"等"，不是"等到就杀" —— 超时后一律放弃本次操作并返回失败，
 *      绝不再 vTaskDelete（理由见 audio_wait_task_gone 上方长注释）。
 *   正常情况任务在 25ms~200ms 内就退完；3s 只为兜住"慢卡 + fclose 卡顿"这种
 *   极端情况，避免把"还在正常收尾"误判成"必须收拾"。 */
#define MP3_STOP_WAIT_MS       3000
/* ★★★ 2026-11-22 【MP3 反复崩机的真正根因】>> 必须读完再改 << ★★★
 *
 * 崩溃栈（三轮现场完全一致）：
 *   vprintf → _vfprintf_r → lock_acquire_recursive
 *          → xQueueTakeMutexRecursive → xQueueSemaphoreTake:1718
 *          → assert(pxQueue->uxItemSize == 0) → panic_abort(IllegalInstruction)
 *
 * 崩在 **main 任务的 printf** 里，取的是 newlib 的**全局** stdio 递归锁，
 * 说明那块锁对象的内存被别人覆盖了 —— 不是音频自己的数据坏。
 *
 * 真凶：这里原来用 `xTaskCreateStaticPinnedToCore(..., s_audio_stack, &s_audio_tcb)`，
 * 也就是**每次播放都复用同一块 TCB 和同一块栈**。
 * `vTaskDelete(NULL)` 只是把任务挂到 terminated 链表，**TCB 的回收由 idle 任务完成**；
 * 播完立刻切下一首时 CPU 正忙，idle 可能几十毫秒都轮不到 ⇒ 旧 TCB 还在链表里，
 * 新任务就用**同一块内存**重新初始化 ⇒ 链表节点被就地覆盖、串成环/断链
 * ⇒ 之后任意一次 malloc/take-lock 撞到坏内存就 abort。
 * 这解释了"为什么第二轮删掉强杀后照样崩"——强杀不是病根，**静态复用**才是。
 *
 * ⇒ 根治：**不再静态复用，一律动态创建**（xTaskCreatePinnedToCoreWithCaps）。
 *   新任务永远拿新 TCB/新栈，旧任务由 FreeRTOS 自己在 idle 里回收，
 *   两者物理隔离，任何时序下都不可能互相覆盖。
 * ⛔ 不要为了"省一次 malloc"改回 xTaskCreateStaticPinnedToCore。
 * ⛔ 也不要在本文件任何地方 vTaskDelete 播放/输出任务（见 audio_wait_task_gone）。 */
/* MP3 解码用的两块帧缓冲(pcm/pcm_out)在任务里是**局部指针**。
 * 它们现在由任务自己在 cleanup 里释放（因为不再有外部强杀，cleanup 一定跑完）。
 * 下面这两个文件作用域指针只做**异常路径**的记录：正常路径始终是 NULL，
 * 不再有任何"外部兜底 free"去动它们（那套 reap 逻辑已随强杀一起删除）。 */
static mp3d_sample_t *s_dec_pcm = NULL;
static int16_t       *s_dec_pcm_out = NULL;
/* ★★★ 2026-11-22 三轮排查的最终结论（写在这里，防止以后再走回头路）★★★
 * 崩溃栈长这样（第二、三轮现场一致）：
 *   ... → vprintf → _vfprintf_r → lock_acquire_recursive
 *       → xQueueSemaphoreTake → assert(pxQueue->uxItemSize == 0)
 * 崩在 **main 任务的 printf** 里，说明坏的不是音频自己的数据，而是
 * **全局 stdio 状态**：`_GLOBAL_REENT->_fp` 文件链表被破坏。
 *
 * 真凶：`fclose()` 内部要维护那条**全局**链表；任务在 fclose 内部被
 * `vTaskDelete` 强杀 ⇒ 链表停在中间状态 ⇒ 之后任何 printf 都在坏链表上跑。
 * 同理 `free()` 被打断会把 TLSF 空闲链表留在中间状态。
 *
 * ⇒ 结论：**永不强杀播放任务**（见 audio_wait_task_gone）。这里不需要任何
 *   "收尾中"标志 —— 因为压根没有"外部删任务"这回事了。 */

static void audio_play_task(void *arg) {
    ESP_LOGI(TAG, ">>> \u97F3\u9891\u4EFB\u52A1\u5DF2\u542F\u52A8 (\u6808 %u \u5B57\u8282) <<<",
             (unsigned)(AUDIO_TASK_STACK_WORDS * sizeof(StackType_t)));
    mp3d_sample_t *pcm = NULL;
    int16_t *pcm_out = NULL;

    ESP_LOGI(TAG, "[1/6] \u6CE8\u518C TWDT...");
    esp_task_wdt_add(NULL);

    /* ★ 2026-11-22 根治崩溃：曲长估算必须在本任务（64KB 栈）里做。
     *   estimate_total_samples() 内部调用 mp3dec_decode_frame()，该函数带一个
     *   16608B 的局部 mp3dec_scratch_t（minimp3.h:1718）。原来在
     *   audio_player_play()（main 任务，栈仅 10240B）里调用 ⇒ 栈指针跌到栈底以下的
     *   堆区，被中断/切换帧踩坏 newlib 的 stdio 锁 ⇒ 下次 printf 触发
     *   assert(pxQueue->uxItemSize == 0)。pcm==NULL 时不写 scratch，所以 canary
     *   不会报"栈溢出"，现场只表现为堆损坏 —— 这就是前几轮一直找不到的原因。
     *   ⛔ 禁止搬回 audio_player_play() 或任何小栈任务。 */
    if (s_mp3_file) {
        s_total_samples = estimate_total_samples(s_mp3_file, s_file_size);
        fseek(s_mp3_file, 0, SEEK_SET);   /* 估算读过文件头，复位到开头再解码 */
        s_mp3_buf_pos = 0;
        s_mp3_buf_len = 0;
    }

    ESP_LOGI(TAG, "[2/6] \u521D\u59CB\u5316 I2S...");
    int sample_rate = 44100;
    int i2s_ret = i2s_init(sample_rate);
    if (i2s_ret != 0) {
        ESP_LOGD(TAG, "I2S \u521D\u59CB\u5316\u5931\u8D25, \u505C\u6B62\u64AD\u653E");
        goto cleanup;
    }
    ESP_LOGI(TAG, "[3/6] \u542F\u52A8 ES8311...");
    vTaskDelay(pdMS_TO_TICKS(20));
    /* ★ 2026-11-22: 原来这里手拼 es8311_start() + pa_enable(true) + set_volume(),
     *   与 audio_out_start / ensure_out_active 各自一套, 极易漏步。
     *   audio_output_begin() 内部已含 codec 自检、**音量下发**、最后开功放。 */
    audio_output_begin();

    ESP_LOGI(TAG, "[5/6] \u5206\u914D\u89E3\u7801\u7F13\u51B2\u533A...");
    /* ★ 2026-10-25: 解码器热数据进内部 RAM(优先), PSRAM 兜底; 播放结束释放 */
    s_mp3dec = heap_caps_malloc(sizeof(mp3dec_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_mp3dec) s_mp3dec = heap_caps_malloc(sizeof(mp3dec_t), MALLOC_CAP_SPIRAM);
    if (!s_mp3dec) s_mp3dec = malloc(sizeof(mp3dec_t));
    if (!s_mp3dec) {
        ESP_LOGD(TAG, "mp3dec_t \u5206\u914D\u5931\u8D25");
        goto cleanup;
    }
    mp3dec_init(s_mp3dec);
    s_mp3_buf_pos = 0;
    s_mp3_buf_len = 0;
    int current_sample_rate = sample_rate;
    bool first_frame = true;
    pcm = heap_caps_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(mp3d_sample_t), MALLOC_CAP_SPIRAM);
    pcm_out = heap_caps_malloc(2304 * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!pcm || !pcm_out) {
        ESP_LOGD(TAG, "PSRAM \u5206\u914D\u5931\u8D25");
        goto cleanup;
    }
    /* 记录一份到文件作用域（诊断用：能看出"任务退出时忘了释放"这类泄漏） */
    s_dec_pcm = pcm;
    s_dec_pcm_out = pcm_out;

    ESP_LOGI(TAG, "[6/6] \u8FDB\u5165\u89E3\u7801\u5FAA\u73AF...");
    while (!s_stop_requested) {
        /* \u5582\u72D7: \u9632\u6B62 TWDT \u8D85\u65F6\u91CD\u542F */
        esp_task_wdt_reset();

        /* 跳转 seek: OS 线程置请求, 本任务在此(播放/暂停均可)执行, 避免竞态.
         * fseek 到目标字节 + 清空解码缓冲, 解码循环随后自动逐字节帧同步重对齐. */
        if (s_seek_req_progress >= 0) {
            int64_t target = (int64_t)s_seek_req_progress * s_file_size / 1000;
            fseek(s_mp3_file, target, SEEK_SET);
            s_mp3_buf_len = 0;
            s_mp3_buf_pos = 0;
            if (s_total_samples > 0)
                s_elapsed_samples = (int64_t)s_seek_req_progress * s_total_samples / 1000;
            s_progress = s_seek_req_progress;
            s_seek_req_progress = -1;
        }

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
        int samples = mp3dec_decode_frame(s_mp3dec,
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
            if (s_mp3_buf_len == 0) {
                if (feof(s_mp3_file)) {
                    s_natural_end = true;   /* 真·读到文件尾, 自然播完 */
                    ESP_LOGI(TAG, "\u64AD\u653E\u7ED3\u675F: %s", s_filename);
                    break;
                }
                /* 瞬时失败: 缓存已耗尽但未到文件尾(fread=0 未置 feof 属读出瞬时抖动).
                 * 绝不能误判"播放结束", 否则页面自动重播 → "~1秒假结束"死循环没声音.
                 * 短暂等待后回到循环顶重新 fread 续播.
                 * V1.2: 日志加 1s 节流 —— 重试间隔仅 25ms, 持续失败会刷 40 条/秒. */
                static uint32_t s_mp3_retry_log_ms = 0;
                uint32_t rnow = esp_log_timestamp();
                if (rnow - s_mp3_retry_log_ms >= 1000) {
                    s_mp3_retry_log_ms = rnow;
                    ESP_LOGD(TAG, "MP3 缓存空且未到文件尾(瞬时读取失败, elapsed=%lld), 等待重试续播",
                             (long long)s_elapsed_samples);
                }
                vTaskDelay(pdMS_TO_TICKS(25));
                continue;
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
                /* ★ 2026-10-25 [LINTOS] 「声音很怪/很闷/慢慢吞吞」根因：
                 *   原来**首帧只调 es8311_set_sample_rate()，不重配 I2S 时钟** ——
                 *   而 I2S 是按 [2/6] 时的默认率(44100)初始化的，
                 *   于是 22050Hz 的数据被 44100 的位时钟送出去 ⇒ 音调/速度全错。
                 *   这里改成**首帧也统一重配时钟**（先 disable 再 reconfig 再 enable）。 */
                i2s_tx_disable();
                current_sample_rate = info.hz;
                ESP_LOGI(TAG, "\u91C7\u6837\u7387: %d Hz, \u58F0\u9053: %d, \u5E27\u5B57\u8282: %d", info.hz, info.channels, info.frame_bytes);
                if (first_frame) {
                    first_frame = false;
                    ESP_LOGI(TAG, ">>> \u5F00\u59CB\u89E3\u7801MP3\u97F3\u9891\u6570\u636E <<<");
                }
                {
                    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(current_sample_rate);
                    i2s_channel_reconfig_std_clock(s_tx_chan, &clk_cfg);
                    i2s_tx_enable();
                }
                es8311_set_sample_rate(current_sample_rate);
                if (s_i2s_mutex) xSemaphoreGive(s_i2s_mutex);
            }

            /* 按平台组织输出样本数:
             *  两平台统一立体声输出: mono 源升 mix 为双声道, 立体声源直接输出.
             *  NS4168 虽然是单声道功放, 但 I2S 配 STEREO (避 ESP-IDF 5 mono bug),
             *  数据复制到 L/R, 功放随 CTRL 取一路播放. */
            int16_t *src = (int16_t *)pcm;
            int frame_count = 0;    /* 本次写的采样数(每帧样本数) */
            if (info.channels == 1) {
                for (int i = 0; i < samples; i++) {
                    pcm_out[i * 2]     = src[i];
                    pcm_out[i * 2 + 1] = src[i];
                }
            } else {
                memcpy(pcm_out, src, samples * 2 * sizeof(int16_t));
            }
            frame_count = samples * 2;

            /* 音量调节 (NS4168 无 DAC 音量, 纯数字缩放; 0=静音, 1~9 保底 10% 防断崖)
             * ★ 2026-10-25 性能: Q15 定点乘移位, 免每样本 int16↔float 转换 */
            {
                int vol_q15 = s_volume * 32768 / 100;
                if (s_volume > 0 && vol_q15 < 3277) vol_q15 = 3277;   /* 保底 10% */
                if (vol_q15 < 32768) {
                    for (int i = 0; i < frame_count; i++)
                        pcm_out[i] = (int16_t)(((int32_t)pcm_out[i] * vol_q15) >> 15);
                }
            }

            /* ★ 2026-10-25 [LINTOS] 叠加游戏音效（SDLPal 的 VOC 实时合成）。
             *   放在音量缩放之后、喂振幅表之前，这样响度表反映的是最终输出。
             *   音效与音乐同为 44100Hz 立体声交错 int16，直接逐样本相加。 */
            audio_player_fx_mix(pcm_out, (uint32_t)frame_count);

            /* V1.0.71: 喂振幅表 (MP3 路径) */
            audio_meter_feed(pcm_out, (uint32_t)frame_count);

            /* \u5199\u5165 I2S */
            ensure_out_active();          /* ★ 通道可能被空闲静音关过, 先拉起 */
            size_t bytes_written = 0;
            size_t to_write = (size_t)frame_count * sizeof(int16_t);
            esp_err_t wret = i2s_channel_write(s_tx_chan, pcm_out, to_write,
                                               &bytes_written,
                                               pdMS_TO_TICKS(I2S_WRITE_TIMEOUT_MS));
            /* ★ 2026-10-31: 原来失败走 ESP_LOGD，而 sdkconfig 的 CONFIG_LOG_MAXIMUM_LEVEL=3
             *   ⇒ 这句在编译期就被裁掉 ⇒ "I2S 写不进去却完全没有日志"。
             *   改成 WARN（保留），失败原因/已写字节一律可见。 */
            if (wret != ESP_OK) {
                ESP_LOGW(TAG, "I2S 写入失败: %s (已写 %u/%u)",
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
    /* ★★ 这段只在"本任务自己决定退出"时执行 —— 外部已不再强杀(见
     *   audio_wait_task_gone)。因此这里的 free/fclose 一定完整跑完，
     *   不会出现"释放到一半被打断"把 TLSF / 全局 stdio 链表搞坏的情况。
     *   仍然保留"先摘所有权、再释放"的写法：它让异常路径(断电前最后几毫秒)
     *   也不会二次释放，成本只有几条指针赋值。 */
    pa_enable(false);
    mp3dec_t *dec   = s_mp3dec;        s_mp3dec = NULL;      /* 解码器退回, 不常驻内部 RAM */
    FILE     *fp    = s_mp3_file;      s_mp3_file = NULL;
    s_dec_pcm = NULL;                  /* 与下方 pcm 同一块 */
    s_dec_pcm_out = NULL;              /* 与下方 pcm_out 同一块 */
    s_state = AUDIO_STATE_STOPPED;
    if (dec) free(dec);
    if (pcm) free(pcm);
    if (pcm_out) free(pcm_out);
    if (fp) fclose(fp);
    /* 注销 TWDT: 注册后不注销, 任务删除后 TWDT 仍持有句柄, 5s 后触发误报警告 */
    esp_task_wdt_delete(NULL);
    ESP_LOGI(TAG, "\u64AD\u653E\u4EFB\u52A1\u7ED3\u675F, \u6808\u9AD8\u6C34\u4F4D\u5269\u4F59=%u \u5B57 (\u603B %u \u5B57) handle=%p",
             (unsigned)uxTaskGetStackHighWaterMark(NULL), (unsigned)AUDIO_TASK_STACK_WORDS,
             (void *)xTaskGetCurrentTaskHandle());
    /* ⚠ 句柄必须在这**最后**才清：一旦清 NULL，主线程就会开始创建新的播放任务。
     *   所有资源释放都必须在它之前完成。
     *   ★ 2026-11-22: TCB/栈现在是动态分配的，清句柄后即使旧 TCB 还没被 idle 回收，
     *     新任务也拿的是**新内存**，不存在"共用同一块 TCB/栈"的覆盖问题。 */
    s_task_handle = NULL;
    /* ★★★ 2026-11-22【真·根因之三】WithCaps 创建 ⇒ 必须 WithCaps 删除 ★★★
     * 本任务由 xTaskCreatePinnedToCoreWithCaps 创建（内部走 static 创建 API），
     * FreeRTOS 的 prvDeleteTCB 对 static 任务**什么都不 free**：
     *   ⇒ 用 vTaskDelete(NULL) 自杀 = **每播一首泄漏 64KB PSRAM + 一个 TCB(内部 RAM)**。
     * 8MB PSRAM / 64KB ≈ 120 首；内部 RAM 更紧。几十首之后 heap_caps_malloc 开始失败，
     * 于是 fopen 的 FILE、newlib 的 stdio 递归锁等内部分配陆续拿不到/拿错，
     * 表现就是"用一阵子之后开始随机崩，断电重启又能好一阵"。
     * vTaskDeleteWithCaps(NULL) 会另起一个临时清理任务把本任务连同 TCB/栈一起 free。
     * ⛔ 不要改回 vTaskDelete(NULL)。 */
    vTaskDeleteWithCaps(NULL);
}

/* ★★★ 2026-11-22 【第三轮 · 彻底删掉强杀】★★★
 * 前两轮都在"怎么杀才安全"上打转，结论是：**杀不安全，不杀才对**。
 *
 * 真根因（把三轮崩溃现场串起来才看清）：
 *   `fclose()` 内部要维护 **全局** 的 `_GLOBAL_REENT->_fp` 文件链表 —— 把 FILE
 *   从链表里摘出来。任务若在 fclose 内部被 `vTaskDelete` 打断，这个**全局**链
 *   表就停在中间状态 ⇒ 之后**任何** printf/vprintf（包括 main 任务的 ESP_LOG）
 *   都在坏链表上跑 ⇒ `assert(pxQueue->uxItemSize == 0)` 重启。
 *   ⚠ 崩溃现场崩在 **main 的 vprintf** 里、而不是音频任务里，正是这个的指纹。
 *   同理，free() 被打断会把 TLSF 空闲链表留在中间状态。
 *
 * 所以：
 *   ⛔ 任何情况下都**不再** vTaskDelete 播放任务。
 *   播放循环每轮 `esp_task_wdt_reset()`，fread 有文件尾判断，i2s_channel_write
 *   有 500ms 超时，且 `s_stop_requested` 每轮都查 ⇒ 它**一定**会自己走到
 *   cleanup 并 `vTaskDelete(NULL)`（自杀是安全的，因为它已经跑完了所有释放）。
 *   等不到就"本次操作失败"，让用户下次再点 —— 播不出来远好于随机崩机。
 *
 * ⛔ 不要因为"等太久"再把强杀加回来。前两轮已经证明这条路只会把堆踩坏。 */
static bool audio_wait_task_gone(int ms)
{
    for (int w = 0; w < ms / 10; w++) {
        if (!s_task_handle) {
            /* ★ 句柄清 NULL 只代表"任务已把资源释放完、正准备自杀"。
             *   给一点时间让 vTaskDelete(NULL) 真正把任务摘离就绪链 ——
             *   否则新任务可能在旧任务还没离开调度器时就开始跑。
             *   （TCB/栈已是动态分配，此处不再有"静态内存被复用"的风险，
             *     这点延迟只是为了让调度状态干净，不是安全前提。） */
            vTaskDelay(pdMS_TO_TICKS(20));
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return (s_task_handle == NULL);
}

/* ============ \u516C\u5171 API ============ */
/* ★★ 2026-10-14 修"音量调整不生效" ★★
 * 原来音量和静音开关只在 **设置页 on_enter** 的 settings_load() 里恢复 ⇒ 每次开机/刷机后
 * s_volume 都停在默认 100%, 用户"上次调的音量"看起来压根没生效。音量归音频服务自己管 ——
 * **开机即读回**。(设置页里对应那段已删, 保证单一归属。)
 * ⚠ 兼容两代存档: 旧固件存 0-100 百分比, 新固件存 0-9 档位 (key "vol", ns "os_settings")。 */
#define AUDIO_CFG_NS       "os_settings"
#define AUDIO_VOL_KEY      "vol_pct"   /* ★ 规范存档 = 百分比 0-100 (唯一真相源) */
#define AUDIO_VOL_KEY_OLD  "vol"       /* 旧存档: 0-9 档位 (更早: 0-100 百分比) —— 仅开机迁移用 */
#define AUDIO_SND_KEY      "snd_on"
#define AUDIO_VOL_SAVE_MS  1500        /* 落盘防抖: 拖滑条是每帧改, 不能每帧写 flash */

static esp_timer_handle_t s_vol_save_timer = NULL;

static void audio_vol_save_now(void)
{
    nvs_handle_t h;
    if (nvs_open(AUDIO_CFG_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, AUDIO_VOL_KEY, (uint8_t)((s_volume < 0) ? 0 : (s_volume > 100 ? 100 : s_volume)));
    nvs_commit(h);
    nvs_close(h);
}

static void audio_vol_save_cb(void *arg) { (void)arg; audio_vol_save_now(); }

/* 防抖落盘: 连续调节(拖音乐滑条/滚轮连按)只在停下来 1.5s 后写一次 */
static void audio_vol_save_deferred(void)
{
    if (!s_vol_save_timer) {
        const esp_timer_create_args_t a = { .callback = audio_vol_save_cb, .name = "vol_save" };
        if (esp_timer_create(&a, &s_vol_save_timer) != ESP_OK) {
            s_vol_save_timer = NULL;
            audio_vol_save_now();
            return;
        }
    }
    esp_timer_stop(s_vol_save_timer);
    esp_timer_start_once(s_vol_save_timer, (uint64_t)AUDIO_VOL_SAVE_MS * 1000);
}

/* ★ 2026-11-22: 声音开关(静音)也归音频服务自己持久化。
 *   原来**只有设置页**在存 snd_on(两处重复), 而 audio_player_set_muted() 自己不存 ⇒
 *   Lua 的 app.mute(true) / 任何非设置页入口改的静音状态, **重启即丢**, 且与设置页
 *   写下的值互相打架。现在写入也收敛到这里, 设置页那段重复保存已删。
 *   静音是开关型事件(不是连续拖动), 直接落盘即可, 不需要防抖。 */
static void audio_snd_save_now(void)
{
    nvs_handle_t h;
    if (nvs_open(AUDIO_CFG_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, AUDIO_SND_KEY, s_muted ? 0 : 1);
    nvs_commit(h);
    nvs_close(h);
}

static void audio_cfg_load(void)
{
    nvs_handle_t h;
    if (nvs_open(AUDIO_CFG_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t v = 0;
    if (nvs_get_u8(h, AUDIO_VOL_KEY, &v) == ESP_OK) {           /* 新存档: 直接是百分比 */
        s_volume = (v > 100) ? 100 : (int)v;
        ESP_LOGI(TAG, "恢复上次音量: %d%%", s_volume);
    } else if (nvs_get_u8(h, AUDIO_VOL_KEY_OLD, &v) == ESP_OK) { /* 旧存档: 0-9 档位 / 0-100 百分比 */
        s_volume = (v > (uint8_t)(AUDIO_VOL_STEPS - 1)) ? (int)v
                                                        : audio_player_vol_to_percent((int)v);
        ESP_LOGI(TAG, "恢复上次音量(旧存档 %u) -> %d%%", (unsigned)v, s_volume);
    }
    if (nvs_get_u8(h, AUDIO_SND_KEY, &v) == ESP_OK) {
        s_muted = (v == 0);
        ESP_LOGI(TAG, "恢复声音开关: %s", s_muted ? "静音" : "开");
    }
    nvs_close(h);

    /* ★ 2026-11-01: 音量为 0 时必须留下**可执行的恢复办法**。
     *   实测踩过：全量烧录会把 NVS 一起写掉，某次存进去的 0 就成了永久静音，
     *   用户只看到"没声音"，日志里也没有任何提示。
     *   ⚠ 不在这里强行改音量 —— 0 可能是用户自己调的，覆盖用户设置更糟。 */
    if (s_volume <= 0 || s_muted) {
        ESP_LOGW(TAG, "当前是静音状态（音量=%d%% 静音=%d）⇒ 需要声音请执行: vol 100",
                 s_volume, (int)s_muted);
    }
}

int audio_player_init(void) {
    if (s_hw_initialized) return 0;
    audio_cfg_load();   /* ★ 开机先把上次的音量/静音读回来 (在 es8311_init 之前) */

    /* ★ 第一件事: 把功放使能脚配置好并**拉低**。
     *   放在最前面(早于 I2C 探测 / I2S 时钟 / ES8311 启动) → 从这一刻起功放确定关断,
     *   开机不会因为引脚悬空 + 模块侧上拉而"噗"一声, 也不会白耗电。
     *   (原来这脚要到本函数**最后**才配置, 中间那几十毫秒是浮空的。) */
    audio_pa_init();

    ESP_LOGI(TAG, "初始化音频硬件...");
    if (!s_i2s_mutex) {
        s_i2s_mutex = xSemaphoreCreateMutex();
    }
    /* ★ 2026-10-28: 补幂等判断 —— 下面 i2s_init 失败会 return -1 且不清 s_hw_initialized,
     *   重试时又会 xSemaphoreCreateMutex() 一次 ⇒ 上一个 mutex 句柄永久丢失(泄漏)。 */
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
    }

    /* MP3 缓冲区延迟到 audio_player_play() 分配, 节省 PSRAM */

    /* V1.0.70: 平台探测 — 有 ES8311 = 官方板; 无 ES8311 = 自制板 NS4168.
     * 必须先于首次 i2s_init 建通执行, 以便 NS4168 平台第一路 I2S 就用 MONO 单声道槽配置.
     * 注意: es8311_init() 未检测到芯片时也返回 0, 必须用 es8311_is_present() 判断!
     * (旧逻辑 if(es8311_init()!=0) 永不成立 → NS4168 平台的 CTRL 硬静音从未生效,
     *  静音时只能靠 ES8311 软静音(芯片不存在被跳过) → 自制板静音无效) */
    es8311_init(s_i2c_sda, s_i2c_scl);
    if (!es8311_is_present()) {
        ESP_LOGD(TAG, "未探测到 ES8311: 按 NS4168 I2S 数字单声道功放直驱 (CTRL=GPIO46)");
        s_use_ns4168_ctrl = true;   /* NS4168 平台: GPIO46 作 CTRL 硬静音 (高=工作, 低=关断) */
    } else {
        s_use_ns4168_ctrl = false;  /* 官方板 ES8311: GPIO46 仍归功放使能, 静音走软静音 */
    }

    /* 先 I2S/MCLK 再 ES8311 */
    if (i2s_init(44100) != 0) {
        ESP_LOGD(TAG, "I2S/MCLK \u521D\u59CB\u5316\u5931\u8D25");
        return -1;
    }
    vTaskDelay(pdMS_TO_TICKS(20));

    /* 探测板载/外接麦克风 ADC(ES7210), 仅日志; 无 ES7210 时麦克风走 INMP441 直读 I2S(GPIO10) */
    int mic_addr = es7210_probe(I2C_NUM_0);
    if (mic_addr) {
        ESP_LOGI(TAG, "ES7210 麦克风 ADC 探测成功: 0x%02X", mic_addr);
    } else {
        ESP_LOGI(TAG, "未探测到 ES7210, 麦克风按 INMP441 直读 I2S(GPIO10) 处理");
    }

    pa_enable(false);

    /* ★★★ 2026-11-02 linit「常开的 I2S 全部关掉」★★★
     *   上面 i2s_init(44100) 会把 TX 通道 enable —— 于是**即使什么都不播**，
     *   MCLK/BCLK 也一直在输出，ES8311/ES7210 永远"有钟无数据"地挂着
     *   （真机实测空闲 tx=1 rx=1，PM 锁表里 i2s_driver APB_FREQ_MAX 常驻
     *     ⇒ CPU 也降不到 80MHz）。初始化完就把通道 disable（只停时钟，不删通道），
     *   首次播放由 ensure_out_active() 重新 enable，采样率重配也不受影响。 */
    if (s_i2s_mutex && xSemaphoreTake(s_i2s_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        i2s_tx_disable();
        i2s_rx_disable();      /* 从未 enable 过 ⇒ 现在会被拦下, 不再打 E 级日志 */
        xSemaphoreGive(s_i2s_mutex);
        ESP_LOGI(TAG, "音频空闲态: I2S 通道已 disable (不输出 MCLK/BCLK, 编解码器不再耗电)");
    }

    s_hw_initialized = true;
    ESP_LOGI(TAG, "\u97F3\u9891\u786C\u4EF6\u521D\u59CB\u5316\u5B8C\u6210");
    return 0;
}

int audio_player_play(const char *filepath) {
    /* ★ 2026-11-22: 静音时**照常播放**(只是不出声), 不再 return -1。
     *   原因: 旧行为下用户在静音状态点播放 ⇒ 页面拿不到"开始", 进度不动、按钮不回弹,
     *   看起来就是"程序坏了"; 而且解除静音后还得重新点一次。
     *   现在: 解码/进度照走, codec 音量为 0 故无声; 一旦解除静音立刻能听见。
     *   ⛔ 别改回"静音即拒绝播放" —— 那是用户"点了没反应"投诉的来源之一。 */

    /* V1.0.60: 解耦 MP3 与游戏音频.
     * 游戏用的 PCM 环形缓冲输出任务 (audio_out) 与 MP3 任务都在写同一个 I2S,
     * 若上一局游戏结束未停, MP3 开始时会双任务并发写 I2S -> 崩溃/卡死.
     * 这里先停掉 audio_out, 保证同一时间只有一个 I2S 写者. */
    /* ★ 2026-11-22: audio_out 没退干净就**绝不**接手 I2S —— 两个任务同时写同一通道
     *   会立刻写坏驱动状态（历史上就是这么炸的）。宁可本次播放失败让用户再点一次，
     *   也不能去抢一条还在被写的通道。 */
    if (!audio_out_stop()) {
        ESP_LOGE(TAG, "audio_out 未能退出 ⇒ 本次播放取消（不抢 I2S 通道）");
        return -1;
    }

    if (!s_hw_initialized) {
        if (audio_player_init() != 0) return -1;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (s_task_handle) {
        s_stop_requested = true;
        xSemaphoreGive(s_mutex);
        /* ★ 2026-11-22 【第三轮】绝不强杀 —— 理由见 audio_wait_task_gone() 上方长注释。
         *   任务自己会走完 cleanup（fread 有文件尾判断、i2s 写有超时、每轮查
         *   s_stop_requested），这里只等。等不到就放弃本次播放。 */
        if (!audio_wait_task_gone(MP3_STOP_WAIT_MS)) {
            ESP_LOGE(TAG, "上一播放任务 %dms 未退 ⇒ 本次播放取消（绝不强杀，"
                          "强杀会打断 fclose 破坏全局 stdio 链表）", MP3_STOP_WAIT_MS);
            return -1;              /* s_mutex 已在上面 give 过 */
        }
        xSemaphoreTake(s_mutex, portMAX_DELAY);
    }
    s_stop_requested = false;
    /* 任务正常退出时已把 s_mp3dec 释放并置 NULL；这里只兜底"上次异常残留"。 */
    if (s_mp3dec) { free(s_mp3dec); s_mp3dec = NULL; }

    const char *name = strrchr(filepath, '/');
    if (name) name++; else name = filepath;
    strncpy(s_filename, name, sizeof(s_filename) - 1);
    s_filename[sizeof(s_filename) - 1] = '\0';

    s_mp3_file = fopen(filepath, "rb");
    if (!s_mp3_file) {
        ESP_LOGD(TAG, "\u65E0\u6CD5\u6253\u5F00: %s", filepath);
        xSemaphoreGive(s_mutex);
        return -1;
    }

    fseek(s_mp3_file, 0, SEEK_END);
    s_file_size = ftell(s_mp3_file);
    fseek(s_mp3_file, 0, SEEK_SET);
    s_progress = 0;
    s_elapsed_samples = 0;
    s_natural_end = false;
    s_seek_req_progress = -1;

    /* ★ 2026-11-22 根治：曲长估算**不在这里做**（本函数跑在 main 任务，栈只有 10240B）。
     *   estimate_total_samples() 会调用 mp3dec_decode_frame()，后者有一个 16608B 的
     *   局部变量 mp3dec_scratch_t（minimp3.h:1718），栈帧远超 main 栈 ⇒ sp 跌到栈底
     *   以下的堆区，被中断帧踩坏 stdio 锁 ⇒ 点播放偶发 panic 在 vprintf 的 assert。
     *   估算已挪进 audio_play_task（64KB 栈）。此处只清 0，等任务算出真实值。
     *   ⛔ 不要搬回来，也不要让任何小栈任务调用 estimate_total_samples()。 */
    s_total_samples = 0;

    s_state = AUDIO_STATE_PLAYING;

    /* 延迟分配 MP3 缓冲区 (只在播放时才占内存) */
    if (!s_mp3_buf) {
        s_mp3_buf = heap_caps_malloc(MP3_FRAME_BUF_SIZE, MALLOC_CAP_SPIRAM);
        if (!s_mp3_buf) s_mp3_buf = malloc(MP3_FRAME_BUF_SIZE);
        if (!s_mp3_buf) {
            ESP_LOGD(TAG, "MP3 缓冲区分配失败");
            fclose(s_mp3_file);
            s_mp3_file = NULL;
            s_state = AUDIO_STATE_STOPPED;
            xSemaphoreGive(s_mutex);
            return -1;
        }
    }

    /* ★ 2026-11-22 根治：动态创建任务（TCB + 64KB 栈都由 FreeRTOS 从堆分配）。
     *   原来用 xTaskCreateStaticPinnedToCore + 文件作用域 static TCB/栈 ⇒
     *   每次播放都复用同一块内存，而旧 TCB 要等 idle 回收，播完立刻切歌就会
     *   "旧 TCB 还在链表、新任务又把它初始化一遍" ⇒ 链表串坏 ⇒ 随机 abort。
     *   动态创建后新旧任务物理隔离，彻底不可能互相覆盖。
     *   栈优先 PSRAM（内部 DRAM 最大连续块仅 ~31KB，容不下 64KB 峰值栈）。 */
    BaseType_t cr = xTaskCreatePinnedToCoreWithCaps(
                        audio_play_task, "audio_play", AUDIO_TASK_STACK_WORDS, NULL, 2,
                        &s_task_handle, 1,
                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (cr != pdPASS) {
        ESP_LOGW(TAG, "PSRAM 任务创建失败 ⇒ 退回内部 RAM 再试一次");
        cr = xTaskCreatePinnedToCoreWithCaps(
                        audio_play_task, "audio_play", AUDIO_TASK_STACK_WORDS, NULL, 2,
                        &s_task_handle, 1, MALLOC_CAP_8BIT);
    }
    if (cr != pdPASS || s_task_handle == NULL) {
        ESP_LOGE(TAG, "音频任务创建失败(%d) ⇒ 无法播放", (int)cr);
        s_task_handle = NULL;
        if (s_mp3_buf) { free(s_mp3_buf); s_mp3_buf = NULL; }
        fclose(s_mp3_file);
        s_mp3_file = NULL;
        s_state = AUDIO_STATE_STOPPED;
        xSemaphoreGive(s_mutex);
        return -1;
    }
    ESP_LOGI(TAG, "音频任务已创建 (动态 TCB+栈 %u 字节, PSRAM) handle=%p",
             (unsigned)(AUDIO_TASK_STACK_WORDS * sizeof(StackType_t)), (void *)s_task_handle);

    ESP_LOGI(TAG, "\u5F00\u59CB\u64AD\u653E: %s (%lld bytes)", s_filename, (long long)s_file_size);
    xSemaphoreGive(s_mutex);
    return 0;
}

void audio_player_pause(void) {
    ESP_LOGI(TAG, "[diag] pause state=%d", (int)s_state);
    if (s_state == AUDIO_STATE_PLAYING) {
        s_state = AUDIO_STATE_PAUSED;
        pa_enable(false);
        ESP_LOGI(TAG, "\u6682\u505C");
    }
}

void audio_player_resume(void) {
    ESP_LOGI(TAG, "[diag] resume state=%d", (int)s_state);
    if (s_state == AUDIO_STATE_PAUSED) {
        s_state = AUDIO_STATE_PLAYING;
        /* ★ 2026-11-22: 统一走收敛点(校验 codec + 下发音量 + 开功放)。
         *   原来只 pa_enable(true), 若暂停期间 codec 被别处关断/掉电就再也补不回来。 */
        audio_output_begin();
        ESP_LOGI(TAG, "\u6062\u590D");
    }
}

void audio_player_stop(void) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    /* ★ 2026-11-22: 无条件清"自然播完"标志。原来只在 s_task_handle 非空时清，
     *   而任务自然播完后是**自己**退出的（句柄已 NULL）⇒ 标志残留。
     *   调用方若拿它做"播完→切下一首"，就会反复触发。stop 的语义本来就是
     *   "这不是自然播完"，与有没有任务无关。 */
    s_natural_end = false;
    if (s_task_handle) s_stop_requested = true;
    xSemaphoreGive(s_mutex);
    pa_enable(false);            /* ⚠ 放锁外: pa_enable 内含 30ms 消爆音等待, 别卡住 s_mutex */

    /* ★ 2026-11-22 【第三轮】同样绝不强杀（理由见 audio_wait_task_gone 上方注释）。
     *   等不到就地返回：任务还在跑就**不能** audio_out_stop —— 那会释放/停掉
     *   它正在写的 I2S 通道，下一轮 i2s_channel_write 就是野指针。 */
    if (!audio_wait_task_gone(MP3_STOP_WAIT_MS)) {
        ESP_LOGW(TAG, "播放任务 %dms 未退 ⇒ 保持现状（不杀、不停 I2S），它会自行退出",
                 MP3_STOP_WAIT_MS);
        return;
    }
    s_state = AUDIO_STATE_STOPPED;
    (void)audio_out_stop();   /* 解耦: 一并停掉游戏 PCM 输出任务 (失败也不强杀) */
    ESP_LOGI(TAG, "\u505C\u6B62\u64AD\u653E");
}

/* ★★ 音量调节的**唯一入口** (2026-10-14 统一) ★★
 * 设置页滚轮 / 音乐页滑条 / 引擎优化 / 串口 vol —— 全部只调这一个函数。
 * 它同时负责: ① 改状态 ② 写编码器寄存器 ③ 防抖落盘。
 * ⛔ 谁都不许再自己 nvs 存音量 (旧代码设置页存 "vol" 档位、音乐页不存 ⇒ 开机不一致)。 */
void audio_player_set_volume(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    bool changed = (percent != s_volume);
    ESP_LOGD(TAG, "set_volume %d -> %d (muted=%d, use_ns=%d)", s_volume, percent, (int)s_muted, (int)s_use_ns4168_ctrl);
    s_volume = percent;
    /* ★ 2026-11-22: 音量**始终**下发给 codec, 即使当前静音(写 0)。
     *   这样"调音量"这个动作本身一定能修好"寄存器停在 0"导致的无声 ——
     *   用户反馈里"动一下音量就恢复"正是这个副作用, 现在把它变成保证。 */
    es8311_set_volume(s_muted ? 0 : percent);
    if (changed) audio_vol_save_deferred();   /* 唯一持久化点 */
}

int audio_player_get_volume(void) {
    return s_volume;
}

/* === 运行时音频诊断 (remcon 的 `audio` 命令) 2026-10-09 加 ===
 * 把"软件以为的状态"和"硬件真值"一起打出来, 用于"初始化全成功但没声音":
 *   ① 先看软件状态对不对 (音量/静音/功放是否真的开了/平台判断对不对)
 *   ② 再看 ES8311 寄存器是不是真的被写进去了 —— 芯片被复位 / MCLK 缺失 / I2C 写失败都会露馅
 * 输出统一带 "ADIAG " / "ESREG " 前缀, 便于主机侧 grep。 */
void audio_player_diag(void)
{
    ESP_LOGI(TAG, "ADIAG state=%d volume=%d muted=%d amp_on=%d pa_on=%d pa_ready=%d",
             (int)s_state, s_volume, (int)s_muted, (int)s_amp_on, (int)s_pa_on, (int)s_pa_ready);
    ESP_LOGI(TAG, "ADIAG platform=%s es8311_present=%d i2s_rate=%d tx=%d rx=%d",
             s_use_ns4168_ctrl ? "NS4168" : "ES8311", (int)es8311_is_present(),
             s_current_i2s_rate, (int)(s_tx_chan != NULL), (int)(s_rx_chan != NULL));
    ESP_LOGI(TAG, "ADIAG pins pa=%d i2c_sda=%d i2c_scl=%d | i2s mclk=%d bclk=%d ws=%d dout=%d din=%d",
             (int)s_pa, s_i2c_sda, s_i2c_scl,
             (int)s_mclk, (int)s_bclk, (int)s_ws, (int)s_dout, (int)s_din);
    es8311_dump_regs();
}

/* 统一音量档位换算: 0-9 档 <-> 0-100 百分比.
 * [2026-10-14 linit 定案 · 二次上移] 曲线重锚:
 *   0 档 = 0% (静音)
 *   1 档 = 68%
 *   1..9 档在 68% ~ 100% 之间均匀分布 (每档 4%)
 *   => 0 / 68 / 72 / 76 / 80 / 84 / 88 / 92 / 96 / 100
 * 铺垫: "声音小"的真正根因在 **ES8311 的百分比→dB 曲线** (原来只铺到 0xC0 = 0dB),
 *   已按官方映射改成 0x00~0xFF ⇒ **0xBF=0dB, 75% 就满刻度, 76~100% 是数字增益**。
 *   于是这条 68~100% 的档位表落在 -9.5dB(1档) ~ +32dB(9档), **6 档=88%=+16.5dB 已很响**。
 *   ⛔ 别把基准再往上抬 (比如 88%) —— 那 9 档会全部挤在 +16~+32dB 的数字增益区, 满刻度音乐直接削波. */
int audio_player_vol_to_percent(int step) {
    if (step <= 0) return 0;
    if (step >= AUDIO_VOL_STEPS - 1) return 100;
    return AUDIO_VOL_BASE_PCT
         + (step - 1) * (100 - AUDIO_VOL_BASE_PCT) / (AUDIO_VOL_STEPS - 2);
}
int audio_player_vol_to_step(int percent) {
    if (percent <= 0) return 0;
    if (percent >= 100) return AUDIO_VOL_STEPS - 1;
    /* 逆映射: 线性扫描找最近档位 (只有 10 档, 开销可忽略) */
    int best = 1, best_d = 1000;
    for (int s = 1; s < AUDIO_VOL_STEPS; s++) {
        int p = audio_player_vol_to_percent(s);
        int d = (p > percent) ? (p - percent) : (percent - p);
        if (d < best_d) { best_d = d; best = s; }
    }
    return best;
}

/* 引脚有效性防护: 仅操作本模组(ESP32-S3-WROOM-1)引出且可作普通 IO 的脚.
 * 22-34 未引出 (含 26), 33-37 为 PSRAM/Flash 专用. */
static bool ns_ctrl_pin_ok(int pin) {
    if (pin < 0 || pin > 48) return false;
    if (pin >= 22 && pin <= 34) return false;
    if (pin >= 33 && pin <= 37) return false;
    return true;
}
/* NS4168 CTRL (GPIO46): 高=工作(右声道), 低=关断(静音省电). 校验不过则跳过, 不触碰硬件. */
static void ns_ctrl_enable(bool on) {
    if (!ns_ctrl_pin_ok(NS_CTRL_PIN)) return;
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << NS_CTRL_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = false, .pull_down_en = false,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(NS_CTRL_PIN, on ? 1 : 0);
}

void audio_player_set_muted(bool muted) {
    ESP_LOGI(TAG, "[diag] set_muted %d -> %d (use_ns=%d)", (int)s_muted, (int)muted, (int)s_use_ns4168_ctrl);
    if (s_muted == muted) return;
    s_muted = muted;
    if (muted) {
        audio_player_stop();          /* 停掉正在播放的音乐 */
        es8311_set_volume(0);         /* 编解码器音量拉到 0, 彻底静音 */
        if (s_use_ns4168_ctrl) ns_ctrl_enable(false);  /* NS4168: CTRL(46)拉低=硬件静音 */
        /* ★ 功放也要真的关断: 只把 ES8311 音量拉到 0 的话, 功放仍然带电工作 ——
         *   既白耗电, 又会把 codec 输出的微弱底噪放大成"嘶嘶"声。
         *   (小智模块的 LPA2103A 没有软静音寄存器, 关断只能靠这根 SD 脚。) */
        pa_enable(false);
        s_amp_on = false;             /* 与 PCM 空闲静音状态同步 */
    } else {
        /* ★ 2026-11-22: 解除静音统一走收敛点 —— 它会校验 codec 是否还在(刚经历过
         *   关断/掉电时自动重配)、**重新下发音量**、再按需开功放。
         *   原来这里只写 REG32: 芯片处于 power-down 时这次写等于丢弃,
         *   于是"开关拨回来了, 系统仍然没声音"。 */
        if (s_amp_on) {
            audio_output_begin();     /* 输出通路还在 → 就地恢复出声 */
        }
        if (s_use_ns4168_ctrl) ns_ctrl_enable(true);   /* NS4168: CTRL(46)拉高=恢复 */
        /* 输出通路没在跑时**不主动开功放**: 此刻没有音频, 开了只会出底噪。
         * 真正出声的路径(MP3 / PCM)会自己走 audio_output_begin()。 */
    }
    audio_snd_save_now();   /* ★ 唯一持久化点: 任何入口改静音都会落盘 */
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

/* V9.1: 跳转播放进度 (0..1000 千分比). 仅在播放/暂停中有效; 置请求标志,
 * 由解码任务循环内安全执行 fseek + 清空缓冲, 随后自动帧同步重对齐 (CBR 精确, VBR 近似). */
void audio_player_seek_to(int progress_0_1000) {
    if (progress_0_1000 < 0) progress_0_1000 = 0;
    if (progress_0_1000 > 1000) progress_0_1000 = 1000;
    if (!s_task_handle || !s_mp3_file) return;   /* 未在播放无法跳转 */
    s_seek_req_progress = progress_0_1000;
}

/* V9.1: 上一曲是否"自然播放结束" (EOF), 区别于用户主动 stop.
 * 一次性消费: 读取即清零, 避免页面 poll 重复触发自动切歌. */
bool audio_player_track_ended(void) {
    bool e = s_natural_end;
    s_natural_end = false;
    return e;
}

bool audio_player_is_running(void) {
    return s_task_handle != NULL;
}

void audio_player_deinit(void) {
    audio_player_stop();
    /* ★ 2026-10-28: 补关麦克风。原来 deinit 只停播放, 若此刻正在录音/频谱仪开着麦:
     *   s_mic_active 仍为 true、ES7210 继续供电(20~30mA), 而下一行 i2s_deinit() 已经把
     *   rx 通道拆掉 ⇒ 状态与实际硬件不一致, 之后 mic_read 全是空转。
     *   mic_stop 内部按 s_mic_active 判空, 未开麦时是空操作。 */
    audio_player_mic_stop();
    i2s_deinit();
    es8311_shutdown();
    pa_enable(false);
    s_hw_initialized = false;
}

/* ★ 进深睡前的一条龙关断 (2026-10-09) —— 见 audio_player.h 的说明。
 * 顺序: 先给 codec 下电(I2C 此刻还活着), 再关功放并 hold 低电平。 */
/* ★★★ 2026-10-28 linit「睡眠时候提前睡眠功放 / 麦克风 / 音频芯片」★★★
 *   动机: 关机流程里 gps_boot_park() 要 4~15 秒(GPS 停靠: 装UART+等稳态+PMREQ+PSMOO 窗口),
 *   而音频关断原来排在它**后面** ⇒ 这十几秒 ES8311(5~10mA)/ES7210(20~30mA)/功放 还挂着。
 *   所以拆成两步:
 *     · 本函数(early): 立刻关 codec + I2S + 功放拉低；
 *       **故意不调 gpio_deep_sleep_hold_en()** —— 那是全局冻结所有 pad，
 *       紧接着要发的 GPS PMREQ(UBX) 会发不出去(TX pad 被锁)。
 *       单脚 gpio_hold_en(pa) 是安全的，GPS 驱动那边会自己 gpio_hold_dis(TX) 再发。
 *     · audio_player_sleep_for_off(): GPS 之后再调一次(幂等)，补 global hold/深睡锁存。 */
void audio_player_sleep_for_off_early(void)
{
    if (s_hw_initialized && s_mutex) audio_player_stop();
    es8311_shutdown();          /* 喇叭 codec: 0x32=0 静音 + 0x00=0 掉电 */
    es7210_deinit();            /* 麦克风 ADC: 探到就会跑, 不关就是 20~30mA */
    i2s_deinit();               /* 通道禁用的话 MCLK/BCLK 还在跑, codec 会"有钟无数据"耗电 */
    audio_pa_init();
    if (s_pa >= 0) {
        gpio_set_level(s_pa, 0);
        gpio_hold_en(s_pa);
        s_pa_on = false;
    }
    ESP_LOGI(TAG, "[early] 音频已提前关断: ES8311/ES7210/I2S/功放(未做全局 pad 冻结)");
}

void audio_player_sleep_for_off(void)
{
    /* 先把播放/输出任务收干净, 再动通道 —— 否则任务可能正卡在 i2s_channel_write
     * 或持有 s_i2s_mutex, 直接删通道会卡死关机流程(电就一直耗着)。 */
    if (s_hw_initialized && s_mutex) audio_player_stop();
    es8311_shutdown();       /* ES8311: 0x32=0(静音) + 0x00=0(power down) */
    /* ★ 2026-10-24: 麦克风 ADC(ES7210) 和它供的 MICBIAS 原来只在"录音结束"时才关，
     *   深睡路径没关 → 整夜常供 1~2mA。这里补上（deinit 内部幂等）。 */
    es7210_deinit();
    /* ★ 2026-10-20: 把 I2S 收发通道整条拆掉 —— 通道只是 disable 的话,
     *   MCLK/BCLK 仍由 I2S 外设输出, ES8311/ES7210 会一直"有钟无数据"地挂着耗电;
     *   拆掉后外设时钟全停, 深睡电流回到 µA 级。 */
    i2s_deinit();
    audio_pa_force_off();    /* 功放 OFF + gpio_hold_en + gpio_deep_sleep_hold_en */
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
        /* ★★★ 2026-10-25 修复「进游戏后没声音且音频任务静默卡死」:
         *   原来这里是 `while (s_task_handle) vTaskDelay(10)` —— **无超时死等**。
         *   MP3 任务只有在 976 行那条正常退出路径上才会把句柄清 NULL;
         *   一旦它因任何原因没走到那里(被强杀/卡在 I2S 写/异常返回), 句柄永久非 NULL
         *   ⇒ 第一个调 feed_pcm 的引擎(仙剑 sdl_aud)就永久卡在这行:
         *     不再产出 PCM → audio_out 300ms 后触发"空闲静音"关功放 →
         *     实测日志: `功放使能 GPIO46=0` 之后**再也不出现任何 SDLAUD 行**,
         *     而游戏本身还在正常运行(PSRAM 仍是游戏占用)。用户听到的就是"进游戏没声音"。
         *   现在最多等 500ms, 还不退就强杀并清句柄 —— MP3 是后台播放, 杀掉不影响游戏,
         *   总比把整个音频链路锁死好。 */
        /* ★ 2026-11-22 【第三轮】绝不强杀（理由见 audio_wait_task_gone 上方长注释）。
         *   等不到就本次返回 0：游戏这一帧没声音，但下次调用会再试，且
         *   **不会**去抢 MP3 任务正在写的 I2S。杀它只会把全局 stdio 链表踩坏。 */
        if (!audio_wait_task_gone(MP3_STOP_WAIT_MS)) {
            ESP_LOGW(TAG, "MP3 任务 %dms 未退 ⇒ 本次 PCM 输出放弃（不杀、不抢 I2S）",
                     MP3_STOP_WAIT_MS);
            return 0;
        }
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
            ESP_LOGD(TAG, "feed_pcm: 音频硬件初始化失败, 无法输出音频");
            return 0;
        }
        if (sample_rate > 0) i2s_init(sample_rate);
        ESP_LOGI(TAG, "feed_pcm: 音频硬件初始化完成 (sr=%d)", sample_rate);
    }

    /* 首次调用时启动音频输出任务 */
    if (!s_audio_out_running) {
        audio_out_start();
    }
    /* ★★★ 2026-11-08 修「录音机回放经常没声音」★★★
     *   2026-11-07「音频空闲彻底休眠」后, 空闲 2.5s 会 audio_out_idle_off():
     *   关功放 + disable I2S 通道 + s_amp_on=false, 但 **s_audio_out_running 仍是 true**。
     *   于是上面的 audio_out_start() 走"任务已存在"分支直接 return, 不会 pa_enable(true);
     *   恢复原本只能靠 audio_out_task 被 notify 唤醒后异步走到 ensure_out_active()。
     *   而旧代码把 notify 发在"写环形缓冲"之前: audio_out_task 是 prio8、比 WAV 任务
     *   (prio7) 高, 一被唤醒就抢占, 此时缓冲还是空的 ⇒ avail==0 ⇒ 又睡回
     *   ulTaskNotifyTake(200), 通知被消费掉。短录音一次灌完后再没有第二次 feed 补发,
     *   功放/I2S 最长关 200ms; 任务若没在跑则整段录音静音。
     *   表现正是"有时候有声、有时候没声": 没出过声时 audio_out 任务不存在→start()会开
     *   功放; 玩过游戏/听过音乐后任务常驻并空闲休眠→功放是关的→回放静音但进度条照走。
     *   修法: ① feed 线程里同步把输出通路拉起来(ensure_out_active 幂等, 已开则立即返回),
     *         不再依赖异步唤醒; ② notify 挪到数据落缓冲之后。 */
    if (!s_amp_on || !s_pa_on) {
        ensure_out_active();
    }

    if (!s_pcm_ring) return 0;

    size_t written = 0;
    /* ★ 2026-10-25 性能: 原来逐帧(2 样本)+每帧一次 % 取模 → 分两段 memcpy 块拷贝.
     *   语义保持: head+2==tail 视为满(保留一帧间隙), 缓冲满时丢弃剩余帧并返回已写帧数。 */
    while (written < frames) {
        uint32_t head = s_ring_head, tail = s_ring_tail;
        uint32_t used = (head >= tail) ? (head - tail) : (PCM_RING_SIZE - tail + head);
        uint32_t free_samples = PCM_RING_SIZE - 2 - used;      /* 预留一帧间隙 */
        if (free_samples < 2) break;                            /* 缓冲满 */
        size_t can = free_samples / 2;
        size_t want = frames - written;
        if (can > want) can = want;
        size_t n = can * 2;                                     /* 本次要写的样本数 */
        uint32_t cont = PCM_RING_SIZE - head;                   /* 到缓冲末端的连续空间 */
        if (n > cont) n = cont;
        memcpy(&s_pcm_ring[head], &data[written * 2], n * sizeof(int16_t));
        head += (uint32_t)n;
        if (head >= PCM_RING_SIZE) head -= PCM_RING_SIZE;
        s_ring_head = head;
        written += n / 2;
    }
    /* ★ 2026-11-08: 唤醒必须在数据落缓冲之后。放在写之前会让高优先级
     *   audio_out_task 醒来看到 avail==0, 重新睡下并把这次通知消费掉。 */
    if (written > 0 && s_audio_out_task) xTaskNotifyGive(s_audio_out_task);
    return written;
}

/* 立即清空 PCM 环形缓冲 (按键音效释放时切断尾音) */
void audio_player_flush_pcm(void)
{
    s_pcm_flush_requested = true;
}

/* === 音效混音缓冲（★ 2026-10-25）===
 * MP3 模式下 I2S 归 MP3 解码任务独占，SDLPal 的音效合成结果没地方去。
 * sdl_audio.c 每块把音效 PCM 推进来，MP3 解码循环写 I2S 前叠加。
 * ⚠ 缓冲**必须大于 sdl_audio 单次推入量**，否则边写边丢、等于没声音：
 *   块长 2048 帧 → 一次推 4096 个 int16。所以取 8192 个 int16（4096 帧 ≈ 93ms），
 *   能装 2 批，延迟比原来 32768(372ms) 小 4 倍，同时保证不会写出即丢。 */
#define FX_RING_SIZE 32768
static int16_t *s_fx_ring = NULL;
static volatile uint32_t s_fx_head = 0, s_fx_tail = 0;

void audio_player_fx_reset(void)
{
    s_fx_head = s_fx_tail = 0;
}

void audio_player_fx_push(const int16_t *data, int frames)
{
    if (!data || frames <= 0) return;
    if (!s_fx_ring) {
        s_fx_ring = (int16_t *)heap_caps_calloc(FX_RING_SIZE, sizeof(int16_t),
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_fx_ring) s_fx_ring = (int16_t *)calloc(FX_RING_SIZE, sizeof(int16_t));
        if (!s_fx_ring) return;
    }
    for (int i = 0; i < frames; i++) {
        uint32_t next = (s_fx_head + 2) % FX_RING_SIZE;
        if (next == s_fx_tail) {
            /* 满: 丢最旧的一帧，保证延迟不累积 */
            s_fx_tail = (s_fx_tail + 2) % FX_RING_SIZE;
        }
        s_fx_ring[s_fx_head]     = data[i * 2];       /* L */
        s_fx_ring[s_fx_head + 1] = data[i * 2 + 1];   /* R */
        s_fx_head = next;
    }
}

void audio_player_fx_mix(int16_t *pcm, uint32_t samples)
{
    if (!s_fx_ring || !pcm) return;
    for (uint32_t i = 0; i < samples; i++) {
        if (s_fx_tail == s_fx_head) break;            /* 音效缓冲已空 */
        int32_t v = (int32_t)pcm[i] + (int32_t)s_fx_ring[s_fx_tail];
        if (v > 32767)  v = 32767;                    /* 防削波 */
        if (v < -32768) v = -32768;
        pcm[i] = (int16_t)v;
        s_fx_tail = (s_fx_tail + 1) % FX_RING_SIZE;
    }
}

/* 按键反馈音选项已移除 (见 menu_system 游戏设置): 不再提供蜂鸣声. */

/* ================= 录音机：WAV 录制 + WAV 播放（2026-10-27 v2） =================
 *
 * 与 v1 的区别（都是实测踩出来的）：
 *  ① 录音任务栈 12KB、PCM 缓冲放堆 —— 4KB 栈 + 栈上 2KB 缓冲会在 fwrite 时溢出重启
 *  ② stop **不阻塞**：只置标志就返回（调用方是 Lua 主循环，等 1 秒会触发 task_wdt）
 *     收尾（回填 WAV 头 / 关文件 / 关麦 / 自杀）全在任务里做
 *  ③ 每 5 秒 fflush，掉电只丢最后几秒
 *  ④ 录音时算峰值 + 16 段频谱，供 UI 画电平/柱状
 *  ⑤ WAV 播放走 audio_player_feed_pcm（不能直接写 I2S：audio_out 空闲 300ms 会关通道），
 *     进度按**墙上时间**推进（按已喂字节算会在缓冲灌满时猛跳）
 */
#define REC_CH        2
#define REC_RATE_DEF  16000
#define REC_STACK     12288
#define REC_PRIO      8
#define SPEC_BINS     16
#define REC_SPIKE_TH  30000   /* ★ 2026-11-23: |样点|≥此值视为满幅尖峰(实测 ±32768), 见 rec_task */

static TaskHandle_t s_rec_task = NULL;
static volatile bool s_rec_stop = false;
static FILE *s_rec_fp = NULL;
static volatile uint32_t s_rec_bytes = 0;
static int s_rec_rate = REC_RATE_DEF;
static volatile uint32_t s_rec_ms = 0;
static volatile int32_t  s_rec_peak = 0;
/* ★ 诊断(2026-10-30): 按声道分别统计峰值和 RMS，用来判断"录音声音小"到底是
 *   ① PGA 增益不够  ② 声道接错(只读了没接麦的那一路)  ③ 真的是麦克风密封/结构问题 */
static volatile int32_t  s_rec_pk[4] = {0,0,0,0};
/* ★ 2026-10-30 录音数字自动增益(AGC)。
 *   为什么要它: 麦克风经 37.5dB PGA 后, 正常说话峰值仍只有满量程的 5~15%,
 *   而音乐(MP3)是母带级 ~90%FS ⇒ 同一音量下回放录音明显偏小(差 15~25dB)。
 *   这里在**写入文件前**把电平抬到目标附近, 使录音回放的响度与音乐接近。
 *   规则刻意保守:
 *     · 增益范围 1x~12x(+21.6dB), 只放大不压缩动态;
 *     · 每块再按瞬时峰值限一次, 保证任何情况下不削波;
 *     · 包络有 200 的底噪门限, 安静时不会把嘶声一起抬起来。 */
static float s_agc_env = 0.0f;     /* 平滑包络(峰值域) */
static int   s_agc_q15 = 32768;    /* 当前增益 Q15, 1.0 = 32768 */

static void rec_agc_apply(int16_t *pcm, int frames, int ch)
{
    if (!pcm || frames <= 0) return;
    if (ch < 1) ch = 1;
    int n = frames * ch;
    int bpk = 0;
    for (int i = 0; i < n; i++) {
        int v = pcm[i];
        if (v < 0) v = -v;
        if (v >= REC_SPIKE_TH) continue;   /* ★ 2026-11-23: 满幅尖峰不参与包络, 否则整块增益被压到 1x */
        if (v > bpk) bpk = v;
    }

    float b = (float)bpk;
    if (b > s_agc_env) s_agc_env = s_agc_env * 0.5f + b * 0.5f;    /* 起得快 */
    else               s_agc_env = s_agc_env * 0.98f + b * 0.02f;  /* 落得慢 */
    if (s_agc_env < 200.0f) s_agc_env = 200.0f;                    /* 底噪门限 */

    const float TARGET = 14000.0f;                                  /* ≈ -7.4 dBFS */
    float g = TARGET / s_agc_env;
    if (g < 1.0f)  g = 1.0f;
    if (g > 12.0f) g = 12.0f;

    int gq = (int)(g * 32768.0f);
    s_agc_q15 = (s_agc_q15 * 3 + gq) / 4;                           /* 平滑, 防"呼吸" */
    int gq_lim = (bpk > 0) ? (int)(30000.0f * 32768.0f / (float)bpk) : 262144;
    int use = (s_agc_q15 < gq_lim) ? s_agc_q15 : gq_lim;            /* 本块硬限幅 */
    if (use < 16384) use = 16384;                                   /* 最多衰减到 0.5x */
    /* ★ 2026-11-23: 纯本底噪声块直通不放大。实测安静块包络 ~500、说话块 ≥1200,
     *   原来静音块也吃满 12x ⇒ 回放"底噪非常大"盖过人声。 */
    if (bpk < 800) use = 32768;

    for (int i = 0; i < n; i++) {
        int32_t v = ((int32_t)pcm[i] * use) >> 15;
        if (v > 32767) v = 32767;
        else if (v < -32768) v = -32768;
        pcm[i] = (int16_t)v;
    }
}
static uint64_t s_rec_sq[4] = {0,0,0,0};
static uint32_t s_rec_nsamp = 0;
static volatile uint8_t  s_spec[SPEC_BINS];
EXT_RAM_BSS_ATTR static char s_rec_path[192];   /* 只在录音机里用 */

static TaskHandle_t s_wav_task = NULL;
static volatile bool s_wav_stop = false;
static volatile uint32_t s_wav_ms = 0;
static volatile uint32_t s_wav_total_ms = 0;
EXT_RAM_BSS_ATTR static char s_wav_path[192];   /* 只在播放 WAV 时用 */

/* 预载的 WAV 缓存 (PSRAM)。见 audio_player.h 的说明:
 * 短音效读一次就常驻, 播放时零 IO ⇒ 不关 cache、不卡 UI、不碰 PSRAM 崩溃路径。 */
static uint8_t *s_wav_cache     = NULL;
static size_t   s_wav_cache_len = 0;
static bool     s_wav_use_cache = false;   /* 本次 wavply 任务的数据来源 */

static void wav_header(uint8_t *h, uint32_t data_bytes, int rate, int ch, int bits)
{
    uint32_t v32; uint16_t v16;
    uint32_t byte_rate = (uint32_t)rate * (uint32_t)ch * (uint32_t)bits / 8u;
    uint16_t align     = (uint16_t)(ch * bits / 8);
    memcpy(h + 0, "RIFF", 4);
    v32 = 36u + data_bytes;      memcpy(h + 4,  &v32, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    v32 = 16u;                   memcpy(h + 16, &v32, 4);
    v16 = 1;                     memcpy(h + 20, &v16, 2);
    v16 = (uint16_t)ch;          memcpy(h + 22, &v16, 2);
    v32 = (uint32_t)rate;        memcpy(h + 24, &v32, 4);
    v32 = byte_rate;             memcpy(h + 28, &v32, 4);
    v16 = align;                 memcpy(h + 32, &v16, 2);
    v16 = (uint16_t)bits;        memcpy(h + 34, &v16, 2);
    memcpy(h + 36, "data", 4);
    v32 = data_bytes;            memcpy(h + 40, &v32, 4);
}

/* 16 段 Goertzel（0~8kHz 均分），只取左声道 */
static void calc_spectrum(const int16_t *pcm, int frames)
{
    if (frames < 64) return;
    int n = frames > 512 ? 512 : frames;
    for (int b = 0; b < SPEC_BINS; b++) {
        float f = (float)(b + 1) * 8000.0f / (float)SPEC_BINS;
        float w = 2.0f * 3.14159265f * f / 16000.0f;
        float coeff = 2.0f * cosf(w);
        float s1 = 0.0f, s2 = 0.0f;
        for (int i = 0; i < n; i++) {
            float x = (float)pcm[i * REC_CH];
            float s0 = x + coeff * s1 - s2;
            s2 = s1; s1 = s0;
        }
        float mag = sqrtf(s1 * s1 + s2 * s2 - coeff * s1 * s2) / (float)(n / 2);
        int v = (int)(20.0f * log10f(mag + 1.0f));
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        s_spec[b] = (uint8_t)v;
    }
}

static void rec_task(void *arg)
{
    (void)arg;
    int16_t *buf = heap_caps_malloc(512 * REC_CH * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = malloc(512 * REC_CH * sizeof(int16_t));
    if (!buf) {
        ESP_LOGE(TAG, "录音缓冲分配失败");
        if (s_rec_fp) { fclose(s_rec_fp); s_rec_fp = NULL; }
        s_rec_task = NULL;
        vTaskDeleteWithCaps(NULL);   /* WithCaps 创建 ⇒ 必须 WithCaps 删除, 否则栈+TCB 泄漏 */
        return;
    }
    TickType_t t0 = xTaskGetTickCount();
    uint32_t last_flush = 0, spec_tick = 0;
    s_agc_env = 0.0f; s_agc_q15 = 32768;     /* 每条录音从 1x 起步 */

    while (!s_rec_stop) {
        int n = audio_player_mic_read(buf, 512, 100);
        if (n > 0) {
            /* ★★ 2026-11-23 满幅尖峰抑制（AGC 崩溃的元凶）★★
             *   实测本机录音里有 ±32768 的孤立尖峰（ES8311/功放与麦克风共电源/地时上电串扰），
             *   一个尖峰就把整块包络顶满 ⇒ AGC 限幅把增益压到 1x，人声得不到任何放大。
             *   这里在统计/AGC/写文件之前，把越界样点替换为同声道前一个有效样点。 */
            {
                int16_t last[4] = {0, 0, 0, 0};
                for (int i = 0; i < n; i++) {
                    for (int c = 0; c < REC_CH && c < 4; c++) {
                        int16_t *p = &buf[i * REC_CH + c];
                        if (*p >= REC_SPIKE_TH || *p <= -REC_SPIKE_TH) *p = last[c];
                        else last[c] = *p;
                    }
                }
            }
            /* ★ 2026-11-23: 声道峰值/RMS 改在 **AGC 之前**按原始电平统计。
             *   原因: AGC 是两路**共用同一个增益**(rec_agc_apply 对一个 buf 只算一次 use),
             *   之前统计的是放大后的值 ⇒ 无法判断"麦克风本身输出多大"。
             *   排查虚焊/灵敏度差异需要看 ADC 原始幅度, 所以先统计再放大。 */
            for (int i = 0; i < n; i++) {
                for (int c = 0; c < REC_CH && c < 4; c++) {
                    int x = buf[i * REC_CH + c];
                    int ax = (x < 0) ? -x : x;
                    if (ax > s_rec_pk[c]) s_rec_pk[c] = ax;
                    s_rec_sq[c] += (uint64_t)((int32_t)x * (int32_t)x);
                }
                s_rec_nsamp++;
            }
            /* ★★ 2026-11-23 双麦抗相消整合（linit: 没问题就使用双麦克风）★★
             *   逐块比较 (L+R) 与 (L-R) 的能量, 取能量大的组合 —— 两麦反相时 (L+R) 相消、
             *   (L-R) 不相消, 反之亦然 ⇒ 自动选到不相消的那一路, 既不丢一只麦的增益,
             *   也不会出现"双麦互相抵消、人声几乎听不到"。结果写回两槽, WAV 仍是立体声。 */
            if (REC_CH >= 2) {
                int64_t e_sum = 0, e_dif = 0;
                for (int i = 0; i < n; i++) {
                    int32_t l = buf[i * REC_CH], r = buf[i * REC_CH + 1];
                    int32_t s = l + r, d = l - r;
                    e_sum += (int64_t)s * (int64_t)s;
                    e_dif += (int64_t)d * (int64_t)d;
                }
                int use_dif = (e_dif > e_sum);
                for (int i = 0; i < n; i++) {
                    int32_t l = buf[i * REC_CH], r = buf[i * REC_CH + 1];
                    int32_t v = (use_dif ? (l - r) : (l + r)) / 2;
                    if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
                    buf[i * REC_CH] = (int16_t)v;
                    for (int c = 1; c < REC_CH && c < 4; c++) buf[i * REC_CH + c] = (int16_t)v;
                }
            }
            rec_agc_apply(buf, n, REC_CH);      /* ★ 写文件前先做电平归一 */
            size_t w = fwrite(buf, sizeof(int16_t) * REC_CH, (size_t)n, s_rec_fp);
            if (w != (size_t)n) {
                ESP_LOGE(TAG, "录音写卡失败 (%u/%d)", (unsigned)w, n);
                break;
            }
            s_rec_bytes += (uint32_t)n * REC_CH * 2u;
            int pk = s_rec_peak;
            for (int i = 0; i < n; i++) {
                int v = buf[i * REC_CH];
                if (v < 0) v = -v;
                if (v > pk) pk = v;
            }
            s_rec_peak = pk;
            if ((spec_tick & 3) == 0) calc_spectrum(buf, n);
            spec_tick++;
        }
        s_rec_ms = (uint32_t)((xTaskGetTickCount() - t0) * portTICK_PERIOD_MS);
        if (s_rec_ms - last_flush >= 5000) {
            last_flush = s_rec_ms;
            if (s_rec_fp) fflush(s_rec_fp);
        }
    }
    free(buf);
    if (s_rec_fp) {
        uint8_t h[44];
        wav_header(h, s_rec_bytes, s_rec_rate, REC_CH, 16);
        fseek(s_rec_fp, 0, SEEK_SET);
        fwrite(h, 1, 44, s_rec_fp);
        fflush(s_rec_fp);
        fclose(s_rec_fp);
        s_rec_fp = NULL;
    }
    audio_player_mic_stop();          /* 关麦放任务里，别让调用方等 */
    {   /* ★ 诊断(2026-10-30): 用实测 B/s 反推采集速率。
         *   16000Hz × REC_CH × 16bit 的期望值 = 64000 B/s。
         *   明显偏高 ⇒ 麦克风实际跑在更高采样率, 但文件头写的是 16k ⇒ 播放必然变快。 */
        unsigned exp_bps = (unsigned)s_rec_rate * REC_CH * 2u;
        unsigned act_bps = (s_rec_ms > 0) ? (unsigned)((uint64_t)s_rec_bytes * 1000ull / s_rec_ms) : 0;
        ESP_LOGI(TAG, "录音结束: %s  %u B  %u ms  峰值=%d%s",
                 s_rec_path, (unsigned)s_rec_bytes, (unsigned)s_rec_ms, (int)s_rec_peak,
                 (s_rec_peak == 0) ? "  (全程无信号)" : "");
        ESP_LOGI(TAG, "录音速率: 实测 %u B/s, 文件头声称 %u Hz → 期望 %u B/s %s",
                 act_bps, (unsigned)s_rec_rate, exp_bps,
                 (exp_bps && act_bps > exp_bps + exp_bps / 10) ? "  ⚠ 采集比声称快" : "");
        {   /* 按声道电平: 峰值 %FS 与 RMS dBFS —— 直接判断"收得小"还是"声道没接"。
             * ★ 2026-11-23: 数值是 **AGC 前**的 ADC 原始电平（见 rec_task 统计位置），
             *   两路差异才是麦克风本身的差异；< -90 dBFS 视为该路完全没信号。 */
            uint32_t ns = s_rec_nsamp ? s_rec_nsamp : 1;
            for (int c = 0; c < REC_CH && c < 4; c++) {
                double ms = (double)s_rec_sq[c] / (double)ns;
                double rms = sqrt(ms);
                int db = (rms > 0.5) ? (int)(20.0 * log10(rms / 32768.0)) : -99;
                ESP_LOGI(TAG, "  声道%d(原始): 峰值 %d (%d%%FS)  RMS %d dBFS%s",
                         c, (int)s_rec_pk[c], (int)s_rec_pk[c] * 100 / 32768, db,
                         (db <= -90 && s_rec_bytes == 0) ? "" : (db <= -90 ? "  ← 该路无信号" : ""));
            }
        }
    }
    s_rec_task = NULL;
    vTaskDeleteWithCaps(NULL);   /* WithCaps 创建 ⇒ 必须 WithCaps 删除, 否则栈+TCB 泄漏 */
}

int audio_player_rec_start(const char *path, int sample_rate)
{
    if (!path || !path[0] || s_rec_task) return -1;
    if (sample_rate <= 0) sample_rate = REC_RATE_DEF;
    audio_player_init();
    if (audio_player_mic_start(sample_rate) != 0) {
        ESP_LOGE(TAG, "麦克风启动失败");
        return -2;
    }
    s_rec_fp = fopen(path, "wb");
    if (!s_rec_fp) {
        audio_player_mic_stop();
        ESP_LOGE(TAG, "无法创建 %s", path);
        return -3;
    }
    uint8_t h[44];
    wav_header(h, 0, sample_rate, REC_CH, 16);
    fwrite(h, 1, 44, s_rec_fp);

    s_rec_rate    = sample_rate;
    s_rec_bytes   = 0;
    s_rec_ms      = 0;
    s_rec_peak    = 0;
    s_rec_stop    = false;
    /* ★ 2026-11-23: 声道统计必须每条录音清零。
     *   之前不清 ⇒ 日志打印的是**上一条(甚至开机以来第一条)的残留峰值**，
     *   实测出现过「0 B / 全程无信号」却报「声道0 峰值 30000」的自相矛盾数据，
     *   差点据此误判麦克风焊接。诊断数据必须与实际这条录音一一对应。 */
    memset((void *)s_rec_pk, 0, sizeof(s_rec_pk));
    memset(s_rec_sq, 0, sizeof(s_rec_sq));
    s_rec_nsamp   = 0;
    memset((void *)s_spec, 0, sizeof(s_spec));
    strncpy(s_rec_path, path, sizeof(s_rec_path) - 1);
    s_rec_path[sizeof(s_rec_path) - 1] = 0;

    /* ★★★ 2026-11-23 同一根因(见 audio_player_play_wav 里 wavply 的长注释):
     *   rec_task 每条录音都在循环里 fwrite 文件; 路径若落在 SPI flash (/store 等
     *   SPIFFS) 上, 写 flash 同样要关 cache ⇒ PSRAM 栈不可访问 ⇒ IllegalInstruction。
     *   12KB 栈放内部 RAM 完全够 (内部最大连续块 ~139KB), 故优先内部。 */
    if (xTaskCreateWithCaps(rec_task, "rec", REC_STACK, NULL, REC_PRIO,
                            &s_rec_task, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGW(TAG, "内部 RAM 建 rec 任务失败 ⇒ 退回 PSRAM (写 /store 文件有 panic 风险)");
        if (xTaskCreateWithCaps(rec_task, "rec", REC_STACK, NULL, REC_PRIO,
                                &s_rec_task, MALLOC_CAP_SPIRAM) != pdPASS) {
            fclose(s_rec_fp); s_rec_fp = NULL;
            audio_player_mic_stop();
            s_rec_task = NULL;
            return -4;
        }
    }
    ESP_LOGI(TAG, "开始录音: %s (%d Hz)", s_rec_path, sample_rate);
    return 0;
}

void audio_player_rec_stop(void)
{
    if (!s_rec_task) return;
    /* 只置标志就返回：调用方是 Lua 主循环，等 1 秒会触发 task_wdt 重启 */
    s_rec_stop = true;
}

bool audio_player_rec_active(void)   { return s_rec_task != NULL; }
uint32_t audio_player_rec_ms(void)   { return s_rec_ms; }
uint32_t audio_player_rec_bytes(void){ return s_rec_bytes; }
const char *audio_player_rec_path(void) { return s_rec_path; }
int audio_player_rec_peak(void)      { return (int)s_rec_peak; }
int audio_player_rec_spec(int bin)   { return (bin < 0 || bin >= SPEC_BINS) ? -1 : (int)s_spec[bin]; }
int audio_player_rec_spec_bins(void) { return SPEC_BINS; }

/* 板上有麦克风吗（标准扩展板 i2s_din = -1 ⇒ 物理上没有输入脚） */
bool audio_player_mic_available(void) { return s_din >= 0; }

/* 环形缓冲里还有多少帧没播（快照；单写多读，够用） */
static uint32_t wav_ring_frames(void)
{
    uint32_t h = s_ring_head, t = s_ring_tail;
    if (h == t) return 0;
    uint32_t used = (h >= t) ? (h - t) : (PCM_RING_SIZE - t + h);
    return used / 2;
}

/* ---------------- WAV 播放 ---------------- */
static void wav_play_task(void *arg)
{
    (void)arg;
    /* 数据源二选一 (2026-11-23 预载缓存):
     *   · 预载缓存 → 直接吃 PSRAM 里的字节, **全程不碰 flash、不关 cache**;
     *   · 否则照旧 fopen/fread 文件。
     * 缓存路径不只是省掉 52KB/188ms 的读取, 还彻底绕开了
     * "读 /store 要关 cache ⇒ 期间 PSRAM 不可访问 ⇒ 栈在 PSRAM 就崩" 那条路径
     * (见 audio_player_play_wav 里那大段 PANIC 记录)。 */
    FILE *fp = NULL;
    const uint8_t *mem = NULL;
    size_t mem_len = 0, mem_pos = 0;
    if (s_wav_use_cache && s_wav_cache && s_wav_cache_len >= 44) {
        mem = s_wav_cache;
        mem_len = s_wav_cache_len;
    } else {
        fp = fopen(s_wav_path, "rb");
        if (!fp) { s_wav_task = NULL; vTaskDeleteWithCaps(NULL); return; }
    }

    uint8_t hdr[44];
    if (mem) {
        memcpy(hdr, mem, 44);
        mem_pos = 44;
    } else if (fread(hdr, 1, 44, fp) != 44) {
        fclose(fp); s_wav_task = NULL; vTaskDeleteWithCaps(NULL); return;
    }
    if (memcmp(hdr, "RIFF", 4) != 0) {
        if (fp) fclose(fp);
        s_wav_task = NULL; vTaskDeleteWithCaps(NULL); return;
    }
    uint16_t ch, bits; uint32_t rate, data_len;
    memcpy(&ch, hdr + 22, 2);
    memcpy(&rate, hdr + 24, 4);
    memcpy(&bits, hdr + 34, 2);
    memcpy(&data_len, hdr + 40, 4);
    if (ch == 0 || ch > 2) ch = 2;
    if (bits != 16) bits = 16;
    if (rate < 8000 || rate > 48000) rate = 16000;

    uint32_t frame_bytes = (uint32_t)ch * (uint32_t)bits / 8u;
    if (frame_bytes == 0) frame_bytes = 4;
    s_wav_total_ms = (uint32_t)((uint64_t)data_len * 1000ull / ((uint64_t)rate * frame_bytes));
    s_wav_ms = 0;
    ESP_LOGI(TAG, "WAV 播放: %u Hz  %u ch  %u bit  data=%u B  → 预计 %u ms  [%s]",
             (unsigned)rate, (unsigned)ch, (unsigned)bits,
             (unsigned)data_len, (unsigned)s_wav_total_ms,
             mem ? "PSRAM预载,零IO" : s_wav_path);

    const size_t CHUNK = 2048;
    uint8_t *buf = heap_caps_malloc(CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = malloc(CHUNK);

    /* ★ 2026-11-23: 上一次 stop 请求的清空可能还没被 audio_out_task 执行。
     *   如果这时就开始 feed, 那个迟到的 flush 会把**新一声的开头**一起清掉
     *   —— 快速连点时听起来就是"起音缺一块/没重新放"。
     *   在本任务里等它落地 (不阻塞 UI 主任务, 本任务 prio 7 < audio_out 的 8),
     *   最多 30ms 防死等 (audio_out 未运行时标志没人清)。 */
    for (int i = 0; i < 15 && s_pcm_flush_requested; i++) vTaskDelay(pdMS_TO_TICKS(2));

    uint32_t sent = 0;
    int stall = 0;
    TickType_t t0 = xTaskGetTickCount();
    while (!s_wav_stop && sent < data_len && buf) {
        size_t want = data_len - sent;
        if (want > CHUNK) want = CHUNK;
        size_t got;
        if (mem) {                                  /* 预载缓存: 纯 memcpy, 不碰 flash */
            size_t remain = (mem_pos < mem_len) ? (mem_len - mem_pos) : 0;
            got = (want < remain) ? want : remain;
            if (got) { memcpy(buf, mem + mem_pos, got); mem_pos += got; }
        } else {
            got = fread(buf, 1, want, fp);
        }
        if (got == 0) break;
        size_t frames = got / frame_bytes, off = 0;
        while (off < frames && !s_wav_stop) {
            size_t w = audio_player_feed_pcm((const int16_t *)(buf + off * frame_bytes),
                                             frames - off, (int)rate);
            if (w == 0) {
                if (++stall > 200) { ESP_LOGW(TAG, "PCM 输出 2s 无空间, 停止播放"); goto done; }
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            stall = 0;
            off += w;
            sent += (uint32_t)(w * frame_bytes);
            s_wav_ms = (uint32_t)((xTaskGetTickCount() - t0) * portTICK_PERIOD_MS);
            if (s_wav_ms > s_wav_total_ms) s_wav_ms = s_wav_total_ms;
        }
    }
done:
    if (buf) free(buf);
    if (fp) fclose(fp);

    /* ★★★ 2026-10-30 修「录音播放快 5-6 倍」：
     *   PCM 环形缓冲有 131072 样本 = 65536 帧 ≈ **4 秒**。
     *   录音机回放时把整段(<4s)一次性推进缓冲只要几百毫秒 ⇒ 这里立刻收尾、
     *   s_wav_task 置 NULL ⇒ 上层 playing() 变 false ⇒ 播放页直接切走、
     *   甚至把缓冲冲掉。用户听到的就是"一小段就没了"，听起来像 5~6 倍速。
     *   正确做法：数据进队列 ≠ 播完，必须等缓冲真正排空再收尾。
     *   超时上限给 total+3s，防止下游异常时卡在这里退不出去。 */
    uint32_t guard_ms = 0, cap_ms = s_wav_total_ms + 3000u;
    while (!s_wav_stop && wav_ring_frames() > 0 && guard_ms < cap_ms) {
        vTaskDelay(pdMS_TO_TICKS(20));
        guard_ms += 20;
        s_wav_ms = (uint32_t)((xTaskGetTickCount() - t0) * portTICK_PERIOD_MS);
        if (s_wav_ms > s_wav_total_ms) s_wav_ms = s_wav_total_ms;
    }
    {
        uint32_t elapsed = (uint32_t)((xTaskGetTickCount() - t0) * portTICK_PERIOD_MS);
        ESP_LOGI(TAG, "WAV 播放结束: 预计 %u ms, 实际 %u ms, 剩余未播 %u 帧", 
                 (unsigned)s_wav_total_ms, (unsigned)elapsed, (unsigned)wav_ring_frames());
        ESP_LOGI(TAG, "  ⓘ i2s=%dHz desired=%dHz  文件帧数=%u  实测播放率=%uHz",
                 audio_player_get_i2s_rate(), (int)s_desired_sample_rate,
                 (unsigned)(data_len / frame_bytes),
                 (unsigned)(elapsed ? ((uint64_t)(data_len / frame_bytes) * 1000ull / elapsed) : 0));
    }
    s_wav_ms = s_wav_total_ms;
    s_wav_task = NULL;
    vTaskDeleteWithCaps(NULL);   /* WithCaps 创建 ⇒ 必须 WithCaps 删除, 否则栈+TCB 泄漏 */
}

/* ★★★ 2026-11-08 统一「输出链路就绪」保证 ★★★
 * 任何播放入口都先过这里: 调用方不必自己初始化硬件、恢复 codec 播放配置、
 * 开功放或重新 enable I2S 通道。全部幂等, 重复调用零开销。 */
static void audio_ensure_output_ready(void)
{
    if (!s_hw_initialized) audio_player_init();
    es8311_start();          /* 内部 s_started 短路; shutdown 后会自动重新配置 */
    if (!s_amp_on || !s_pa_on) ensure_out_active();   /* 开功放 + enable I2S TX */
}

/* 按扩展名/文件头判断 WAV —— 应用不必知道格式 */
static bool path_is_wav(const char *path)
{
    const char *d = strrchr(path, '.');
    if (d) {
        static const char *ext = ".wav";
        for (int i = 0; i < 4; i++) {
            char c = d[i];
            if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
            if (c == '\0' || c != ext[i]) return false;
        }
        return d[4] == '\0';
    }
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t h[12];
    size_t n = fread(h, 1, sizeof(h), f);
    fclose(f);
    return n == 12 && memcmp(h, "RIFF", 4) == 0 && memcmp(h + 8, "WAVE", 4) == 0;
}

int audio_player_play_file(const char *path)
{
    if (!path || !path[0]) return -1;
    if (audio_player_is_muted()) return -4;
    audio_ensure_output_ready();          /* 硬件/codec/功放/I2S/音量 全部在这里收口 */
    return path_is_wav(path) ? audio_player_play_wav(path)
                             : audio_player_play(path);
}

void audio_player_stop_all(void)
{
    if (s_wav_task) {
        s_wav_stop = true;        /* 只置标志, wav_play_task 自己收尾 */
        /* ★★★ 2026-11-23 修「电子木鱼快速连点, 音效不打断、不重新放」：
         *   wav_play_task 会把整段音频(52KB ≈ 296ms)一次性推进 PCM 环形缓冲
         *   (65536 帧 ≈ 4s)。只置 s_wav_stop 只是让**生产者**停下, 缓冲里已经
         *   推进去的样本仍会被 audio_out_task 照常播完 ⇒ 上层"停止"后声音根本没停,
         *   紧接着的新一声被叠在旧尾音上, 听感就是"没打断、也没重新放"。
         *   ⇒ 停止必须同时清空环形缓冲, 才是真的断音。 */
        audio_player_flush_pcm();
    }
    if (s_task_handle) audio_player_stop();   /* MP3 走正常停止 */
}

bool audio_player_any_playing(void)
{
    return (s_wav_task != NULL) || (s_task_handle != NULL);
}

/* 统一的 WAV 启动: path==NULL 表示"播放预载缓存", 否则按路径播放。
 * 两者只差数据来源, 硬件/采样率/功放/任务创建完全共用一条路径。 */
static int wav_kick(const char *path)
{
    if (s_wav_task) return -1;
    if (audio_player_is_muted()) return -4;
    if (path) {
        if (!path[0]) return -1;
    } else if (!s_wav_cache || s_wav_cache_len < 44) {
        return -5;                       /* 没预载就别假装能播 */
    }
    audio_ensure_output_ready();
    s_wav_use_cache = (path == NULL);
    if (path) {
        strncpy(s_wav_path, path, sizeof(s_wav_path) - 1);
        s_wav_path[sizeof(s_wav_path) - 1] = 0;
    } else {
        s_wav_path[0] = 0;
    }
    s_wav_stop = false;
    s_wav_ms = 0;
    s_wav_total_ms = 0;
    /* ★★★ 2026-11-23 修「点电子木鱼 → 震动不停 → 重启」(rst=4 PANIC, task=wavply,
     *   exc=4 IllegalInstruction, addr=panic_abort; backtrace 自底向上:
     *   esp_flash_read → spiflash_start_default → spi1_start → cache_disable
     *   → spi_flash_disable_interrupts_caches_and_other_cpu)。
     *
     *   根因: 本任务的核心工作就是 fopen/fread 音频文件。当文件位于 **SPI flash**
     *   (电子木鱼的音效是 `/store/muyu_sfx.wav` = SPIFFS/apps 分区) 时, 读 flash
     *   必须**关闭 cache**; 而 PSRAM 与 flash 共用同一套 cache 映射 ⇒ cache 关闭
     *   期间 PSRAM 不可访问。栈若在 PSRAM, 取/存栈帧当场 IllegalInstruction。
     *
     *   ⇒ 本任务栈**必须放内部 RAM**: 仅 4096B, 而内部最大连续块 ~139KB, 毫无压力。
     *   ⛔ 不要为"省内部 RAM"改回 MALLOC_CAP_SPIRAM —— 读 /store 文件是必崩路径。
     *   PSRAM 只作内部 RAM 极度紧张时的降级, 并打告警留证。 */
    if (xTaskCreateWithCaps(wav_play_task, "wavply", 4096, NULL, 7,
                            &s_wav_task, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGW(TAG, "内部 RAM 建 wavply 失败 ⇒ 退回 PSRAM (读 /store 文件有 panic 风险)");
        if (xTaskCreateWithCaps(wav_play_task, "wavply", 4096, NULL, 7,
                                &s_wav_task, MALLOC_CAP_SPIRAM) != pdPASS) {
            s_wav_task = NULL;
            return -2;
        }
    }
    ESP_LOGI(TAG, "WAV 播放: %s", path ? s_wav_path : "PSRAM 预载缓存");
    return 0;
}

int audio_player_play_wav(const char *path)
{
    if (!path || !path[0]) return -1;
    return wav_kick(path);
}

int audio_player_play_wav_cache(void)
{
    return wav_kick(NULL);
}

/* 把一段 WAV 拷进 PSRAM 常驻缓存。同长度视为"已就绪"直接返回 (内容变更由调用方
 * 用版本号作废旧数据, 例如电子木鱼的 SFX_VER) —— 省掉每次进应用都 memcmp 52KB。 */
int audio_player_wav_preload(const void *data, size_t len)
{
    if (!data || len < 44) return -1;
    if (s_wav_cache && s_wav_cache_len == len) return 0;
    audio_player_wav_unload();
    uint8_t *p = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = malloc(len);                    /* PSRAM 耗尽才退内部 RAM */
    if (!p) return -2;
    memcpy(p, data, len);
    s_wav_cache = p;
    s_wav_cache_len = len;
    ESP_LOGI(TAG, "WAV 预载 %u B (之后播放零 IO, 不关 cache)", (unsigned)len);
    return 0;
}

void audio_player_wav_unload(void)
{
    if (s_wav_task) {
        s_wav_stop = true;
        for (int i = 0; i < 100 && s_wav_task; i++) vTaskDelay(pdMS_TO_TICKS(5));
        if (s_wav_task) return;   /* 任务没退干净: 宁可留着缓存, 也不能让它读野指针 */
    }
    if (s_wav_cache) { free(s_wav_cache); s_wav_cache = NULL; }
    s_wav_cache_len = 0;
    s_wav_use_cache = false;
}

bool audio_player_wav_cache_ready(void)
{
    return s_wav_cache != NULL && s_wav_cache_len >= 44;
}

void audio_player_wav_stop(void) { s_wav_stop = true; }
bool audio_player_wav_active(void) { return s_wav_task != NULL; }
/* ★ 2026-11-08 统一进度视图: 保留 wav_ 前缀只为 ABI 兼容, 返回的是**当前任意播放**
 *   (WAV 或 MP3) 的进度。Lua 侧 app.rec.play_ms()/play_total_ms() 因此不必区分格式,
 *   MP3 播放时也能画进度条。 */
uint32_t audio_player_wav_ms(void)
{
    if (s_wav_task) return s_wav_ms;
    if (s_task_handle && s_mp3_sample_rate > 0)
        return (uint32_t)((uint64_t)s_elapsed_samples * 1000ull / (unsigned)s_mp3_sample_rate);
    return s_wav_ms;
}
uint32_t audio_player_wav_total_ms(void)
{
    if (s_wav_task) return s_wav_total_ms;
    if (s_task_handle && s_mp3_sample_rate > 0 && s_total_samples > 0)
        return (uint32_t)((uint64_t)s_total_samples * 1000ull / (unsigned)s_mp3_sample_rate);
    return s_wav_total_ms;
}
