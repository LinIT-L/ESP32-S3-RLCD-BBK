/* pal_stub.c — SDLPal 里 ESP32 上不实现的模块（空实现）
 *
 * 为什么用空实现而不是编原文件：
 *  · aviplay.c —— 片头/结局动画，依赖 SDL_Overlay(YUV 拉伸)、SDL_AudioCVT、SDL_mutex，
 *    为几段动画在 ESP32 上补齐这一整套不值得。PAL_PlayAVI 返回 0 = 视为播完，剧情照走。
 *  · RIX/MP3/OGG/OPUS/TIMIDITY/TSF —— 各种音乐后端，本移植**先不做音乐**（pal_config.h 里
 *    PAL_HAS_MP3/OGG/OPUS/NATIVEMIDI 全 0）。这里返回 NULL 让 AUDIO_OpenDevice 走"无音乐"分支，
 *    音效（VOC）不受影响。
 */
#include "palcommon.h"
#include "players.h"
#include "aviplay.h"

/* ---- 动画 ---- */
int PAL_PlayAVI(const char *lpszPath)
{
    (void)lpszPath;
    return 0;                        /* 跳过，视为播完 */
}

void *AVI_GetPlayState(void) { return NULL; }

void PAL_AVIInit(void) {}
void PAL_AVIShutdown(void) {}

/* ★★★ 2026-10-25 关键修复（"仙剑一直没声音"的真正原因）:
 *   AUDIO_FillBuffer() 的流程是:
 *       memset(stream,0) → 音乐 pMusPlayer->FillBuffer(stream) → 调音量
 *                       → 音效混音 → **最后** AVI_FillAudioBuffer(stream)
 *   原来这个桩写了 memset(stream,0,len) —— 把刚刚填好的音乐和音效**整块抹掉**。
 *   结果: 合成一直在跑(占 36~77% CPU)、播放器 probe 有满幅样本、音量全正常,
 *         而送到 I2S 的永远是零 ⇒ 无论换哪个 OPL 核都"没声音"。
 *   正确语义: 没有 AVI 在播时**什么都不做**(真实 aviplay.c 也只在 avi->fp != NULL 时才写)。 */
void AVI_FillAudioBuffer(void *udata, uint8_t *stream, int len)
{
    (void)udata; (void)stream; (void)len;   /* 不碰 stream! */
}

/* ---- 音乐后端（全部不可用 → 只有音效）---- */
/* ★ 2026-10-25: RIX_Init 不再用桩 —— 官方 rixplay.cpp + adplug 已编进固件,
 *   仙剑的音乐(mus.mkf 里的 RIX)由它播放。见 CMakeLists 的 RIX 段。 */
LPAUDIOPLAYER OGG_Init(VOID)      { return NULL; }
LPAUDIOPLAYER OPUS_Init(VOID)     { return NULL; }
LPAUDIOPLAYER MP3_Init(VOID)      { return NULL; }
LPAUDIOPLAYER TIMIDITY_Init(VOID) { return NULL; }
LPAUDIOPLAYER TSF_Init(VOID)      { return NULL; }
