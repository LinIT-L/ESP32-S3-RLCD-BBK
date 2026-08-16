/* video_audio.c — esp-box-emu nes 组件的 OSD (C 版, 去掉 BoxEmu/espp 依赖)
 *
 * 实现 nofrendo 所需的宿主回调:
 *   - 视频: 自定义 viddriver, custom_blit 把 256x224 调色板索引帧转成 RGB565
 *   - 音频: do_audio_frame 把 APU 输出喂给 audio_player (22050Hz)
 *   - 输入: osd_getinput 从 nes_emu_set_joypad 的掩码产生事件
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "noftypes.h"
#include "bitmap.h"
#include "nofconfig.h"
#include "event.h"
#include "log.h"
#include "nes.h"
#include "nes_pal.h"
#include "nesinput.h"
#include "osd.h"

#include "audio_player.h"
#include "make_color.h"

/* V1.0.55: 22050 在部分 I2S/ES8311 组合下实际播放偏快 (~26-28kHz),
 * 改回硬件验证过、与 MP3 路径一致的 44100Hz (每帧 44100/60=735 样本). */
#define NES_AUDIO_RATE  44100
#define NES_AUDIO_MAX_SAMPLES 2048

/* 视频输出缓冲: 256x224 灰度 (0=白..3=黑) 2bit 打包, 每字节 4 像素.
 * 14336 字节, 只有原来 57KB 字节帧的 1/4, 模拟任务与视频任务都更快. */
uint8_t *nes_video_shade_packed = NULL;
volatile bool nes_video_frame_ready = false;
volatile uint32_t nes_audio_peak = 0;   /* 诊断: 最近一帧音频峰值 */
static uint16_t *myPalette = NULL;
static uint8_t s_shade_lut[256];        /* 调色板索引 -> 4 级灰度 */
static bitmap_t *myBitmap = NULL;
static uint8_t s_hw_dummy[4];           /* lock_write 占位 (实际不走硬件位图) */

/* 诊断 (来自 nes_apu.c) */
extern void nes_apu_dump_state(char *buf, size_t len);

/* 输入掩码 (与 GB 布局一致: bit0=A bit1=B bit2=Select bit3=Start
 *  bit4=右 bit5=左 bit6=上 bit7=下, 低电平有效) */
static volatile uint8_t s_joypad = 0xFF;

/* ROM 数据 (adapter 启动前设置) */
uint8_t *nes_rom_data = NULL;
size_t   nes_rom_size = 0;

bool forceConsoleReset = false;

/* nofrendo.c 的 main_quit/main_eject 由事件系统引用, 本项目无对应主循环, 置空 */
void main_quit(void) {}
void main_eject(void) {}

/* === 音频 === */
static void (*audio_callback)(void *buffer, int length) = NULL;
static int num_samples = 0;
static int16_t *audio_frame = NULL;
static int64_t last_audio_us = 0;

int osd_installtimer(int frequency, void *func, int funcsize, void *counter, int countersize)
{
    (void)frequency; (void)func; (void)funcsize; (void)counter; (void)countersize;
    return 0;
}

void do_audio_frame(void)
{
    if (audio_callback == NULL || audio_frame == NULL) {
        static int cb_null_logs = 0;
        if (cb_null_logs < 3) {
            printf("do_audio_frame: callback=%p frame=%p (静音)\n",
                   (void *)audio_callback, (void *)audio_frame);
            cb_null_logs++;
        }
        return;
    }
    /* 按真实经过时间生成音频样本数: samples = 22050 * dt.
     * 这样无论模拟帧率是 59 还是 60, 生产速率都与扬声器消耗速率一致,
     * 环形缓冲不再被慢慢掏空 (之前固定 367/帧, 59fps 时欠载 -> 声音一顿一顿). */
    int64_t now = esp_timer_get_time();
    if (last_audio_us == 0) last_audio_us = now;
    int64_t dt_us = now - last_audio_us;
    last_audio_us = now;
    if (dt_us < 1000) dt_us = 1000;          /* 防抖: 至少 1ms */
    if (dt_us > 100000) dt_us = 100000;      /* 防止暂停恢复后突发 */
    int samples = (int)((NES_AUDIO_RATE * dt_us) / 1000000);
    if (samples < 256) samples = 256;
    if (samples > NES_AUDIO_MAX_SAMPLES) samples = NES_AUDIO_MAX_SAMPLES;

    audio_callback(audio_frame, samples);
    /* 诊断: 记录音频峰值 */
    {
        uint32_t peak = 0;
        for (int i = 0; i < samples; i++) {
            int16_t s = audio_frame[i];
            uint32_t a = (uint32_t)(s < 0 ? -s : s);
            if (a > peak) peak = a;
        }
        nes_audio_peak = peak;
        static int audio_diag_cnt = 0;
        if ((++audio_diag_cnt % 60) == 0) {
            char abuf[200];
            nes_apu_dump_state(abuf, sizeof(abuf));
            printf("NES audio diag: n=%d dt=%lldus peak=%u s0=%d s1=%d s2=%d s3=%d | %s\n",
                   samples, (long long)dt_us,
                   (unsigned)peak, audio_frame[0], audio_frame[1],
                   audio_frame[2], audio_frame[3], abuf);
        }
    }
    /* APU 输出为单声道 int16; audio_player 环形缓冲是立体声交错 (L/R).
     * 从尾部倒序展开为立体声 (L=R), 避免把相邻两个单声道样本当左右声道. */
    for (int i = samples - 1; i >= 0; i--) {
        audio_frame[i * 2] = audio_frame[i];
        audio_frame[i * 2 + 1] = audio_frame[i];
    }
    audio_player_feed_pcm(audio_frame, (size_t)samples, NES_AUDIO_RATE);
}

void osd_setsound(void (*playfunc)(void *buffer, int length))
{
    audio_callback = playfunc;
}

void osd_getsoundinfo(sndinfo_t *info)
{
    if (!info) return;
    info->sample_rate = NES_AUDIO_RATE;
    info->bps = 16;
}

/* === 视频驱动 === */
static int nes_drv_init(int width, int height);
static void nes_drv_shutdown(void);
static int nes_drv_set_mode(int width, int height);
static void nes_drv_set_palette(rgb_t *pal);
static void nes_drv_clear(uint8 color);
static bitmap_t *nes_drv_lock_write(void);
static void nes_drv_free_write(int num_dirties, rect_t *dirty_rects);
static void nes_drv_custom_blit(bitmap_t *bmp, int num_dirties, rect_t *dirty_rects);

static viddriver_t nes_driver = {
    "esp-box-emu nes driver",
    nes_drv_init,
    nes_drv_shutdown,
    nes_drv_set_mode,
    nes_drv_set_palette,
    nes_drv_clear,
    nes_drv_lock_write,
    nes_drv_free_write,
    nes_drv_custom_blit,
    false
};

void osd_getvideoinfo(vidinfo_t *info)
{
    if (!info) return;
    info->default_width = 256;
    info->default_height = NES_VISIBLE_HEIGHT;
    info->driver = &nes_driver;
}

static int nes_drv_init(int width, int height)
{
    (void)width; (void)height;
    if (!nes_video_shade_packed) {
        nes_video_shade_packed = heap_caps_malloc(NES_SCREEN_WIDTH * NES_VISIBLE_HEIGHT / 4,
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!nes_video_shade_packed) return -1;
    }
    memset(nes_video_shade_packed, 0, NES_SCREEN_WIDTH * NES_VISIBLE_HEIGHT / 4);
    return 0;
}

static void nes_drv_shutdown(void)
{
}

static int nes_drv_set_mode(int width, int height)
{
    (void)width; (void)height;
    return 0;
}

static void nes_drv_set_palette(rgb_t *pal)
{
    if (!pal) return;
    if (!myPalette) {
        myPalette = heap_caps_malloc(256 * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!myPalette) return;
    for (int i = 0; i < 256; i++) {
        myPalette[i] = make_color(pal[i].r, pal[i].g, pal[i].b);
        /* 与 nes_rgb565_to_shade 相同阈值, 但一次查表避免每像素除法 */
        uint32_t luma = (uint32_t)pal[i].r * 77u + (uint32_t)pal[i].g * 150u +
                        (uint32_t)pal[i].b * 29u;
        uint8_t level = (uint8_t)((luma * 255u) / 65280u);
        if (level < 64)      s_shade_lut[i] = 3;
        else if (level < 128) s_shade_lut[i] = 2;
        else if (level < 192) s_shade_lut[i] = 1;
        else                  s_shade_lut[i] = 0;
    }
}

uint16_t *get_nes_palette(void)
{
    return myPalette;
}

static void nes_drv_clear(uint8 color)
{
    (void)color;
}

static bitmap_t *nes_drv_lock_write(void)
{
    myBitmap = bmp_createhw(s_hw_dummy, NES_SCREEN_WIDTH, NES_VISIBLE_HEIGHT,
                            NES_SCREEN_WIDTH * 2);
    if (myBitmap) myBitmap->hardware = true;
    return myBitmap;
}

static void nes_drv_free_write(int num_dirties, rect_t *dirty_rects)
{
    (void)num_dirties; (void)dirty_rects;
    bmp_destroy(&myBitmap);
}

static void nes_drv_custom_blit(bitmap_t *bmp, int num_dirties, rect_t *dirty_rects)
{
    (void)num_dirties; (void)dirty_rects;
    if (!bmp || !bmp->line[0] || !nes_video_shade_packed) return;
    const uint8_t *src = bmp->line[0];
    uint8_t *dst = nes_video_shade_packed;
    int n = NES_SCREEN_WIDTH * NES_VISIBLE_HEIGHT;
    for (int i = 0; i < n; i += 4) {
        dst[i >> 2] = (uint8_t)(s_shade_lut[src[i]] |
                                (s_shade_lut[src[i + 1]] << 2) |
                                (s_shade_lut[src[i + 2]] << 4) |
                                (s_shade_lut[src[i + 3]] << 6));
    }
    nes_video_frame_ready = true;
}

/* === 输入 === */
void osd_getinput(void)
{
    static const int ev[16] = {
        event_joypad1_select, 0, 0, event_joypad1_start,
        event_joypad1_up, event_joypad1_right, event_joypad1_down, event_joypad1_left,
        0, 0, 0, 0, event_soft_reset, event_joypad1_a, event_joypad1_b, event_hard_reset
    };
    static int oldb = 0xFFFF;
    /* GB 布局低电平有效 -> nofrendo 位 (bit0=select bit3=start bit4=up bit5=right
     * bit6=down bit7=left bit13=A bit14=B; 与 esp-box-emu ConvertJoystickInput 一致,
     * bit12 是 event_soft_reset, 不能占用) */
    int m = 0;
    if (!(s_joypad & (1u << 2))) m |= (1 << 0);    /* Select */
    if (!(s_joypad & (1u << 3))) m |= (1 << 3);    /* Start  */
    if (!(s_joypad & (1u << 6))) m |= (1 << 4);    /* Up     */
    if (!(s_joypad & (1u << 4))) m |= (1 << 5);    /* Right  */
    if (!(s_joypad & (1u << 7))) m |= (1 << 6);    /* Down   */
    if (!(s_joypad & (1u << 5))) m |= (1 << 7);    /* Left   */
    if (!(s_joypad & (1u << 0))) m |= (1 << 13);   /* A      */
    if (!(s_joypad & (1u << 1))) m |= (1 << 14);   /* B      */

    int chg = m ^ oldb;
    oldb = m;
    event_t evh;
    for (int x = 0; x < 16; x++) {
        if (chg & 1) {
            evh = event_get(ev[x]);
            /* m 为高电平有效 (置位=按下): 按下->MAKE, 松开->BREAK.
             * 注意不能照搬 esp-box-emu 的低电平掩码 + BREAK/MAKE 逻辑, 否则按键反相. */
            if (evh) evh((m & 1) ? INP_STATE_MAKE : INP_STATE_BREAK);
        }
        chg >>= 1;
        m >>= 1;
    }
    /* 诊断: 原始按键掩码变化时打印 */
    static uint8_t last_logged = 0xFF;
    if (last_logged != s_joypad) {
        printf("osd_getinput: joypad=0x%02X\n", (unsigned)s_joypad);
        last_logged = s_joypad;
    }
}

void osd_getmouse(int *x, int *y, int *button)
{
    (void)x; (void)y; (void)button;
}

void nes_set_joypad_mask(uint8_t mask)
{
    s_joypad = mask;
}

/* === 文件/杂项 === */
void osd_fullname(char *fullname, const char *shortname)
{
    if (fullname && shortname) strcpy(fullname, shortname);
}

char *osd_newextension(char *string, char *ext)
{
    (void)ext;
    return string;
}

int osd_makesnapname(char *filename, int len)
{
    (void)filename; (void)len;
    return -1;
}

char *osd_getromdata(void)
{
    return (char *)nes_rom_data;
}

void osd_set_video_scale(bool new_video_scale)
{
    (void)new_video_scale;
}

static int logprint(const char *string)
{
    return printf("%s", string);
}

void osd_shutdown(void)
{
    audio_callback = NULL;
}

int osd_init(void)
{
    log_chain_logfunc(logprint);
    if (!audio_frame) {
        num_samples = NES_AUDIO_MAX_SAMPLES;
        /* 2x: 单声道样本展开为立体声交错 */
        audio_frame = heap_caps_malloc((size_t)num_samples * 2 * sizeof(int16_t),
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!audio_frame) return -1;
    }
    audio_callback = NULL;
    last_audio_us = 0;
    return 0;
}

int osd_main(int argc, char *argv[])
{
    (void)argc; (void)argv;
    return 0;
}
