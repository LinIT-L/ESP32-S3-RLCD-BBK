/* sdl_audio.c — SDL 1.2 音频垫片 -> LinTOS audio_player
 *
 * SDL 的模型是"我调你的回调，你填满缓冲"。我们没有 SDL 音频线程，
 * 所以这里开一个独立任务，按 samples/freq 的节奏调回调，再把 PCM 丢给
 * audio_player_feed_pcm（它自己管 I2S 环形缓冲和非阻塞写出）。
 *
 * 只支持 S16 立体声/单声道 —— SDLPal 的音效解码出来就是这两种。
 */
#include "SDL.h"

#include "palcfg.h"        /* gConfig: 对齐 OPL 采样率 */
#include "audio.h"          /* gAudioDevice: 判断音乐是否在放 */
#include "audio_player.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"   /* ★ vTaskDeleteWithCaps: 与 xTaskCreate...WithCaps 配对 */

#include <stdlib.h>
#include <string.h>

#ifndef PAL_MUSIC_RATE
/* 输出率: 只影响重采样/I2S, **不**决定 OPL 运算量 (OPL 内部恒为 clk/72)。
 * ⚠ 必须用 44100: 实测 22050 时 ES8311 分频不整, 实际跑成 24.8kHz,
 *   ring 持续 underrun (underruns 1518+), 听感是断续/无声。 */
#define PAL_MUSIC_RATE 44100
#endif

static const char *TAG = "SDLAUD";
static volatile int s_probe_req = 0;   /* palaud 触发: 在音频任务里用"回调同长度"直接问播放器 */

/* ★ 音乐总开关 (默认关): OPL 软合成在这颗 ESP32-S3 上**跑不到实时** —— 实测两组数据:
 *     · 44.1kHz 双芯片: CPU0 被 sdl_aud 吃满, underruns 上万;
 *     · 22.05kHz 单芯片 + 钉到 CPU1: 变成 IDLE1 饿死, underruns 2316, PCM 仍全 0。
 *   结论: DBOPL 软合成单核实时因子 < 1, 不是调参能救的。所以默认不开,
 *   要音乐应走"预渲染音频"(RIX→MP3, 由 audio_player 的 minimp3 解码, CPU 极低)。
 *   需要现场实验时 `palmus 1`(下次进游戏生效)。
 *   RIX_FillBuffer→CConvertopl→CSurroundopl→CEmuopl→DBOPL2::adlib_getsample,
 *   即使降到 22050Hz + 单芯片, 仍会把 CPU0 占满 ⇒ IDLE0 饿死 ⇒ 任务看门狗触发。
 *   所以默认不建音乐播放器 (音效 VOC 不受影响), 需要实验时 `palmus 1` 下次进游戏生效。 */
static bool s_music_on = true;
void pal_music_enable(bool on) { s_music_on = on; }
bool pal_music_enabled(void)   { return s_music_on; }

static SDL_AudioSpec s_spec;
static TaskHandle_t  s_task = NULL;
static volatile int  s_paused = 1;

/* ★★★ 2026-10-25 [LINTOS] 预渲染 MP3 后端专用：
 *   MP3 模式下音乐由 audio_player 的解码任务独占 I2S，而本文件的 audio_task 仍在
 *   每 46ms 调一次 audio_player_feed_pcm —— 那会触发 audio_out 任务启动并抢走 I2S，
 *   导致 MP3 解码任务 30ms 就被顶掉（实测日志：
 *     [6/6] 进入解码循环 → 30ms → 播放任务结束 → PCM 环形缓冲已预填充…开始播放）。
 *   置位后 audio_task 只跑回调（音效合成）但**不再往环形缓冲喂**，I2S 留给 MP3。 */
bool pal_audio_external_music = false;
/* ★★★ 2026-10-25 修复「准备音频时游戏自己退出」:
 *   之前把 s_task 当循环条件, 又允许外部置 NULL 来"请退" ⇒ 旧任务晚一步退出时
 *   会把**新任务**的句柄一起清掉, 新任务下一轮就自行结束, audio 初始化等于失败
 *   ⇒ SDLPal 认为音频起不来, 直接退出。
 *   现在循环只看 s_run; s_task 只表示"句柄", 由任务自己在退出时清。 */
static volatile bool s_run = false;

/* ★★★ 2026-10-25 修复 PSRAM 残留泄漏（退出后仍少 122KB 的一部分）:
 *   音频块缓冲原来是任务里局部 malloc —— 任务被 pal_shutdown 强杀时
 *   那行 free(buf) 永远走不到 ⇒ 每次进出漏 4KB(1024帧×2ch×2B)。
 *   现在提到文件级, 由 pal_audio_free_buf() 在"任务确认死亡后"统一释放。 */
static uint8_t *s_abuf = NULL;
static size_t   s_abuf_sz = 0;      /* ★ 已分配字节数 —— 规格变大时必须重分, 否则回调写越界 */
#define ABUF_TAIL    1024
#define ABUF_CANARY  0xA5

void pal_audio_free_buf(void)
{
    if (s_abuf) { free(s_abuf); s_abuf = NULL; s_abuf_sz = 0; }
}

static void audio_task(void *arg)
{
    (void)arg;
    const int ch = s_spec.channels ? s_spec.channels : 2;
    const int frames = s_spec.samples ? s_spec.samples : 1024;
    const int bytes  = frames * ch * 2;             /* S16 */
    /* ★★★ 2026-10-27 修堆越界: 原来只在"没分配过"时 malloc 一次。
     *   如果第二次 SDL_OpenAudio 的 samples/channels 比第一次大(例如 512→1024 帧,
     *   或 mono→stereo), 回调按新尺寸写进**旧的小缓冲** ⇒ 写穿到相邻堆块,
     *   之后任意 malloc 在 tlsf 里崩(实测 PAL_GetPalette → f_stat → malloc)。
     *   现在按尺寸增长重分配。 */
    if (bytes > 0 && s_abuf_sz < (size_t)bytes) {
        if (s_abuf) { free(s_abuf); s_abuf = NULL; }
        /* ★ 2026-10-27 诊断: 尾部留 1KB 哨兵区, 回调后检查是否被写 ——
         *   能直接抓到"SDLPal 混音写超设备缓冲"这类越界(它会把相邻堆块写坏)。 */
        s_abuf = malloc((size_t)bytes + ABUF_TAIL);
        s_abuf_sz = s_abuf ? (size_t)bytes : 0;
        if (s_abuf) memset(s_abuf + bytes, ABUF_CANARY, ABUF_TAIL);
    }
    uint8_t *buf = s_abuf;
    if (!buf) { ESP_LOGE(TAG, "no mem for audio buf"); s_run = false; s_task = NULL; vTaskDeleteWithCaps(NULL); return; }

    const TickType_t period = pdMS_TO_TICKS((frames * 1000) / (s_spec.freq > 0 ? s_spec.freq : 22050));
    TickType_t last = xTaskGetTickCount();

    while (s_run) {
        if (s_paused) {
            vTaskDelay(pdMS_TO_TICKS(10));
            last = xTaskGetTickCount();
            continue;
        }
        memset(buf, s_spec.silence, (size_t)bytes);
        const int64_t cb_t0 = esp_timer_get_time();
        if (s_spec.callback) s_spec.callback(s_spec.userdata, buf, bytes);

        /* ★ 越界哨兵检查: 回调若写超设备缓冲, 这里立刻报出越界了多少字节 */
        if (s_abuf && s_abuf_sz) {
            for (int k = 0; k < ABUF_TAIL; k++) {
                if (s_abuf[s_abuf_sz + k] != ABUF_CANARY) {
                    ESP_LOGE(TAG, "[AUDIO-OVERFLOW] 回调写超设备缓冲: 越界 %d 字节 (buf=%d)",
                             ABUF_TAIL - k, (int)s_abuf_sz);
                    memset(s_abuf + s_abuf_sz, ABUF_CANARY, ABUF_TAIL);   /* 复原, 避免刷屏 */
                    break;
                }
            }
        }
        /* ★ 2026-10-25 诊断: 只在第一块打一次，确认"调用时"callback 是否还有效 */
        {
            static int s_cb_logged = 0;
            if (!s_cb_logged) {
                s_cb_logged = 1;
                ESP_LOGW(TAG, "首次调回调: cb=%p ud=%p bytes=%d",
                         (void*)s_spec.callback, (void*)s_spec.userdata, (int)bytes);
            }
        }
        /* ★ 模块级自保: 单次回调超过 120ms 就算"这个模块跑不动了" ——
         *   直接关掉音乐(音效不动), 保证游戏/系统继续跑, 绝不让它把 TWDT 拖炸。
         *   这是 linit 要的"跑死就关掉对应模块, 不要卡死整机"。 */
        const int64_t cb_us = esp_timer_get_time() - cb_t0;
        {
            /* ★★★ 2026-10-25 linit「点新的故事后完全没声音」:
             *   原来这里超时就 fMusicEnabled=FALSE **永久关音乐** —— 一次场景加载抖动
             *   就把整局 BGM 关掉, 而且永不恢复。现在改成"暂停 2 秒后自动恢复",
             *   既避免持续拖慢 CPU, 又不会把音乐彻底弄丢。 */
            static int64_t mute_until_us = 0;
            static int     slow_hits = 0;
            if (cb_us > 120000) {
                if (++slow_hits >= 2) {
                    mute_until_us = esp_timer_get_time() + 2000000;   /* 静音 2s */
                    gAudioDevice.fMusicEnabled = FALSE;
                    ESP_LOGW(TAG, "音乐合成超时(%dms) ⇒ 暂停音乐 2s 后自动恢复", (int)(cb_us / 1000));
                    slow_hits = 0;
                }
            } else if (slow_hits > 0) {
                slow_hits--;
            }
            if (mute_until_us && esp_timer_get_time() >= mute_until_us) {
                mute_until_us = 0;
                if (pal_music_enabled()) {
                    gAudioDevice.fMusicEnabled = TRUE;
                    ESP_LOGI(TAG, "音乐已自动恢复");
                }
            }
        }
        /* ★ 2026-10-25: MP3 后端时音乐由解码任务独占 I2S，但**音效仍要出声** ——
         *   这块 buf 里是 AUDIO_FillBuffer 混好的音效(VOC)，推给音效缓冲，
         *   由 MP3 解码任务写 I2S 前叠加。这样"音乐 + 音效"两路都正常。 */
        if (pal_audio_external_music)
            audio_player_fx_push((const int16_t *)buf, (int)frames);
        else
            audio_player_feed_pcm((const int16_t *)buf, (size_t)frames, s_spec.freq);
        /* 诊断: 每秒报一次"喂出去有没有信号" —— 判断"没声音"是引擎没出声
         * 还是出声了但后端/功放没放, 不用靠耳朵和截图。 */
        {
            static int64_t last_us = 0; static int peak = 0; static int frames_acc = 0;
            /* 每次回调都记录"最深用过多少栈", 250ms 报一次; 崩溃前的最后一组数就是真实需求 */
            static unsigned worst = 0;
            const int16_t *pcm = (const int16_t *)buf;
            for (int i = 0; i < frames * ch; i++) {
                int v = pcm[i] < 0 ? -pcm[i] : pcm[i];
                if (v > peak) peak = v;
            }
            frames_acc += frames;
            {
                unsigned free_b = (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t));
                if (worst == 0 || free_b < worst) worst = free_b;
            }
            static int64_t win_t0 = 0; static int64_t busy_us = 0; static int cb_ms_max = 0;
            if (win_t0 == 0) win_t0 = esp_timer_get_time();
            busy_us += (cb_us > 0 ? cb_us : 0);
            if ((int)(cb_us / 1000) > cb_ms_max) cb_ms_max = (int)(cb_us / 1000);
            int64_t now = esp_timer_get_time();
            /* ★ 静音自愈: 输出连续 2s 全零而游戏还在跑 ⇒ 说明门禁被关掉了(音乐/音效开关),
             *   直接恢复并留日志。比"整局没声音"好得多。 */
            {
                static int zero_sec = 0;
                if (peak == 0) zero_sec++; else zero_sec = 0;
                if (zero_sec >= 8) {                    /* 8 × 250ms = 2s */
                    zero_sec = 0;
                    if (pal_music_enabled() && !gAudioDevice.fMusicEnabled) {
                        gAudioDevice.fMusicEnabled = TRUE;
                        ESP_LOGW(TAG, "静音 2s ⇒ 恢复音乐开关");
                    }
                    if (!gAudioDevice.fSoundEnabled) {
                        gAudioDevice.fSoundEnabled = TRUE;
                        ESP_LOGW(TAG, "静音 2s ⇒ 恢复音效开关");
                    }
                }
            }
            if (now - last_us >= 250000) {
                extern bool pal_audio_music_active(void);
                const int period_ms = (int)((int64_t)frames * 1000 / (s_spec.freq ? s_spec.freq : 1));
                const int64_t wall = esp_timer_get_time() - win_t0;
                const int duty = (int)(busy_us * 100 / (wall > 0 ? wall : 1));
                /* ★ 2026-10-25 加 men/mvol/sen/svol: 定位"菜单有声音、进游戏没声音"。
                 *   AUDIO_FillBuffer 只有在 (fMusicEnabled && iMusicVolume>0) 时才做合成,
                 *   否则**只 memset 就返回** ⇒ 表现正好是 duty=0%/cb_max=0/peak=0。 */
                ESP_LOGI(TAG, "PCM: peak=%d mus=%d pmus=%d psnd=%d duty=%d%% cb_max=%dms/每块%dms 栈余=%u men=%d mvol=%d sen=%d svol=%d pz=%d (%dHz %dch)",
                         peak, pal_audio_music_active() ? 1 : 0,
                         gAudioDevice.pMusPlayer ? 1 : 0,
                         gAudioDevice.pSoundPlayer ? 1 : 0,
                         duty, cb_ms_max, period_ms, worst,
                         gAudioDevice.fMusicEnabled ? 1 : 0, gAudioDevice.iMusicVolume,
                         gAudioDevice.fSoundEnabled ? 1 : 0, gAudioDevice.iSoundVolume,
                         (int)s_paused,
                         s_spec.freq, ch);
                win_t0 = esp_timer_get_time(); busy_us = 0; cb_ms_max = 0;
                last_us = now; peak = 0; frames_acc = 0;
            }
        }
        if (s_probe_req) {
            s_probe_req = 0;
            extern void pal_audio_probe_here(void);
            pal_audio_probe_here();          /* 同一线程/同一长度, 排除并发与长度差异 */
        }
        vTaskDelayUntil(&last, period ? period : 1);
    }
    pal_audio_free_buf();   /* ★ 退出即还: 这块原来被强杀时永远漏 4KB */
    s_run  = false;
    s_task = NULL;          /* 只有任务自己能清句柄, 外部不再动它 */
    vTaskDeleteWithCaps(NULL);
}

/* 诊断用: 音乐播放器是否已建立(RIX 初始化成功) */
bool pal_audio_music_active(void) { return gAudioDevice.pMusPlayer != NULL; }

/* ★ 诊断: 直接把 gAudioDevice 的状态和"RIX 到底出不出样本"打出来 ——
 *   串口 `palaud` 调用。判断依据全是状态, 不看截图。
 *   probe: 单独向音乐播放器要 1024 帧, 看 peak 是否非 0
 *          (非 0 = RIX 正常出声, 问题在音量/后级; 0 = RIX 本身没输出)。 */
void pal_audio_diag(void)
{
    /* ★★★ 2026-10-25 修「palaud 把整机卡死」:
     *   原来这里直接对 pMusPlayer 调 FillBuffer 取峰值, 而 sdl_aud 任务**同时**也在
     *   调同一个 OPL 合成器(emu8950 内部无锁) ⇒ 实测发一条 palaud 后 16 秒内连
     *   主循环心跳都没有, 只能复位。诊断命令必须只读状态, 绝不能碰合成器。
     *   真实峰值请看周期日志 `SDLAUD: PCM: peak=...`(那是合成线程自己测的)。 */
    int peak = -1;   /* -1 = 本命令不探测 */
    ESP_LOGI(TAG, "PALAD state=%d opened=%d mus_en=%d mus_vol=%d pMus=%p snd_en=%d snd_vol=%d pSnd=%p",
             (int)gAudioDevice.spec.freq, gAudioDevice.fOpened ? 1 : 0,
             gAudioDevice.fMusicEnabled ? 1 : 0, gAudioDevice.iMusicVolume,
             (void *)gAudioDevice.pMusPlayer,
             gAudioDevice.fSoundEnabled ? 1 : 0, gAudioDevice.iSoundVolume,
             (void *)gAudioDevice.pSoundPlayer);
    ESP_LOGI(TAG, "PALAD cfg musicvol=%d sndvol=%d rate=%d oplrate=%d oplcore=%d oplchip=%d surround=%d bufsz=%d",
             gConfig.iMusicVolume, gConfig.iSoundVolume, gConfig.iSampleRate,
             gConfig.iOPLSampleRate, (int)gConfig.eOPLCore, (int)gConfig.eOPLChip,
             gConfig.fUseSurroundOPL ? 1 : 0, (int)gConfig.dwAudioBufferSize);
    {
        extern void rix_diag(int *, int *, int *, int *, int *);   /* C++ 侧 extern "C" */
        int rdy = 0, mus = 0, nxt = 0, fade = 0, bsz = 0;
        rix_diag(&rdy, &mus, &nxt, &fade, &bsz);
        ESP_LOGI(TAG, "PALAD rix ready=%d music=%d next=%d fade=%d bufsize=%d",
                 rdy, mus, nxt, fade, bsz);
    }
    {
        s_probe_req = 1;   /* 让音频任务下个周期做同长度探针 */
        /* ★ 2026-10-25: 用**弱符号**引用 —— emu8950 只在 PAL_MUSIC_MODE=opl 时才编进固件,
         *   MP3(预渲染)后端下没有这个符号, 直接 extern 会在链接期 undefined reference。
         *   弱符号在 mp3 模式下解析为 NULL, 这里判空跳过即可。 */
        extern int emu8950_selftest(void) __attribute__((weak));
        int st = emu8950_selftest ? emu8950_selftest() : -1;
        ESP_LOGI(TAG, "PALAD selftest peak=%d (%d=该后端无OPL核, 0=合成核出不了声, >0=核正常)", st, -1);
    }
    ESP_LOGI(TAG, "PALAD probe peak=%d (0=音乐播放器没出样本)", peak);
}

/* 在音频任务上下文里, 用两种长度直接向音乐播放器取样本。
 * 目的: 区分"播放器对 4096 字节请求出零" 还是 "AUDIO_FillBuffer 的门禁/混音把它清零"。 */
void pal_audio_probe_here(void)
{
    static short pb[2048];                 /* 4096 字节 */
    const int lens[2] = { 2048, 4096 };
    for (int k = 0; k < 2; k++) {
        const int bytes = lens[k];
        memset(pb, 0, sizeof(pb));
        int peak = 0, nz = 0, first = -1;
        if (gAudioDevice.pMusPlayer) {
            gAudioDevice.pMusPlayer->FillBuffer(gAudioDevice.pMusPlayer, (LPBYTE)pb, bytes);
            for (int i = 0; i < bytes / 2; i++) {
                int v = pb[i] < 0 ? -pb[i] : pb[i];
                if (v) { nz++; if (first < 0) first = i; }
                if (v > peak) peak = v;
            }
        }
        ESP_LOGW(TAG, "PROBE len=%d peak=%d nz=%d first=%d s=[%d %d %d %d]",
                 bytes, peak, nz, first, pb[0], pb[1], pb[2], pb[3]);

        /* B: 同一个缓冲区走**完整回调** —— 与音频任务里的调用完全一致 */
        memset(pb, 0, sizeof(pb));
        int peak2 = 0, nz2 = 0;
        if (s_spec.callback) s_spec.callback(s_spec.userdata, (uint8_t *)pb, bytes);
        for (int i = 0; i < bytes / 2; i++) {
            int v = pb[i] < 0 ? -pb[i] : pb[i];
            if (v) nz2++;
            if (v > peak2) peak2 = v;
        }
        ESP_LOGW(TAG, "PROBE-CB len=%d peak=%d nz=%d | mus_en=%d mus_vol=%d snd_en=%d snd_vol=%d cb=%p ud=%p",
                 bytes, peak2, nz2,
                 gAudioDevice.fMusicEnabled ? 1 : 0, gAudioDevice.iMusicVolume,
                 gAudioDevice.fSoundEnabled ? 1 : 0, gAudioDevice.iSoundVolume,
                 (void *)s_spec.callback, s_spec.userdata);
    }
}

/* ★ 退出游戏时请音频任务自己退: 只置标志等它收尾, **不强杀** ——
 *   强杀时它可能正持有 audio_player 的环形缓冲/互斥, 会把后端一起带坏。
 *   下次进游戏 SDL_OpenAudio 会重新创建。 */
void pal_audio_stop(void)
{
    s_paused = 1;
    s_run = false;                     /* 只置标志; 句柄由任务退出时自己清 */
    for (int i = 0; i < 12; i++) {     /* 最多等 120ms: 正常 1~2 个块(23ms)就退完 */
        if (!xTaskGetHandle("sdl_aud")) return;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_LOGW(TAG, "音频任务 120ms 内未退出, 下次 OpenAudio 会先等它结束");
}

int SDL_OpenAudio(SDL_AudioSpec *desired, SDL_AudioSpec *obtained)
{
    /* ★ 2026-10-25 诊断「MP3 版一进游戏就 InstrFetchProhibited 重启」:
     *   反解崩溃地址得 audio_task@sdl_audio.c:88 —— 即 `s_spec.callback(...)` 时
     *   指针 = 0xfffffffd。这里把"进来时的 callback"和"真正调用时的 callback"都打出来，
     *   就能分清是"传进来就是坏的"还是"open 之后被写坏的"。 */
    ESP_LOGW(TAG, "SDL_OpenAudio IN: desired=%p cb=%p ud=%p freq=%d ch=%d samples=%d",
             (void*)desired,
             (void*)(desired ? desired->callback : NULL),
             (void*)(desired ? desired->userdata : NULL),
             desired ? (int)desired->freq : -1,
             desired ? (int)desired->channels : -1,
             desired ? (int)desired->samples : -1);
    if (!desired || !desired->callback) return -1;
#ifdef PAL_DISABLE_AUDIO
    /* 诊断开关: 完全不开音频, 用来二分"堆被写坏"是否来自音效混音 */
    ESP_LOGW(TAG, "PAL_DISABLE_AUDIO: SDL_OpenAudio 直接失败(诊断模式)");
    return -1;
#endif

    /* ★ 2026-10-27: 先停旧任务再换规格 —— 否则旧任务正拿 s_abuf 回调,
     *   这里换 s_spec 会让它按新尺寸写旧缓冲。 */
    if (s_run) pal_audio_stop();

    s_spec = *desired;
    /* 只接受 S16（audio_player 走 int16 环形缓冲） */
    if (s_spec.format != AUDIO_S16LSB && s_spec.format != AUDIO_S16SYS) {
        ESP_LOGW(TAG, "format 0x%04x 不支持, 强制 S16LSB", s_spec.format);
        s_spec.format = AUDIO_S16LSB;
    }
    if (s_spec.channels < 1 || s_spec.channels > 2) s_spec.channels = 2;
    /* ★ 2026-10-25 [LINTOS] 试过降到 22050(照 DOOM 移植的思路压 OPL 负载),
     *   ❌ 实测**明显破音** —— 原因: emu8950 是 FM 合成, 内部振荡器/包络产生的高频
     *   远超 11kHz, 22050 采样时全部折返成混叠(aliasing), 听感就是"破/沙"。
     *   FM 音源至少需要 32kHz 才不混叠, 所以这里保持 SDLPal 请求的 44100。
     *   ⇒ 结论: OPL 实时合成的 CPU 负载在这个硬件上没法靠降采样省。 */
    if (s_spec.freq < 8000 || s_spec.freq > 48000) {
        ESP_LOGW(TAG, "采样率 %d 越界, 回落到 44100", (int)s_spec.freq);
        s_spec.freq = 44100;
    }
    /* ★ 2026-10-25 回退: 块长统一回 2048 帧(46ms)。
     *   曾经为压音效延迟试过 256/1024，结果音效**完全没声音**（原因未明，
     *   怀疑 SDLPal 的 VOC 混音在小块长下不工作），先恢复到"能出声"的基线，
     *   延迟问题用别的办法解决（不要再用块长去调）。 */
    if (s_spec.samples < 128 || s_spec.samples > 4096) s_spec.samples = 2048;
    if (s_spec.samples < 2048) s_spec.samples = 2048;
    s_spec.size = (Uint32)(s_spec.samples * s_spec.channels * 2);

    /* ★★★ 2026-10-25 [LINTOS] 「MP3 版一进游戏就重启」的根因之一:
     *   AUDIO_FillBuffer 里对音效临时缓冲做的是
     *       memset(gAudioDevice.pSoundBuffer, 0, len);        // len = samples*ch*soshort
     *       pSoundPlayer->FillBuffer(..., pSoundBuffer, len);
     *   而 pSoundBuffer 是按 gConfig.dwAudioBufferSize 分配的(audio.c:289)。
     *   我把块长提到 2048 帧后 len=8192, 但 dwAudioBufferSize 还是默认的 1024 帧
     *   ⇒ 每次回调**写越界 4KB**, 直接踩坏堆元数据, 之后各种野指针崩溃
     *   (表现为 InstrFetchProhibited 跳到 0xfffffffd)。
     *   这里强制让音效缓冲跟块长一致, 越界从根上消失。 */
    gConfig.dwAudioBufferSize = (Uint32)s_spec.samples;

    /* ★ 把**实际**采用的参数回写给调用方 (desired 就是 SDLPal 的 gAudioDevice.spec)。
     *   否则 SDLPal 自己记录的是"请求值 44100", 而底层跑 22050:
     *   音效(VOC)会按 44100 做重采样/计算 ⇒ 变调、变速。
     *   SDL 标准里 desired 是 in 参数, 但这里 desired 与设备结构体同一份,
     *   回写才能保证"上层看到的 = 实际输出的"。 */
    *desired = s_spec;

    /* ★★★ 2026-10-25 linit「模拟引擎没声音 / 会死机重启」的真正原因:
     *   SDLPal 的 RIX 音乐默认让 OPL 按 49716Hz 合成, 再经 resampler 重采样到 44100。
     *   在 ESP32 上这条路径会**死循环**: RIX_FillBuffer 里
     *      while (sample_count) {
     *          ... resampler_get_free_count() 写入 ...
     *          to_get = resampler_get_sample_count();   // 一直返回 0
     *          sample_count -= to_get;                  // 永远减不下去
     *      }
     *   而且循环体里还有 alloca() ⇒ 每转一圈就往下啃栈 ⇒ 先报 stack overflow、
     *   再被 TWDT 抓到 "audio_task 卡在 resampler_run_sinc_c" ⇒ 看门狗重启。
     *   实测反解: audio_task → AUDIO_FillBuffer → RIX_FillBuffer → resampler_run_sinc_c。
     *
     *   修法: 让 OPL **直接按设备采样率**合成 (iOPLSampleRate = 实际 s_spec.freq),
     *   RIX_Init 里 `if (iOPLSampleRate != iSampleRate)` 不成立 ⇒ 根本不建 resampler,
     *   走 `opl->update()` 直通分支。既没有死循环/alloca, 也省掉一次重采样运算。
     *   必须在 RIX_Init 之前生效 —— SDL_OpenAudio 正是 AUDIO_OpenDevice 里
     *   先于 SOUND_Init/RIX_Init 的那一步, 所以放这里最稳。 */
    /* ★ 减负(实测必须): CSurroundopl 会同时驱动**两个** OPL2 芯片, 44100Hz 下
     *   DBOPL2::adlib_getsample 单次回调要几百毫秒 ⇒ I2S 饿死(underruns 上万),
     *   任务看门狗把整机拖死(反解栈: RIX_FillBuffer→CConvertopl→CSurroundopl→
     *   CEmuopl::update→DBOPL2::adlib_getsample)。ESP32-S3 单核跑不动"双芯片 44.1k"。
     *   处理:
     *     · 关掉 surround (单芯片, 工作量减半)
     *     · 采样率降到 22050 (工作量再减半; OPL 音乐听感差别很小, 但 CPU 只要 1/4)
     *   OPL 率仍然 = 设备率 ⇒ 依旧不建 resampler (见下面注释)。 */
    /* ★ 片上 OPL 合成: 只做两件事把工作量压到能实时 ——
     *   ① 关 surround（否则 CSurroundopl 会跑**两个** OPL2 芯片，直接翻倍）
     *   ② 采样率 11025（相比 44.1k 是 1/4，再乘上单芯片 = 原来的 1/8）
     *   实测起点: 44.1k 双芯片 CPU0 100%（underruns 上万）；22.05k 单芯片 CPU1 也 100%。
     *   现在按 1/8 试，能否实时看 `SDLAUD: duty` 日志（<60% 才算稳）。 */
    gConfig.fUseSurroundOPL = FALSE;         /* 双芯片翻倍, 单核吃不消 */
    if (s_spec.freq > PAL_MUSIC_RATE) s_spec.freq = PAL_MUSIC_RATE;
    gConfig.iSampleRate     = s_spec.freq;
    gConfig.iOPLSampleRate  = s_spec.freq;   /* 两者相等 ⇒ rixplay 不建 resampler (那条路会死循环) */
    /* ★ 合成核: emu8950 (MIT 轻量 OPL2)。emu8950 内部按 clk/72=49716Hz 生成,
     *   再重采样到 iOPLSampleRate。CPU 只与 clk 有关, 与输出率无关。 */
    gConfig.eOPLCore        = OPLCORE_EMU8950;
    gConfig.eOPLChip        = OPLCHIP_OPL2;
    if (!s_music_on) gConfig.eMusicType = (MUSICTYPE)99;


    if (obtained) *obtained = s_spec;
    { extern void pal_port_note_stage(int); pal_port_note_stage(3); }   /* 音频就绪 */

    s_paused = 1;
    /* ★★★ 2026-10-25 回归修复「第二次进仙剑就没声音」:
     *   pal_shutdown() 的第④步在音频任务 120ms 内没退干净时, 会按名字 vTaskDelete 强杀 sdl_aud。
     *   强杀的任务**没有机会执行 164~165 行的自清**, 于是本文件的 static s_task 仍是非 NULL;
     *   下一次进来走到下面 `if (s_task) return 0;` 就直接返回 ⇒ 本次根本没有音频任务
     *   (游戏能玩、画面正常、就是永远静音, 且没有任何报错日志)。
     *   判据: 按名字查不到 ⇒ 句柄早已失效 ⇒ 清掉重建。 */
    if (s_task && xTaskGetHandle("sdl_aud") == NULL) {
        ESP_LOGW(TAG, "残留 s_task(任务已被强杀) ⇒ 清句柄重建音频任务");
        s_task = NULL;
    }
    if (s_task) return 0;
    /* 上一轮的任务可能还在收尾(最多约一个块 23ms): 等它真的消失再建,
     * 否则两个同名任务并存, 旧的退出时会把新句柄清掉。 */
    for (int i = 0; i < 10 && xTaskGetHandle("sdl_aud"); i++) vTaskDelay(pdMS_TO_TICKS(10));

    /* 栈放 PSRAM。★ 2026-10-25: 4096 → 12288 —— 接了 RIX 音乐后, 回调里跑的是
     *   adplug 的 OPL2 软合成(rixplay → CConvertopl/CSurroundopl → 各 OPL 核),
     *   函数嵌套深、局部数组多, 4KB 必炸(实测 "stack overflow in task sdl_aud")。
     *   栈仍在 PSRAM, 不吃内部 RAM。 */
    /* ★★★ 2026-10-25 实测: 音乐一开, WDT 报的是 `CPU 0: sdl_aud`, 而 `CPU 1: IDLE1`
     *   —— 也就是说 OPL 合成和游戏(sdlpal)抢同一个核, 把 IDLE0 饿死。
     *   这里把音频任务**钉到 CPU1** (双核各干一件事), 是 tiny386-esp32s3 那类
     *   带 Adlib OPL2 的移植的通行做法。优先级也给足(5), 保证每 1/70s 的块按时出。 */
    s_run = true;
    if (xTaskCreatePinnedToCoreWithCaps(audio_task, "sdl_aud", 32768, NULL, 9, &s_task, 1,
                                       MALLOC_CAP_SPIRAM) != pdPASS) {
        s_run  = false;
        s_task = NULL;
        /* ★ 音频起不来也**不能让游戏退出**: 用户要的是"能玩"。静音继续,
         *   日志留证, 下次进游戏会再试。原来这里 return -1 会被 SDLPal
         *   当成致命错误, 表现就是"准备音频时候游戏自己退出了"。 */
        ESP_LOGE(TAG, "audio task 创建失败 ⇒ 本次静音继续(不退出游戏)");
        return 0;
    }
    ESP_LOGI(TAG, "opened %d Hz %dch %d frames", s_spec.freq, s_spec.channels, s_spec.samples);
    return 0;
}

void SDL_CloseAudio(void)
{
    if (s_task) {
        TaskHandle_t t = s_task;
        s_task = NULL;
        for (int i = 0; i < 30 && t; i++) vTaskDelay(pdMS_TO_TICKS(2));
    }
    s_paused = 1;
}

void SDL_PauseAudio(int pause_on) { s_paused = pause_on ? 1 : 0; }

/* ★★★ 2026-10-25 给 RIX_Play 用的"安全暂停"：
 *   换曲目时要执行 rix->rewind()（实测 190ms，因为要重发整套 OPL 寄存器）。
 *   原来这一步是**在音频回调里**做的 ⇒ 193ms 的块要等 190ms ⇒ 欠载 8 倍、
 *   窗口内 peak 只剩个位数 ⇒ 用户听到"没声音"。
 *   现在改到游戏线程做，做之前必须让回调退出（否则两个线程同时碰 OPL 合成器）。
 *   等 60ms（> 2 个块周期）足够让正在跑的回调走完一轮。 */
void pal_audio_guard_begin(void)
{
    s_paused = 1;
    vTaskDelay(pdMS_TO_TICKS(100));   /* ★ 块长 46ms ⇒ 等 2 个块确保回调已退出 */
}
void pal_audio_guard_end(void) { s_paused = 0; }
void SDL_LockAudio(void) {}
void SDL_UnlockAudio(void) {}
