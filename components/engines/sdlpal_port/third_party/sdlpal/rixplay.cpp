/* -*- mode: c; tab-width: 4; c-basic-offset: 4; c-file-style: "linux" -*- */
//
// Copyright (c) 2009-2011, Wei Mingzhi <whistler_wmz@users.sf.net>.
// Copyright (c) 2011-2024, SDLPAL development team.
// All rights reserved.
//
// This file is part of SDLPAL.
//
// SDLPAL is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 3
// as published by the Free Software Foundation.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include <math.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "global.h"
#include "palcfg.h"
#include "players.h"
#include "audio.h"

#include "resampler.h"
#include "adplug/opl.h"
#include "adplug/emuopls.h"
#include "adplug/surroundopl.h"
#include "adplug/convertopl.h"
#include "adplug/rix.h"

/* ★ 2026-10-25 [LINTOS]: 音频回调的"安全暂停"接口, 实现在 sdl_audio.c (C 文件)。
 *   换曲目时要在游戏线程做 rix->rewind(), 期间必须让音频回调退出, 否则两个线程
 *   会同时访问同一个 OPL 合成器。必须 extern "C" 才不会名字修饰不匹配。 */
extern "C" {
void pal_audio_guard_begin(void);
void pal_audio_guard_end(void);
}

typedef struct tagRIXPLAYER :
	public AUDIOPLAYER
{
   Copl                      *opl;
   CrixPlayer                *rix;
   void                      *resampler[2];
   //
   // Mixing scratch for one 70 Hz RIX tick. Sized from the effective output
   // rate at init time (see RIX_Init) so it never has to be a compile-time
   // worst case, and so an arbitrarily high SampleRate cannot inflate the
   // player struct.
   //
   LPBYTE                     buf;
   INT                        buf_size;
   LPBYTE                     pos;
   INT                        iNextMusic; // the next music number to switch to
   DWORD                      dwStartFadeTime;
   INT                        iTotalFadeOutSamples;
   INT                        iTotalFadeInSamples;
   INT                        iRemainingFadeSamples;
   enum { NONE, FADE_IN, FADE_OUT } FadeType; // fade in or fade out ?
   BOOL                       fNextLoop;
   BOOL                       fReady;
   /* ★ 2026-10-25 [LINTOS]: RIX_Play 已经在本线程把 rix->rewind() 做完了,
    *   回调看到这个标志就只切曲号、不再重复做那 190ms 的重活。 */
   BOOL                       fPreRewound;
} RIXPLAYER, *LPRIXPLAYER;

/* ★ 2026-10-25 [LINTOS] 诊断: "第一首 BGM 有声音, 切第二首就没了"。
 *   回调 cb_max=0ms / peak=0 ⇒ RIX_FillBuffer 提前 return 了。这里在每个 return
 *   点打一行(500ms 限流), 一次就能分清是哪条路径退出的。 */
static void
rix_diag(
	const char *why,
	LPRIXPLAYER p
)
{
	static uint32_t last = 0;
	uint32_t now = SDL_GetTicks();
	if (now - last < 500) return;
	last = now;
	if (p == NULL)
	{
		ESP_LOGW("PALRIX", "%s: player=NULL", why);
		return;
	}
	ESP_LOGW("PALRIX", "%s: mus=%d next=%d fade=%d pre=%d loop=%d ready=%d",
		why, (int)p->iMusic, (int)p->iNextMusic, (int)p->FadeType,
		(int)p->fPreRewound, (int)p->fLoop, (int)p->fReady);
}

static VOID
RIX_FillBuffer(
	VOID      *object,
	LPBYTE     stream,
	INT        len
)
/*++
	Purpose:

	Fill the background music into the sound buffer. Called by the SDL sound
	callback function only (audio.c: AUDIO_FillBuffer).

	Parameters:

	[OUT] stream - pointer to the stream buffer.

	[IN]  len - Length of the buffer.

	Return value:

	None.

--*/
{
	LPRIXPLAYER pRixPlayer = (LPRIXPLAYER)object;

	if (pRixPlayer == NULL || !pRixPlayer->fReady)
	{
		//
		// Not initialized
		//
		rix_diag("not-ready", pRixPlayer);
		return;
	}

	while (len > 0)
	{
		INT       volume, delta_samples = 0, vol_delta = 0;

		//
		// fading in or fading out
		//
		switch (pRixPlayer->FadeType)
		{
		case RIXPLAYER::FADE_IN:
			if (pRixPlayer->iRemainingFadeSamples <= 0)
			{
				pRixPlayer->FadeType = RIXPLAYER::NONE;
				volume = SDL_MIX_MAXVOLUME;
			}
			else
			{
				volume = (INT)(SDL_MIX_MAXVOLUME * (1.0 - (double)pRixPlayer->iRemainingFadeSamples / pRixPlayer->iTotalFadeInSamples));
				delta_samples = (pRixPlayer->iTotalFadeInSamples / SDL_MIX_MAXVOLUME) & ~(gConfig.iAudioChannels - 1); vol_delta = 1;
			}
			break;
		case RIXPLAYER::FADE_OUT:
			if (pRixPlayer->iTotalFadeOutSamples == pRixPlayer->iRemainingFadeSamples && pRixPlayer->iTotalFadeOutSamples > 0)
			{
				UINT  now = SDL_GetTicks();
				INT   passed_samples = ((INT)(now - pRixPlayer->dwStartFadeTime) > 0) ? (INT)((now - pRixPlayer->dwStartFadeTime) * AUDIO_GetDeviceSpec()->freq / 1000) : 0;
				pRixPlayer->iRemainingFadeSamples -= passed_samples;
			}
			if (pRixPlayer->iMusic == -1 || pRixPlayer->iRemainingFadeSamples <= 0)
			{
				//
				// There is no current playing music, or fading time has passed.
				// Start playing the next one or stop playing.
				//
				if (pRixPlayer->iNextMusic > 0)
				{
					pRixPlayer->iMusic = pRixPlayer->iNextMusic;
					pRixPlayer->iNextMusic = -1;
					pRixPlayer->fLoop = pRixPlayer->fNextLoop;
					pRixPlayer->FadeType = RIXPLAYER::FADE_IN;
					if (pRixPlayer->iMusic > 0)
						pRixPlayer->dwStartFadeTime += pRixPlayer->iTotalFadeOutSamples * 1000 / gConfig.iSampleRate;
					else
						pRixPlayer->dwStartFadeTime = SDL_GetTicks();
					pRixPlayer->iTotalFadeOutSamples = 0;
					pRixPlayer->iRemainingFadeSamples = pRixPlayer->iTotalFadeInSamples;
					/* ★ 2026-10-25: 正常路径已经在 RIX_Play() 里把 rewind 做完(fPreRewound),
					 *   这里不再重复那 190ms 的重活; 只有兜底情况(未预做)才就地执行。 */
					if (!pRixPlayer->fPreRewound)
					{
						pRixPlayer->rix->rewind(pRixPlayer->iMusic);
						if (pRixPlayer->resampler[0]) resampler_clear(pRixPlayer->resampler[0]);
						if (pRixPlayer->resampler[1]) resampler_clear(pRixPlayer->resampler[1]);
					}
					pRixPlayer->fPreRewound = FALSE;
					continue;
				}
				else
				{
					pRixPlayer->iMusic = -1;
					pRixPlayer->FadeType = RIXPLAYER::NONE;
					rix_diag("fadeout-no-next", pRixPlayer);
					return;
				}
			}
			else
			{
				volume = (INT)(SDL_MIX_MAXVOLUME * ((double)pRixPlayer->iRemainingFadeSamples / pRixPlayer->iTotalFadeOutSamples));
				delta_samples = (pRixPlayer->iTotalFadeOutSamples / SDL_MIX_MAXVOLUME) & ~(gConfig.iAudioChannels - 1); vol_delta = -1;
			}
			break;
		default:
			if (pRixPlayer->iMusic <= 0)
			{
				//
				// No current playing music
				//
				rix_diag("no-music", pRixPlayer);
				return;
			}
			else
			{
				volume = SDL_MIX_MAXVOLUME;
			}
		}

		//
		// Fill the buffer with sound data
		//
		int buf_max_len = pRixPlayer->buf_size;
		bool fContinue = true;
		while (len > 0 && fContinue)
		{
			if (pRixPlayer->pos == NULL || pRixPlayer->pos - pRixPlayer->buf >= buf_max_len)
			{
				pRixPlayer->pos = pRixPlayer->buf;
				if (!pRixPlayer->rix->update())
				{
					if (!pRixPlayer->fLoop)
					{
						//
						// Not loop, simply terminate the music
						//
						pRixPlayer->iMusic = -1;
						if (pRixPlayer->FadeType != RIXPLAYER::FADE_OUT && pRixPlayer->iNextMusic == -1)
						{
							pRixPlayer->FadeType = RIXPLAYER::NONE;
						}
						rix_diag("end-noloop", pRixPlayer);
						return;
					}
					pRixPlayer->rix->rewindReInit(pRixPlayer->iMusic, false);
					if (!pRixPlayer->rix->update())
					{
						//
						// Something must be wrong
						//
						pRixPlayer->iMusic = -1;
						pRixPlayer->FadeType = RIXPLAYER::NONE;
						rix_diag("reinit-fail", pRixPlayer);
						return;
					}
				}
				int sample_count = gConfig.iSampleRate / 70;
				if (pRixPlayer->resampler[0])
				{
					unsigned int samples_written = 0;
					short *finalBuf = (short*)pRixPlayer->buf;

					while (sample_count)
					{
						int to_write = resampler_get_free_count(pRixPlayer->resampler[0]);
						if (to_write)
						{
							short *tempBuf = (short*)alloca(to_write * gConfig.iAudioChannels * sizeof(short));
							int temp_buf_read = 0;
							pRixPlayer->opl->update(tempBuf, to_write);
							for (int i = 0; i < to_write * gConfig.iAudioChannels; i++)
								resampler_write_sample(pRixPlayer->resampler[i % gConfig.iAudioChannels], tempBuf[temp_buf_read++]);
						}

						int to_get = resampler_get_sample_count(pRixPlayer->resampler[0]);
						if (to_get > sample_count) to_get = sample_count;
						for (int i = 0; i < to_get * gConfig.iAudioChannels; i++)
							finalBuf[samples_written++] = resampler_get_and_remove_sample(pRixPlayer->resampler[i % gConfig.iAudioChannels]);
						sample_count -= to_get;
					}
				}
				else
				{
					pRixPlayer->opl->update((short *)(pRixPlayer->buf), sample_count);
				}
			}

			int l = buf_max_len - (pRixPlayer->pos - pRixPlayer->buf);
			l = (l > len) ? len / sizeof(short) : l / sizeof(short);

			//
			// Put audio data into buffer and adjust volume
			//
			if (pRixPlayer->FadeType != RIXPLAYER::NONE)
			{
				short* ptr = (short*)stream;
				for (int i = 0; i < l && pRixPlayer->iRemainingFadeSamples > 0; volume += vol_delta)
				{
					int j = 0;
					for (j = 0; i < l && j < delta_samples; i++, j++)
					{
						*ptr++ = *(short*)pRixPlayer->pos * volume / SDL_MIX_MAXVOLUME;
						pRixPlayer->pos += sizeof(short);
					}
					pRixPlayer->iRemainingFadeSamples -= j;
				}
				fContinue = (pRixPlayer->iRemainingFadeSamples > 0);
				len -= (LPBYTE)ptr - stream; stream = (LPBYTE)ptr;
			}
			else
			{
				memcpy(stream, pRixPlayer->pos, l * sizeof(short));
				pRixPlayer->pos += l * sizeof(short);
				stream += l * sizeof(short);
				len -= l * sizeof(short);
			}
		}
	}
}

// Size in bytes of one 70 Hz RIX tick of mixed output at the current effective
// sample rate. The +69/+1 rounding keeps this an upper bound on
// (iSampleRate / 70) * channels * sizeof(short) even when the rate is not a
// multiple of 70, so the tick can never overrun the buffer.
static INT
RIX_GetBufferSize(
	VOID
)
{
	return (INT)(((long long)gConfig.iSampleRate + 69) / 70) * gConfig.iAudioChannels * (INT)sizeof(short);
}

static VOID
RIX_Shutdown(
	VOID     *object
)
/*++
	Purpose:

	Shutdown the RIX player subsystem.

	Parameters:

	None.

	Return value:

	None.

--*/
{
	if (object != NULL)
	{
		LPRIXPLAYER pRixPlayer = (LPRIXPLAYER)object;
		pRixPlayer->fReady = FALSE;
		for (int i = 0; i < gConfig.iAudioChannels; i++)
			if (pRixPlayer->resampler[i])
				resampler_delete(pRixPlayer->resampler[i]);
		delete pRixPlayer->rix;
		delete pRixPlayer->opl;
		free(pRixPlayer->buf);
		delete pRixPlayer;
	}
}

static BOOL
RIX_Play(
	VOID     *object,
	INT       iNumRIX,
	BOOL      fLoop,
	FLOAT     flFadeTime
)
/*++
	Purpose:

	Start playing the specified music.

	Parameters:

	[IN]  iNumRIX - number of the music. 0 to stop playing current music.

	[IN]  fLoop - Whether the music should be looped or not.

	[IN]  flFadeTime - the fade in/out time when switching music.

	Return value:

	None.

--*/
{
	LPRIXPLAYER pRixPlayer = (LPRIXPLAYER)object;

	//
	// Check for NULL pointer.
	//
	if (pRixPlayer == NULL)
	{
		return FALSE;
	}

	if (iNumRIX == pRixPlayer->iMusic && pRixPlayer->iNextMusic == -1)
	{
		/* Will play the same music without any pending play changes,
		   just change the loop attribute */
		pRixPlayer->fLoop = fLoop;
		return TRUE;
	}

	if (pRixPlayer->FadeType != RIXPLAYER::FADE_OUT)
	{
		if (pRixPlayer->FadeType == RIXPLAYER::FADE_IN && pRixPlayer->iTotalFadeInSamples > 0 && pRixPlayer->iRemainingFadeSamples > 0)
		{
			pRixPlayer->dwStartFadeTime = SDL_GetTicks() - (int)((float)pRixPlayer->iRemainingFadeSamples / pRixPlayer->iTotalFadeInSamples * flFadeTime * (1000 / 2));
		}
		else
		{
			pRixPlayer->dwStartFadeTime = SDL_GetTicks();
		}
		pRixPlayer->iTotalFadeOutSamples = (int)round(flFadeTime / 2.0f * gConfig.iSampleRate) * gConfig.iAudioChannels;
		pRixPlayer->iRemainingFadeSamples = pRixPlayer->iTotalFadeOutSamples;
		pRixPlayer->iTotalFadeInSamples = pRixPlayer->iTotalFadeOutSamples;
	}
	else
	{
		pRixPlayer->iTotalFadeInSamples = (int)round(flFadeTime / 2.0f * gConfig.iSampleRate) * gConfig.iAudioChannels;
	}

	/* ★★★ 2026-10-25 [LINTOS] 解决"实时 OPL 合成没声音"的关键一步:
	 *   原来 RIX_Play 只记下 iNextMusic, 真正的 rix->rewind() 被拖到**下一次音频回调**
	 *   里执行。而 rewind 要重发整套 OPL 寄存器, 实测耗时 190ms —— 一块音频只有 23ms,
	 *   于是必然欠载 8 倍; 欠载期间回调拿不到有效样本, 该统计窗口的 peak 只剩个位数,
	 *   听感就是"进游戏后没声音"(而且不报错)。
	 *   实测对照(同一次开机、连续三次进入):
	 *     第 1 次(没换曲) cb_max=5ms   peak=2956  ← 正常
	 *     第 2 次(换曲)   cb_max=190ms peak=9     ← 静音
	 *   现在改成: 在**游戏线程**里先把 rewind 做完(期间暂停回调 60ms 以免并发访问 OPL),
	 *   回调只需 iMusic=iNextMusic 立即切换, 不再有 190ms 阻塞。
	 *   ⚠ 用 pal_audio_guard_* 而不是 SDL_LockAudio: 后者的 shim 是空实现。 */
	if (iNumRIX > 0 && pRixPlayer->rix)
	{
		pal_audio_guard_begin();
		pRixPlayer->rix->rewind(iNumRIX);
		if (pRixPlayer->resampler[0]) resampler_clear(pRixPlayer->resampler[0]);
		if (pRixPlayer->resampler[1]) resampler_clear(pRixPlayer->resampler[1]);
		pal_audio_guard_end();
		pRixPlayer->fPreRewound = TRUE;
	}

	pRixPlayer->iNextMusic = iNumRIX;
	pRixPlayer->FadeType = RIXPLAYER::FADE_OUT;
	pRixPlayer->fNextLoop = fLoop;
	pRixPlayer->fReady = TRUE;

	return TRUE;
}

/* [LINTOS] 诊断接口: 串口 `palaud` 用, 判断"没声音"是没播(fReady/iMusic)、
 * 还是在播但合成全零。只读, 不影响播放逻辑。 */
static LPRIXPLAYER s_rix_diag = NULL;
extern "C" void rix_diag(int *ready, int *music, int *next, int *fade, int *bufsize)
{
	if (!s_rix_diag) { *ready = -1; *music = -1; *next = -1; *fade = -1; *bufsize = 0; return; }
	*ready   = s_rix_diag->fReady ? 1 : 0;
	*music   = s_rix_diag->iMusic;
	*next    = s_rix_diag->iNextMusic;
	*fade    = (int)s_rix_diag->FadeType;
	*bufsize = (int)s_rix_diag->buf_size;
}

LPAUDIOPLAYER
RIX_Init(
	LPCSTR     szFileName
)
/*++
  Purpose:

    Initialize the RIX player subsystem.

  Parameters:

    [IN]  szFileName - Filename of the mus.mkf file.

  Return value:

    0 if success, -1 if cannot allocate memory, -2 if file not found.
--*/
{
	if (!szFileName) return NULL;

	LPRIXPLAYER pRixPlayer = new RIXPLAYER;
	if (pRixPlayer == NULL)
	{
		return NULL;
	}
	else
	{
		memset(pRixPlayer, 0, sizeof(RIXPLAYER));
		pRixPlayer->FillBuffer = RIX_FillBuffer;
		pRixPlayer->Shutdown = RIX_Shutdown;
		pRixPlayer->Play = RIX_Play;

		//
		// Allocate the per-tick mixing scratch at the effective sample rate.
		// This replaces a compile-time worst-case array so that a high
		// SampleRate costs only the memory it actually needs.
		//
		pRixPlayer->buf_size = RIX_GetBufferSize();
		if (pRixPlayer->buf_size <= 0)
		{
			delete pRixPlayer;
			return NULL;
		}
		pRixPlayer->buf = (LPBYTE)calloc(1, pRixPlayer->buf_size);
		if (pRixPlayer->buf == NULL)
		{
			delete pRixPlayer;
			return NULL;
		}
	}

	auto chip = (Copl::ChipType)gConfig.eOPLChip;
	if (chip == Copl::TYPE_OPL2 && gConfig.fUseSurroundOPL)
	{
		chip = Copl::TYPE_DUAL_OPL2;
	}

	Copl* opl = CEmuopl::CreateEmuopl((OPLCORE::TYPE)gConfig.eOPLCore, chip, gConfig.iOPLSampleRate);
	if (NULL == opl)
	{
		RIX_Shutdown(pRixPlayer);
		return NULL;
	}

	if (gConfig.fUseSurroundOPL)
	{
		Copl* tmpopl = new CSurroundopl(gConfig.iOPLSampleRate, gConfig.iSurroundOPLOffset, opl);
		if (NULL == tmpopl)
		{
			delete opl;
			RIX_Shutdown(pRixPlayer);
			return NULL;
		}
		opl = tmpopl;
	}

	pRixPlayer->opl = new CConvertopl(opl, true, gConfig.iAudioChannels == 2);
	if (pRixPlayer->opl == NULL)
	{
		delete opl;
		RIX_Shutdown(pRixPlayer);
		return NULL;
	}

	pRixPlayer->rix = new CrixPlayer(pRixPlayer->opl);
	if (pRixPlayer->rix == NULL)
	{
		RIX_Shutdown(pRixPlayer);
		return NULL;
	}

	//
	// Load the MKF file.
	//
	if (!pRixPlayer->rix->load(szFileName, CProvider_Filesystem()))
	{
		RIX_Shutdown(pRixPlayer);
		return NULL;
	}

	if (gConfig.iOPLSampleRate != gConfig.iSampleRate)
	{
		for (int i = 0; i < gConfig.iAudioChannels; i++)
		{
			pRixPlayer->resampler[i] = resampler_create();
			resampler_set_quality(pRixPlayer->resampler[i], AUDIO_IsIntegerConversion(gConfig.iOPLSampleRate) ? RESAMPLER_QUALITY_MIN : gConfig.iResampleQuality);
			resampler_set_rate(pRixPlayer->resampler[i], (double)gConfig.iOPLSampleRate / (double)gConfig.iSampleRate);
		}
	}

#if USE_RIX_EXTRA_INIT
	if (gConfig.pExtraFMRegs && gConfig.pExtraFMVals)
	{
		pRixPlayer->rix->set_extra_init(gConfig.pExtraFMRegs, gConfig.pExtraFMVals, gConfig.dwExtraLength);
	}
#endif

	//
	// Success.
	//
	pRixPlayer->FadeType = RIXPLAYER::NONE;
	pRixPlayer->iMusic = pRixPlayer->iNextMusic = -1;
	pRixPlayer->pos = NULL;
	pRixPlayer->fLoop = FALSE;
	pRixPlayer->fNextLoop = FALSE;
	pRixPlayer->fReady = FALSE;

	s_rix_diag = pRixPlayer;    /* [LINTOS] 诊断: 暴露内部状态给串口命令 palaud */

	return pRixPlayer;
}
