/* -*- mode: c; tab-width: 4; c-basic-offset: 4; c-file-style: "linux" -*- */
//
// Copyright (c) 2009-2011, Wei Mingzhi <whistler_wmz@users.sf.net>.
// Copyright (c) 2011-2026, SDLPAL development team.
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

#include "font.h"
#include "util.h"
#include "text.h"

#define _FONT_C

#include "fontglyph.h"

/* ★★★ LinTOS 移植：**全部字模资源外置到 TF 卡**（flash 不再占一字节）
 *
 *   原版存在四块常驻 flash 的各式字模，共 1.05MB：
 *     unicode_font[65536][32]  2MB   可写（运行时要往里写 ASCII / wor16.fon）
 *     fontglyph_cn[8836]       293KB 简体内置字模
 *     fontglyph_tw[13826]      459KB 繁体内置字模
 *     fontglyph_jp[6879]       228KB 日文内置字模
 *     font_width[65536]         64KB 字宽表
 *   factory 分区只有 5MB，全放下去直接溢出。
 *   ⇒ 全部由 tools/gen_pal_fontbin.py 导成 TF 卡文件，运行时按需读进 PSRAM。
 *
 *   TF 卡 /sdcard/pal/ 需要：
 *     fontglyph.bin     2,162,688 B  unicode_font(2MB) + font_width(64KB)
 *     fontglyph_cn.bin    300,424 B  简体
 *     fontglyph_tw.bin    470,084 B  繁体（玩台版才用）
 *     fontglyph_jp.bin    233,886 B  日文（玩日版才用）
 *   缺文件不会崩，只是对应字形缺失。 */
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "font_zh16.h"      /* LinTOS 系统字库: 文泉驿点阵宋体 16px @appdata */

#define PAL_GLYPH_OFF_WIDTH  ((long)UNICODE_FONT_COUNT * 32)

static unsigned char (*unicode_font)[32] = NULL;
static unsigned char *font_width        = NULL;
static int _font_height = 16;

bool PAL_EnsureFontTable(void)
{
    if (unicode_font) return true;

    const size_t gbytes = (size_t)UNICODE_FONT_COUNT * 32;
    unicode_font = heap_caps_malloc(gbytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    font_width   = heap_caps_malloc(UNICODE_FONT_COUNT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!unicode_font || !font_width) {
        ESP_LOGE("PALFONT", "字模/字宽 PSRAM 分配失败 (need %u+%u)",
                 (unsigned)gbytes, (unsigned)UNICODE_FONT_COUNT);
        return false;
    }
    memset(unicode_font, 0, gbytes);
    memset(font_width, 32, UNICODE_FONT_COUNT);      /* 默认全角，与原表末尾一致 */

    FILE *fp = fopen(PAL_PREFIX "fontglyph.bin", "rb");
    if (!fp) {
        ESP_LOGW("PALFONT", "缺少 %sfontglyph.bin ⇒ 所有汉字将为空白", PAL_PREFIX);
        return true;                                  /* ASCII 仍可用 */
    }
    size_t ng = fread(unicode_font, 1, gbytes, fp);
    size_t nw = fread(font_width, 1, UNICODE_FONT_COUNT, fp);
    fclose(fp);
    if (ng != gbytes) ESP_LOGW("PALFONT", "fontglyph.bin 字模只读到 %u/%u", (unsigned)ng, (unsigned)gbytes);
    if (nw != UNICODE_FONT_COUNT) ESP_LOGW("PALFONT", "字宽表只读到 %u/%u ⇒ 使用默认 32", (unsigned)nw, (unsigned)UNICODE_FONT_COUNT);
    if (ng == gbytes && nw == UNICODE_FONT_COUNT)
        ESP_LOGI("PALFONT", "字模表已加载: %u GB + %u B 字宽 (均在 PSRAM)", (unsigned)ng, (unsigned)nw);
    return true;
}

/* ★★★ 2026-10-25 修复 PSRAM 泄漏（实测一次进出仙剑永久少 2.59MB）:
 *   unicode_font = UNICODE_FONT_COUNT*32 = 2,097,152 B (2MB) + font_width 64KB。
 *   这两个缓冲原来**分配后从没有释放路径** —— 退出游戏后 2.16MB 永久占着 PSRAM,
 *   而它们只有玩游戏时才用得到 (桌面/其他应用一律不用)。
 *   现在由 pal_shutdown() 显式归还；下次进游戏 PAL_EnsureFontTable() 看到 NULL
 *   会重新从 fontglyph.bin 加载 (SD 读 2MB 约 0.1~0.2s, 载入页有进度条)。
 *   ⚠ 只置 NULL, 不动 _font_height 等标量 —— 那些是默认值, 重新加载时会被覆盖。 */
void PAL_FreeFontTable(void)
{
    if (unicode_font) { heap_caps_free(unicode_font); unicode_font = NULL; }
    if (font_width)   { heap_caps_free(font_width);   font_width   = NULL; }
}

/* 从 TF 卡读一套内置字模包并覆盖到 unicode_font。
 * 格式: [u32 count][count x (u16 code_le + 32B bitmap)]（由 gen_pal_fontbin.py 生成） */
static bool PAL_LoadGlyphPack(const char *name, int height)
{
    if (!PAL_EnsureFontTable()) return false;

    char path[80];
    snprintf(path, sizeof(path), PAL_PREFIX "%s", name);
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        ESP_LOGW("PALFONT", "缺少 %s ⇒ 该套字模不加载", path);
        return false;
    }
    uint32_t cnt = 0;
    if (fread(&cnt, 1, 4, fp) != 4 || cnt == 0 || cnt > UNICODE_FONT_COUNT) {
        ESP_LOGW("PALFONT", "%s 头部异常 (count=%u)", name, (unsigned)cnt);
        fclose(fp);
        return false;
    }
    uint8_t rec[34];
    int loaded = 0;
    for (uint32_t i = 0; i < cnt; i++) {
        if (fread(rec, 1, sizeof(rec), fp) != sizeof(rec)) break;
        int w = rec[0] | (rec[1] << 8);
        w = (w >= unicode_upper_base) ? (w - unicode_upper_base + unicode_lower_top) : w;
        if (w >= 0 && w < UNICODE_FONT_COUNT) {
            memcpy(unicode_font[w], rec + 2, 32);
            font_width[w] = 32;
            loaded++;
        }
    }
    fclose(fp);
    ESP_LOGI("PALFONT", "%s: 加载 %d/%u 个字形", name, loaded, (unsigned)cnt);
    _font_height = height;
    return loaded > 0;
}

/* ★★★ 优先用 LinTOS 系统字库（文泉驿点阵宋体 16px，mmap 自 appdata，零 flash 开销）
 *
 *   格式完全一致：16x16 1bpp，每行 2 字节，MSB 在左 ⇒ 直接 memcpy 进 unicode_font。
 *   系统字库字符表是定长 3 字节 UTF-8（只收全角汉字/标点），ASCII 由 PAL_LoadISOFont 补。
 *   ⇒ TF 卡上**不需要任何字模文件**，游戏也能正常显示中文。
 */
static int PAL_LoadSystemFont(void)
{
    if (!font_zh16_ready()) {
        ESP_LOGW("PALFONT", "系统 16px 字库不可用 ⇒ 尝试 TF 卡字模");
        return 0;
    }
    if (!PAL_EnsureFontTable()) return 0;

    const int n = font_zh16_count();
    const unsigned char *chars = (const unsigned char *)zh16_chars;
    if (!chars || n <= 0) return 0;

    int loaded = 0;
    for (int i = 0; i < n; i++) {
        const unsigned char *u = chars + (size_t)i * 3;
        if ((u[0] & 0xF8) == 0xF0) continue;          /* 4 字节 UTF-8（BMP 外）跳过 */
        int w;
        if ((u[0] & 0xE0) == 0xC0)      w = ((u[0] & 0x1F) << 6) | (u[1] & 0x3F);
        else if ((u[0] & 0xF0) == 0xE0) w = ((u[0] & 0x0F) << 12) | ((u[1] & 0x3F) << 6) | (u[2] & 0x3F);
        else continue;                                 /* 单字节字符交给 ISO 字体 */
        if (w <= 0 || w >= UNICODE_FONT_COUNT) continue;
        const uint8_t *b = font_zh16_get_bitmap_by_index(i);
        if (!b) continue;
        memcpy(unicode_font[w], b, 32);
        font_width[w] = 32;                            /* 32 = 16 像素全角 */
        loaded++;
    }
    _font_height = 16;
    ESP_LOGI("PALFONT", "系统字库已接管: %d/%d 个字形", loaded, n);
    return loaded;
}

#include "ascii.h"

//#define POINT_IMPORT 1

char *font_offset_x;
char *font_offset_y;

static uint8_t reverseBits(uint8_t x) {
    uint8_t y = 0;
    for (int i = 0 ; i < 8; i++){
        y <<= 1;
        y |= (x & 1);
        x >>= 1;
    }
    return y;
}

#ifdef DEBUG
void dump_font(BYTE *buf,int rows, int cols)
{
   for(int row=0;row<rows;row++)
   {
      for( int bit=0;bit<cols;bit++)
         if( buf[row] & (uint8_t)pow(2,bit) )
            printf("*");
         else
            printf(" ");
      printf("\n");
   }
}

static uint16_t reverseBits16(uint16_t x)
{
   //specified to unifont structure; not common means
   uint8_t l = reverseBits(x);
   uint8_t h = reverseBits(x >> 8);
   return h << 8 | l;
}

void dump_font16(WORD *buf,int rows, int cols)
{
   for(int row=0;row<rows;row++)
   {
      for( int bit=0;bit<cols;bit++)
         if( reverseBits16(buf[row]) & (uint16_t)pow(2,bit) )
            printf("*");
         else
            printf(" ");
      printf("\n");
   }
}
#endif

static void PAL_LoadISOFont(void)
{
    int         i, j;
    if (!PAL_EnsureFontTable()) return;

    for (i = 0; i < sizeof(iso_font) / 15; i++)
    {
        for (j = 0; j < 15; j++)
        {
            unicode_font[i][j] = reverseBits(iso_font[i * 15 + j]);
        }

        unicode_font[i][15] = 0;
        font_width[i] = 16;
    }
}

static void PAL_LoadEmbeddedFont(void)
{
	FILE *fp;
	char *char_buf;
	wchar_t *wchar_buf;
	size_t nBytes;
	int nChars, i;

	//
	// Load the wor16.asc file.
	//
	if (NULL == (fp = UTIL_OpenFile("wor16.asc")))
	{
		return;
	}

	//
	// Get the size of wor16.asc file.
	//
	fseek(fp, 0, SEEK_END);
	nBytes = ftell(fp);

	//
	// Allocate buffer & read all the character codes.
	//
	if (NULL == (char_buf = (char *)malloc(nBytes)))
	{
		fclose(fp);
		return;
	}
	fseek(fp, 0, SEEK_SET);
	if (fread(char_buf, 1, nBytes, fp) < nBytes)
	{
		fclose(fp);
		return;
	}

	//
	// Close wor16.asc file.
	//
	fclose(fp);

	//
	// Detect the codepage of 'wor16.asc' and exit if not BIG5 or probability < 99
	// Note: 100% probability is impossible as the function does not recognize some special
	// characters such as bopomofo that may be used by 'wor16.asc'.
	//
	if (PAL_DetectCodePageForString(char_buf, nBytes, CP_BIG5, &i) != CP_BIG5 || i < 99)
	{
		free(char_buf);
		return;
	}

	//
	// Convert characters into unicode
	// Explictly specify BIG5 here for compatibility with codepage auto-detection
	//
	nChars = PAL_MultiByteToWideCharCP(CP_BIG5, char_buf, nBytes, NULL, 0);
	if (NULL == (wchar_buf = (wchar_t *)malloc(nChars * sizeof(wchar_t))))
	{
		free(char_buf);
		return;
	}
	PAL_MultiByteToWideCharCP(CP_BIG5, char_buf, nBytes, wchar_buf, nChars);
	free(char_buf);

	//
	// Read bitmaps from wor16.fon file.
	//
	fp = UTIL_OpenFile("wor16.fon");

	//
	// The font glyph data begins at offset 0x682 in wor16.fon.
	//
	fseek(fp, 0x682, SEEK_SET);

	//
	// Replace the original fonts
	//
	for (i = 0; i < nChars; i++)
	{
		wchar_t w = (wchar_buf[i] >= unicode_upper_base) ? (wchar_buf[i] - unicode_upper_base + unicode_lower_top) : wchar_buf[i];
		if (w >= UNICODE_FONT_COUNT) { continue; }
		if (fread(unicode_font[w], 30, 1, fp) == 1)
		{
			unicode_font[w][30] = 0;
			unicode_font[w][31] = 0;
		}
		font_width[w] = 32;
	}
	free(wchar_buf);

	fclose(fp);

	_font_height = 15;
}

INT
PAL_LoadUserFont(
   LPCSTR      pszBdfFileName
)
/*++
  Purpose:

    Loads a BDF bitmap font file.

  Parameters:

    [IN]  pszBdfFileName - Name of BDF bitmap font file..

  Return value:

    0 = success, -1 = failure.

--*/
{
   char buf[4096];
   int state = 0, fstate = 0;
   int codepage = -1;
   DWORD dwEncoding = 0;
   BYTE bFontGlyph[32] = {0};
   int iCurHeight = 0;
   int bbw = 0, bbh = 0, bbox, bboy;
   int got_size;
   int size_x, size_y;
   int cstate = 0;
   int font_w_global = 0;
   int font_w = 16;

   FILE *fp = UTIL_OpenFileForMode(pszBdfFileName, "r");

   if (fp == NULL)
   {
      return -1;
   }

   _font_height = 16;

   while (fgets(buf, 4096, fp) != NULL)
   {
      if (state == 0)
      {
         if (strncmp(buf, "CHARSET_REGISTRY", 16) == 0)
         {
            if (strcasestr(buf, "Big5") != NULL)
            {
               codepage = CP_BIG5;
            }
            else if (strcasestr(buf, "GBK") != NULL || strcasestr(buf, "GB2312") != NULL)
            {
               codepage = CP_GBK;
            }
            else if (strcasestr(buf, "ISO10646") != NULL)
            {
               codepage = CP_UCS;
            }
            //else if (strstr(buf, "JISX0208") != NULL)
            //
            //  codepage = CP_JISX0208;
            //}
            else
            {
               TerminateOnError("Charset of %s is %s, which is not a supported yet.",pszBdfFileName,buf);
            }
         }
         else if (strncmp(buf, "STARTCHAR", 9) == 0)
         {
            cstate = 1;
            font_w = font_w_global;
         }
         else if (strncmp(buf, "ENCODING", 8) == 0)
         {
            dwEncoding = atoi(buf + 8);
         }
         else if (strncmp(buf, "SIZE", 3) == 0)
         {
            int bytes_consumed = 0, bytes_now;
            BOOL got_expected = FALSE;
            char size[10];
            sscanf(buf + bytes_consumed, "%s%n", size, &bytes_now);
            bytes_consumed += bytes_now;
            sscanf(buf + bytes_consumed, "%d%n", &got_size, &bytes_now); bytes_consumed += bytes_now;
            sscanf(buf + bytes_consumed, "%d%n", &size_x, &bytes_now); bytes_consumed += bytes_now;
            sscanf(buf + bytes_consumed, "%d%n", &size_y, &bytes_now); bytes_consumed += bytes_now;
               if (got_size == 16 || got_size == 15)
               {
                  _font_height = got_size;
                  got_expected = TRUE;
               }
            if (!got_expected)
               TerminateOnError("%s not contains expected font size 15/16!", pszBdfFileName);
         }
         /*else if (strncmp(buf, "SWIDTH", 6) == 0)
         {
             int bytes_consumed = 0, bytes_now;
             char swidth[10];
             int swidth_x, swidth_y;
             sscanf(buf + bytes_consumed, "%s%n", swidth, &bytes_now); bytes_consumed += bytes_now;
             sscanf(buf + bytes_consumed, "%d%n", &swidth_x, &bytes_now); bytes_consumed += bytes_now;
             sscanf(buf + bytes_consumed, "%d%n", &swidth_y, &bytes_now); bytes_consumed += bytes_now;
             font_w = (int)ceil(swidth_x / 1000.0 * (size_x / 72) * got_size / 2) << 1;
             if( font_w > 8 && font_w <= 16 )
                font_w = 16;
             if( !cstate ) {
                 swidth_x_global = swidth_x;
                 swidth_y_global = swidth_y;
                 font_w_global = font_w;
             }
         }*/
         else if (strncmp(buf, "DWIDTH", 6) == 0)
         {
             int bytes_consumed = 0, bytes_now;
             char dwidth[10];
             int dwidth_x, dwidth_y;
             sscanf(buf + bytes_consumed, "%s%n", dwidth, &bytes_now); bytes_consumed += bytes_now;
             sscanf(buf + bytes_consumed, "%d%n", &dwidth_x, &bytes_now); bytes_consumed += bytes_now;
             sscanf(buf + bytes_consumed, "%d%n", &dwidth_y, &bytes_now); bytes_consumed += bytes_now;
             font_w = dwidth_x;
             if (font_w > 8 && font_w <= 16)
                 font_w = 16;
             if (!cstate) {
                 font_w_global = font_w;
             }
         }
         else if (strncmp(buf, "BBX", 3) == 0)
         {
            int bytes_consumed = 0, bytes_now;
            char bbx[10];
            sscanf(buf+bytes_consumed,"%s%n",bbx,&bytes_now);bytes_consumed += bytes_now;
            sscanf(buf+bytes_consumed,"%d%n",&bbw,&bytes_now);bytes_consumed += bytes_now;
            sscanf(buf+bytes_consumed,"%d%n",&bbh,&bytes_now);bytes_consumed += bytes_now;
            sscanf(buf+bytes_consumed,"%d%n",&bbox,&bytes_now);bytes_consumed += bytes_now;
            sscanf(buf+bytes_consumed,"%d%n",&bboy,&bytes_now);bytes_consumed += bytes_now;
         }
         else if (strncmp(buf, "BITMAP", 6) == 0)
         {
            state = 1;
            iCurHeight = 0;
            memset(bFontGlyph, 0, sizeof(bFontGlyph));
         }
      }
      else if (state == 1)
      {
         if (strncmp(buf, "ENDCHAR", 7) == 0)
         {
            //
            // Replace the original fonts
            //
            BYTE szCp[3];
            szCp[0] = (dwEncoding >> 8) & 0xFF;
            szCp[1] = dwEncoding & 0xFF;
            szCp[2] = 0;

            if (codepage == CP_GBK && dwEncoding > 0xFF)
            {
               szCp[0] |= 0x80;
               szCp[1] |= 0x80;
            }

            wchar_t wc[2] = { 0 };
            PAL_MultiByteToWideCharCP(codepage, (LPCSTR)szCp, 2, wc, 1);
            if (wc[0] != 0)
            {
               wchar_t w = (wc[0] >= unicode_upper_base) ? (wc[0] - unicode_upper_base + unicode_lower_top) : wc[0];
#if POINT_IMPORT
               if (font_w <= 8) {
                   bFontGlyph[0] = bFontGlyph[14] = 0xff;
                   for (int m = 1; m <= 14; m++)
                       bFontGlyph[m] |= 0x81;
               }
               else {
                   bFontGlyph[0] = bFontGlyph[1] = bFontGlyph[28] = bFontGlyph[29] = 0xff;
                   for (int m = 2; m <= 27; m++)
                       bFontGlyph[m] |= ((m % 2) ? 1 : 0x80);
               }
#endif
               if (w < UNICODE_FONT_COUNT)
               {
                  memcpy(unicode_font[w], bFontGlyph, sizeof(bFontGlyph));
                  font_width[w] = font_w << 1;
                  font_offset_x[w] = bbox;
                  font_offset_y[w] = _font_height - bboy - bbh - 4;
               }
               else
               {
                   UTIL_LogOutput(LOGLEVEL_DEBUG, "this user font has much code than unifont! ignoring %d\n", w);
               }
            }

            state = 0;
            cstate = 0;
         }
         else
         {
            if (iCurHeight < _font_height )
            {
               WORD wCode = strtoul(buf, NULL, 16);
               if(font_w <= 8)
               {
                  switch(fstate)
                  {
                     case 0:
                        bFontGlyph[iCurHeight * 2] = wCode;
                        fstate = 1;
                        break;
                     case 1:
                        bFontGlyph[iCurHeight * 2+1] = wCode;
                        fstate = 0;
                        iCurHeight++;
                        break;
                  }
               }else{
                  if(strnlen(buf,sizeof(buf))<=3){
                     bFontGlyph[iCurHeight * 2] = wCode;
                     bFontGlyph[iCurHeight * 2 + 1] = 0;
                  } else {
                     bFontGlyph[iCurHeight * 2] = (wCode >> 8);
                     bFontGlyph[iCurHeight * 2 + 1] = (wCode & 0xFF);
                  }
                  iCurHeight++;
               }
            }
         }
      }
   }

   fclose(fp);
   return 0;
}

int
PAL_InitFont(
   const CONFIGURATION* cfg
)
{
    /* ★★★ 必须先分配 PSRAM 字模/字宽表：
     *   PAL_LoadEmbeddedFont() 会直接往 unicode_font[w] / font_width[w] 写
     *   （wor16.asc + wor16.fon = 原版 DOS 字库）。表没分配就是 NULL 解引用 → 当场崩。
     *   ⛔ 另一颗雷：它读 wor16.fon 后**不判空**就 fseek，所以 wor16.fon 必须在卡上。 */
    if (!PAL_EnsureFontTable()) return -1;

    size_t fonts = UNICODE_FONT_COUNT;
    font_offset_x = calloc(1, fonts);
    font_offset_y = calloc(1, fonts);

   if (!cfg->pszMsgFile)
   {
      PAL_LoadEmbeddedFont();
   }

   if (g_TextLib.fUseISOFont)
   {
      PAL_LoadISOFont();
   }

   if (cfg->pszFontFile)
   {
      PAL_LoadUserFont(cfg->pszFontFile);
   }
   else if (_font_height != 15)
   {
      switch (g_TextLib.iFontFlavor)
      {
      case kFontFlavorAuto:
         switch (PAL_GetCodePage())
         {
         case CP_GBK:
            if (!PAL_LoadSystemFont()) PAL_LoadGlyphPack("fontglyph_cn.bin", 16);
            break;

         case CP_BIG5:
            PAL_LoadGlyphPack("fontglyph_tw.bin", 15);
            break;

         default:
            break;
         }
         break;

      case kFontFlavorSimpChin:
         if (!PAL_LoadSystemFont()) PAL_LoadGlyphPack("fontglyph_cn.bin", 16);
         break;

      case kFontFlavorTradChin:
         PAL_LoadGlyphPack("fontglyph_tw.bin", 15);
         break;

      case kFontFlavorJapanese:
         PAL_LoadGlyphPack("fontglyph_jp.bin", 16);
         break;

      case kFontFlavorUnifont:
      default:
         break;
      }
   }

   return 0;
}

void
PAL_FreeFont(
	void
)
{
    free(font_offset_x);
    free(font_offset_y);
}

void
PAL_DrawCharOnSurface(
	uint16_t                 wChar,
	SDL_Surface             *lpSurface,
	PAL_POS                  pos,
	uint8_t                  bColor,
	BOOL                     fUse8x8Font
)
{
	int       i, j;
	int       x = PAL_X(pos), y = PAL_Y(pos);
    int       x_offset, y_offset;

	//
	// Check for NULL pointer & invalid char code.
	//
	if (lpSurface == NULL || (wChar >= unicode_lower_top && wChar < unicode_upper_base) ||
		wChar >= unicode_upper_top || (_font_height == 8 && wChar >= 0x100))
	{
		return;
	}

	//
	// Locate for this character in the font lib.
	//
	if (wChar >= unicode_upper_base)
	{
		wChar -= (unicode_upper_base - unicode_lower_top);
	}

    x_offset = font_offset_x[wChar];
    y_offset = font_offset_y[wChar];

	//
	// Draw the character to the surface.
	//
	LPBYTE dest = (LPBYTE)lpSurface->pixels + (int)max(y + y_offset, 0) * lpSurface->pitch + x;
	LPBYTE top = (LPBYTE)lpSurface->pixels + lpSurface->h * lpSurface->pitch;
	if (fUse8x8Font)
	{
		for (i = 0; i < 8 && dest < top; i++, dest += lpSurface->pitch)
		{
			for (j = 0; j < 8 && x + j < lpSurface->w; j++)
			{
				if (iso_font_8x8[wChar][i] & (1 << j))
				{
					dest[j] = bColor;
				}
			}
		}
	}
	else
	{
		if (font_width[wChar] == 32)
		{
			for (i = 0; i < _font_height * 2 && dest < top; i += 2, dest += lpSurface->pitch)
			{
				for (j = 0; j < 8 && x + j + x_offset < lpSurface->w && x + j + x_offset >= 0; j++)
				{
					if (unicode_font[wChar][i] & (1 << (7 - j)))
					{
						dest[j + x_offset] = bColor;
					}
				}
				for (j = 0; j < 8 && x + j + 8 + x_offset < lpSurface->w && x + j + 8 + x_offset >= 0; j++)
				{
					if (unicode_font[wChar][i + 1] & (1 << (7 - j)))
					{
						dest[j + 8 + x_offset] = bColor;
					}
				}
			}
		}
		else
		{
			for (i = 0; i < _font_height && dest < top; i++, dest += lpSurface->pitch)
			{
				for (j = 0; j < 8 && x + j + x_offset < lpSurface->w && x + j + x_offset >= 0; j++)
				{
					if (unicode_font[wChar][i] & (1 << (7 - j)))
					{
						dest[j + x_offset] = bColor;
					}
				}
			}
		}
	}
}

int
PAL_CharWidth(
	uint16_t                 wChar
)
{
	if ((wChar >= unicode_lower_top && wChar < unicode_upper_base) || wChar >= unicode_upper_top)
	{
		return 0;
	}

	//
	// Locate for this character in the font lib.
	//
	if (wChar >= unicode_upper_base)
	{
		wChar -= (unicode_upper_base - unicode_lower_top);
	}

	return font_width[wChar] >> 1;
}

int
PAL_FontHeight(
	void
)
{
	return _font_height;
}
