/*
 * Adplug - Replayer for many OPL2/OPL3 audio file formats.
 * Copyright (C) 1999 - 2007 Simon Peter, <dn.tlp@gmx.net>, et al.
 * 
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * 
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * rix.cpp - Softstar RIX OPL Format Player by palxex <palxex.ys168.com>
 *                                             BSPAL <BSPAL.ys168.com>
 */
#include "../common.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>
/* ★ 2026-10-25 [LINTOS] 本文件同时被"设备固件"和"PC 端离线渲染器
 *   (tools/pal_rix2wav.cpp)"编译。PC 上没有 ESP-IDF 头文件，
 *   所以这里按平台分流：设备用真日志/PSRAM 分配，PC 用空桩 + calloc。 */
#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_heap_caps.h"
#else
#define ESP_LOGI(tag, ...) ((void)0)
#define ESP_LOGW(tag, ...) ((void)0)
#define ESP_LOGE(tag, ...) ((void)0)
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_8BIT   0
static inline void *heap_caps_calloc(size_t n, size_t s, int) { return calloc(n, s); }
static inline void *heap_caps_malloc(size_t s, int) { return malloc(s); }
#endif
#include "rix.h"

using namespace std;

#if defined(__hppa__) || \
   defined(__m68k__) || defined(mc68000) || defined(_M_M68K) || \
   (defined(__MIPS__) && defined(__MISPEB__)) || \
   defined(__ppc__) || defined(__POWERPC__) || defined(_M_PPC) || \
   defined(__sparc__)
   // big endian
   #define RIX_SWAP32(a) (((a) << 24) | (((a) << 8) & 0x00FF0000) | (((a) >> 8) & 0x0000FF00) | ((a) >> 24))
   #define RIX_SWAP16(a) ((((a) << 8) & 0xFF00)  | ((a) >> 8))
#else
   // little endian
   #define RIX_SWAP32(a) (a)
   #define RIX_SWAP16(a) (a)
#endif

#ifdef DEBUG
#define RELEASE_INLINE
#else
#define RELEASE_INLINE inline
#endif

const uint8_t CrixPlayer::adflag[] = {0,0,0,1,1,1,0,0,0,1,1,1,0,0,0,1,1,1};
const uint8_t CrixPlayer::reg_data[] = {0,1,2,3,4,5,8,9,10,11,12,13,16,17,18,19,20,21};
const uint8_t CrixPlayer::ad_C0_offs[] = {0,1,2,0,1,2,3,4,5,3,4,5,6,7,8,6,7,8};
const uint8_t CrixPlayer::modify[] = {0,3,1,4,2,5,6,9,7,10,8,11,12,15,13,16,14,17,12,\
					    15,16,0,14,0,17,0,13,0};
const uint8_t CrixPlayer::bd_reg_data[] = {
  0x00,0x00,0x00,0x00,0x00,0x00,0x10,0x08,0x04,0x02,0x01,
  0x00,0x01,0x01,0x03,0x0F,0x05,0x00,0x01,0x03,0x0F,0x00,
  0x00,0x00,0x01,0x00,0x00,0x01,0x01,0x0F,0x07,0x00,0x02,
  0x04,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x0A,
  0x04,0x00,0x08,0x0C,0x0B,0x00,0x00,0x00,0x01,0x00,0x00,
  0x00,0x00,0x0D,0x04,0x00,0x06,0x0F,0x00,0x00,0x00,0x00,
  0x01,0x00,0x00,0x0C,0x00,0x0F,0x0B,0x00,0x08,0x05,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x04,0x00,0x0F,0x0B,0x00,
  0x07,0x05,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x00,
  0x0F,0x0B,0x00,0x05,0x05,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x01,0x00,0x0F,0x0B,0x00,0x07,0x05,0x00,0x00,0x00,
  0x00,0x00,0x00};
uint8_t CrixPlayer::for40reg[] = {0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,
					0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F};
const uint16_t CrixPlayer::mus_time = 0x4268;


/*** public methods *************************************/

CPlayer *CrixPlayer::factory(Copl *newopl)
{
  return new CrixPlayer(newopl);
}

CrixPlayer::CrixPlayer(Copl *newopl)
  : CPlayer(newopl), flag_mkf(0)
#if USE_RIX_EXTRA_INIT
	, extra_regs(NULL), extra_vals(NULL), extra_length(0)
#endif
#ifdef USE_RIX_MKF_FILE_BUFFER
    , file_buffer(0), file_total(0)
#endif
    , rix_buf_owned(true), fp(NULL), rix_buf(0)
{
}

CrixPlayer::~CrixPlayer()
{
#ifdef USE_RIX_MKF_FILE_BUFFER
    /* 内存模式: rix_buf 可能指向 file_buffer 内部, 只有自己 calloc 的才 free */
    if (rix_buf_owned && rix_buf)
        free(rix_buf);
    if (file_buffer)
        free(file_buffer);       /* PSRAM: 用 free 而不是 delete[] */
    if (fp)                      /* load() 成功时已 fclose 并置 NULL */
    {
        fclose(fp);
        fp = NULL;
    }
#else
  fclose(fp);
  if(rix_buf)
    free(rix_buf);
#endif
#if USE_RIX_EXTRA_INIT
  if (extra_regs) delete[] extra_regs;
  if (extra_vals) delete[] extra_vals;
#endif
}

#if USE_RIX_EXTRA_INIT
void CrixPlayer::set_extra_init(uint32_t* regs, uint8_t* datas, int n)
{
	extra_length = n;
	if (extra_regs) delete[] extra_regs;
	if (extra_vals) delete[] extra_vals;

	if (n > 0)
	{
		extra_regs = new uint32_t[n];
		extra_vals = new uint8_t[n];
		if (extra_regs) memcpy(extra_regs, regs, n * sizeof(uint32_t));
		if (extra_vals) memcpy(extra_vals, datas, n * sizeof(uint8_t));
	}
	else
	{
		extra_regs = NULL;
		extra_vals = NULL;
	}
}
#endif

void CrixPlayer::read_file_to(uint8_t *&buf) {
    fseek(fp, 0, SEEK_END);
    length = (uint32_t)ftell(fp);
    if (buf)
        free(buf);
    /* ★ 2026-10-25 [LINTOS] 整个 mus.mkf 搬进 PSRAM。
     *   原来每次换曲都 fseek+fread 从 SD 卡读一整首到**内部 RAM**:
     *     · 游戏运行时内部 RAM 只剩约 148~190KB, 大曲子 calloc 失败 ⇒ 静默静音
     *     · 换曲要读卡 ~40ms, 遇上 SD 忙会被拉长到几百 ms
     *   现在读一次(约 1.5~2MB, 走 PSRAM), 之后换曲只是移动指针, 完全不碰 SD。 */
    buf = (uint8_t*)heap_caps_malloc(length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    bool in_psram = (buf != NULL);
    if (!buf)
        buf = (uint8_t*)calloc(1, length);   /* 兜底: 内部 RAM(小文件才可能成功) */
    if (!buf)
    {
        ESP_LOGE("PALRIX", "mkf 缓冲分配失败: %u 字节", (unsigned)length);
        length = 0;
        return;
    }
    fseek(fp, 0, SEEK_SET);
    size_t got = fread(buf, length, 1, fp);
    ESP_LOGI("PALRIX", "mus.mkf 载入内存: %u B (位置=%s) 读回=%d",
             (unsigned)length, in_psram ? "PSRAM" : "内部RAM", (int)got);
    return;
}

bool CrixPlayer::load(const std::string &filename, const CFileProvider &cfp)
{
  fp = fopen(filename.c_str(),"rb");
  if(!fp) {
      ESP_LOGE("PALRIX", "MUS 打不开(SD卡?): %s", filename.c_str());
      return false;                     /* ★ 2026-10-25: 原来静默失败 ⇒ 全程无声且无日志 */
  }
  ESP_LOGI("PALRIX", "MUS 已打开: %s", filename.c_str());

  if(SDL_strcasecmp(filename.substr(filename.length()-4,4).c_str(),".mkf")==0)
  {
	  flag_mkf=1;
	  fseek(fp,0,SEEK_SET);
      int offset;
      fread(&offset,4,1,fp);
	  fseek(fp,RIX_SWAP32(offset),SEEK_SET);
  }
  uint16_t signature;
  fread(&signature,2,1,fp);
  if(RIX_SWAP16(signature)!=0x55aa){ 
      ESP_LOGE("PALRIX", "MUS 签名不对: 0x%04x (期望 0x55aa)", RIX_SWAP16(signature));
      fclose(fp);return false; }
    if(!flag_mkf)
    {
        read_file_to(rix_buf);
        subsongs = 1;
#ifdef USE_RIX_MKF_FILE_BUFFER
        file_buffer = rix_buf;
        fclose(fp);
#endif
    }
    else
    {
        fseek(fp, 0, SEEK_SET);
        fread(&subsongs, 4, 1, fp);
        subsongs = RIX_SWAP32(subsongs);
        subsongs /= 4;
        ESP_LOGI("PALRIX", "MUS.MKF: 曲目表项=%d", subsongs);
#ifdef USE_RIX_MKF_FILE_BUFFER
        read_file_to(file_buffer);
        file_total = length;
        /* 只有真的读进内存了才关文件; 否则留着 fp 走原来的"逐曲读卡"兜底 */
        if (file_buffer != NULL && file_total > 64)
        {
            fclose(fp);
            fp = NULL;             /* ★ 已关闭: 后面换曲只看内存, 不再碰 SD */
        }
        else
        {
            ESP_LOGW("PALRIX", "mus.mkf 未能进内存(剩余 PSRAM 不足?), 退回逐曲读卡模式");
        }
#endif
    }
  rewind(0);
  return true;
}

bool CrixPlayer::update()
{
	int_08h_entry();
	return !play_end;
}
void CrixPlayer::rewind(int subsong)
{
    rewindReInit(subsong, true);
}
void CrixPlayer::rewindReInit(int subsong, bool reinit)
{
	play_end = 0;
	pos = 0;

	if (reinit)
	{
		music_on = 0;
		I = 0; T = 0;
		mus_block = 0;
		ins_block = 0;
		rhythm = 0;
		pause_flag = 0;
		band = 0;
		band_low = 0;
		e0_reg_flag = 0;
		bd_modify = 0;
		sustain = 0;

		memset(f_buffer, 0, sizeof(f_buffer));
		memset(a0b0_data2, 0, sizeof(a0b0_data2));
		memset(a0b0_data3, 0, sizeof(a0b0_data3));
		memset(a0b0_data4, 0, sizeof(a0b0_data4));
		memset(a0b0_data5, 0, sizeof(a0b0_data5));
		memset(addrs_head, 0, sizeof(addrs_head));
		memset(insbuf, 0, sizeof(insbuf));
		memset(displace, 0, sizeof(displace));
		memset(reg_bufs, 0, sizeof(reg_bufs));
		memset(for40reg, 0x7F, sizeof(for40reg));
	}

	if (flag_mkf)
	{
        int index,index2;
        bool from_mem = false;
#ifdef USE_RIX_MKF_FILE_BUFFER
        /* ★ 2026-10-25 [LINTOS] 内存模式: 整个 mkf 已在 PSRAM, 换曲只改指针。
         *   曲目表就是文件开头的一串 uint32: tab[0] = 数据区起点(≈356),
         *   tab[i] = 第 i 首的起点 ⇒ length = tab[i+1] - tab[i]。
         *   这样"换第二首就静音"(SD 读/内部 RAM 分配失败) 从根上消失。 */
        if (file_buffer != NULL && file_total > 64)
        {
            uint32_t *tab = (uint32_t*)file_buffer;
            int n = subsongs;
            if (n < 2) n = 2;
            int idx = (subsong < 0) ? 0 : (subsong >= n ? n - 1 : subsong);
            uint32_t o1 = RIX_SWAP32(tab[idx]);
            uint32_t o2 = file_total;
            for (int j = idx + 1; j < n; j++)
            {
                uint32_t o = RIX_SWAP32(tab[j]);
                if (o != o1) { o2 = o; break; }
            }
            if (o2 > o1 && o2 <= file_total)
            {
                length = o2 - o1;
                rix_buf = file_buffer + o1;
                rix_buf_owned = false;
                from_mem = true;
            }
            else
            {
                ESP_LOGW("PALRIX", "rewind(内存): subsong=%d o1=%u o2=%u 越界, 退回读卡",
                         subsong, (unsigned)o1, (unsigned)o2);
            }
        }
        if (!from_mem && fp == NULL)
        {
            ESP_LOGE("PALRIX", "rewind: 无内存缓冲且文件已关, subsong=%d 无法换曲", subsong);
            return;
        }
#endif
        if (!from_mem)
        {
        fseek(fp,subsong*4,SEEK_SET);
        fread(&index,4,1,fp);
        fread(&index2,4,1,fp);
        index = RIX_SWAP32(index);
        index2 = RIX_SWAP32(index2);
        length = index2 - index;
        /* ★ 2026-10-25 [LINTOS] "第二首 BGM 没声音"的现场取证:
         *   1) length==0 只可能是 SD 读失败(fseek/fread 没拿到数据) 或曲目表越界
         *   2) 原来用 calloc(内部RAM) —— 游戏运行时内部 RAM 只剩 ~148KB,
         *      大曲子直接分配失败, 而失败没人报 ⇒ 表现为"静音且无日志"
         *   现在: 优先 PSRAM + 全程打日志。 */
        if(length==0) {
            /* subsong=0 是 load() 末尾的预定位, MKF 表首自指 ⇒ 天然为 0, 不是错误。
             * 只有 subsong>0 才是真正的"读不到/越界"。 */
            if(subsong > 0) {
                ESP_LOGW("PALRIX", "rewind: subsong=%d index=%d index2=%d length=0 (SD读失败/越界) buf=%p",
                         subsong, index, index2, (void*)rix_buf);
            }
            return;
        }
        if(length > 4*1024*1024) {
            ESP_LOGE("PALRIX", "rewind: subsong=%d 长度异常 %d (index=%d index2=%d)", subsong, length, index, index2);
            length = 0;
            return;
        }
        fseek(fp,index,SEEK_SET);
        if(rix_buf_owned && rix_buf)   /* ★ 内存模式下的 rix_buf 指向 file_buffer 内部, 绝不能 free */
            free(rix_buf);
        rix_buf = (uint8_t *)heap_caps_calloc(1, length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if(!rix_buf) {
            rix_buf = (uint8_t *)calloc(1, length);      /* 兜底: 内部 RAM */
            ESP_LOGW("PALRIX", "rewind: subsong=%d PSRAM 分配失败, 退内部RAM len=%d ptr=%p",
                     subsong, length, (void*)rix_buf);
        }
        if(!rix_buf) {
            ESP_LOGE("PALRIX", "rewind: subsong=%d 分配 %d 字节彻底失败", subsong, length);
            length = 0;
            return;
        }
        rix_buf_owned = true;
        {
            size_t got = fread(rix_buf,length,1,fp);
            if(got != 1) {
                ESP_LOGE("PALRIX", "rewind: subsong=%d fread 短读 (len=%d got=%d)", subsong, length, (int)got);
                length = 0;
                return;
            }
            ESP_LOGI("PALRIX", "rewind OK: subsong=%d len=%d ptr=%p", subsong, length, (void*)rix_buf);
        }
        }   /* end if (!from_mem) */
	}

	if (reinit)
	{
		opl->init();
		opl->write(1, 32);	// go to OPL2 mode
		set_new_int();
		data_initial();
	}
}

unsigned int CrixPlayer::getsubsongs()
{
    return subsongs;
}

float CrixPlayer::getrefresh()
{
	return 70.0f;
}

/*------------------Implemention----------------------------*/
RELEASE_INLINE void CrixPlayer::set_new_int()
{
//   if(!ad_initial()) exit(1);
  ad_initial();
}
/*----------------------------------------------------------*/
RELEASE_INLINE void CrixPlayer::Pause()
{
  uint16_t i;
  pause_flag = 1;
  for(i=0;i<11;i++)
    switch_ad_bd(i);
}
/*----------------------------------------------------------*/
RELEASE_INLINE void CrixPlayer::ad_a0b0l_reg_(uint16_t index,uint16_t p2,uint16_t p3)
{
//   uint16_t i = p2+a0b0_data2[index];
  a0b0_data4[index] = p3;
  a0b0_data3[index] = p2;
}
RELEASE_INLINE void CrixPlayer::data_initial()
{
  rhythm = rix_buf[2];
  mus_block = (rix_buf[0x0D]<<8)+rix_buf[0x0C];
  ins_block = (rix_buf[0x09]<<8)+rix_buf[0x08];
  I = mus_block+1;
  if(rhythm != 0)
    {
      ad_a0b0_reg(6);
      ad_a0b0_reg(7);
      ad_a0b0_reg(8);
      ad_a0b0l_reg_(8,0x18,0);
      ad_a0b0l_reg_(7,0x1F,0);

	  // This is required for correct attack effect, by louyihua
#if USE_RIX_EXTRA_INIT
	  if (extra_regs && extra_vals && extra_length > 0)
	  {
		  for (uint32_t i = 0; i < extra_length; i++)
			  opl->write(extra_regs[i], extra_vals[i]);
	  }
#else
	  opl->write(0xa8, 87);
	  opl->write(0xb8, 9);
	  opl->write(0xa7, 3);
	  opl->write(0xb7, 15/*10*/);	// Changed from 10 (original value) to 15 for better quality
#endif
  }
  bd_modify = 0;
  ad_bd_reg();
  band = 0; music_on = 1;
}
/*----------------------------------------------------------*/
RELEASE_INLINE uint16_t CrixPlayer::ad_initial()
{
  uint16_t i,j,k = 0;
  for(i=0;i<25;i++) 
  {
		uint32_t res = ((uint32_t)i*24+10000)*52088/250000*0x24000/0x1B503;
		f_buffer[i*12]=((uint16_t)res+4)>>3;
		for(int t=1;t<12;t++)
		{
			res*=1.06;
			f_buffer[i*12+t]=((uint16_t)res+4)>>3;
		}
  }
  for(i=0;i<8;i++)
    for(j=0;j<12;j++)
      {
			a0b0_data5[k] = i;
			addrs_head[k] = j;
			k++;
      }
  ad_bd_reg();
  ad_08_reg();
  for(i=0;i<9;i++) ad_a0b0_reg(i);
  e0_reg_flag = 0x20;
  for(i=0;i<18;i++) ad_bop(0xE0+reg_data[i],0);
  ad_bop(1,e0_reg_flag);
  return 1;//ad_test();
}
/*----------------------------------------------------------*/
RELEASE_INLINE void CrixPlayer::ad_bop(uint16_t reg,uint16_t value)
{
  //if(reg == 2 || reg == 3)
  //  AdPlug_LogWrite("switch OPL2/3 mode!\n");
  opl->write(reg & 0xff, value & 0xff);
}
/*--------------------------------------------------------------*/
RELEASE_INLINE void CrixPlayer::int_08h_entry()
  {   
    uint16_t band_sus = 1;   
    while(band_sus)   
      {   
        if(sustain <= 0)   
          {
            band_sus = rix_proc();   
            if(band_sus) sustain += band_sus; 
            else
              {   
                play_end=1;   
                break;   
              }   
          }   
        else   
          {   
            if(band_sus) sustain -= 14; /* aging */   
            break;   
          }   
      }   
  }   
/*--------------------------------------------------------------*/ 
RELEASE_INLINE uint16_t CrixPlayer::rix_proc()
{
  uint8_t ctrl = 0;
  if(music_on == 0||pause_flag == 1) return 0;
  band = 0;
  while(rix_buf[I] != 0x80 && I<length-1)
    {
      band_low = rix_buf[I-1];
      ctrl = rix_buf[I]; I+=2;
      switch(ctrl&0xF0)
	{
	case 0x90:  rix_get_ins(); rix_90_pro(ctrl&0x0F); break;
	case 0xA0:  rix_A0_pro(ctrl&0x0F,((uint16_t)band_low)<<6); break;
	case 0xB0:  rix_B0_pro(ctrl&0x0F,band_low); break;
	case 0xC0:  switch_ad_bd(ctrl&0x0F);
	  if(band_low != 0) rix_C0_pro(ctrl&0x0F,band_low);
	  break;
	default:    band = (ctrl<<8)+band_low; break;
	}
      if(band != 0) return band;
    }
  music_ctrl();
  I = mus_block+1;
  band = 0; music_on = 1;
  return 0;
}
/*--------------------------------------------------------------*/
RELEASE_INLINE void CrixPlayer::rix_get_ins()
{
  int		i;
  uint8_t	*baddr = (&rix_buf[ins_block])+(band_low<<6);

  for(i = 0; i < 28; i++)
    insbuf[i] = (baddr[i * 2 + 1] << 8) + baddr[i * 2];
}
/*--------------------------------------------------------------*/
RELEASE_INLINE void CrixPlayer::rix_90_pro(uint16_t ctrl_l)
{
  if(rhythm == 0 || ctrl_l < 6)
    {
      ins_to_reg(modify[ctrl_l*2],insbuf,insbuf[26]);
      ins_to_reg(modify[ctrl_l*2+1],insbuf+13,insbuf[27]);
      return;
    }
  else if(ctrl_l > 6)
		{
		  ins_to_reg(modify[ctrl_l*2+6],insbuf,insbuf[26]);
		  return;
		}
  else
		{
		  ins_to_reg(12,insbuf,insbuf[26]);
		  ins_to_reg(15,insbuf+13,insbuf[27]);
		  return;
		}
}
/*--------------------------------------------------------------*/
RELEASE_INLINE void CrixPlayer::rix_A0_pro(uint16_t ctrl_l,uint16_t index)
{
  if(rhythm == 0 || ctrl_l <= 6)
    {
      prepare_a0b0(ctrl_l,index>0x3FFF?0x3FFF:index);
      ad_a0b0l_reg(ctrl_l,a0b0_data3[ctrl_l],a0b0_data4[ctrl_l]);
    }
  else return;
}
/*--------------------------------------------------------------*/
RELEASE_INLINE void CrixPlayer::prepare_a0b0(uint16_t index,uint16_t v)  /* important !*/
{
  short high = 0,low = 0; uint32_t res;
  int res1 = (v-0x2000)*0x19;
  if(res1 == (int)0xff) return;
  low = res1/0x2000;
  if(low < 0)
    {
      low = 0x18-low; high = (signed short)low<0?0xFFFF:0;
      res = high; res<<=16; res+=low;
      low = ((signed short)res)/(signed short)0xFFE7;
      a0b0_data2[index] = low;
      low = res;
      res = low - 0x18;
      high = (signed short)res%0x19;
      low = (signed short)res/0x19;
      if(high != 0) {low = 0x19; low = low-high;}
    }
  else
    {
      res = high = low;
      low = (signed short)res/(signed short)0x19;
      a0b0_data2[index] = low;
      res = high;
      low = (signed short)res%(signed short)0x19;
    }
  low = (signed short)low*(signed short)0x18;
  displace[index] = low;
}
/*--------------------------------------------------------------*/
RELEASE_INLINE void CrixPlayer::ad_a0b0l_reg(uint16_t index,uint16_t p2,uint16_t p3)
{
  uint16_t data; uint16_t i = p2+a0b0_data2[index];
  a0b0_data4[index] = p3;
  a0b0_data3[index] = p2;
  i = ((signed short)i<=0x5F?i:0x5F);
  i = ((signed short)i>=0?i:0);
  data = f_buffer[addrs_head[i]+displace[index]/2];
  ad_bop(0xA0+index,data);
  data = a0b0_data5[i]*4+(p3<1?0:0x20)+((data>>8)&3);
  ad_bop(0xB0+index,data);
}
/*--------------------------------------------------------------*/
RELEASE_INLINE void CrixPlayer::rix_B0_pro(uint16_t ctrl_l,uint16_t index)
{
  int temp = 0;
  if(rhythm == 0 || ctrl_l < 6) temp = modify[ctrl_l*2+1];
  else
    {
      temp = ctrl_l > 6?ctrl_l*2:ctrl_l*2+1;
      temp = modify[temp+6];
    }
  for40reg[temp] = index>0x7F?0x7F:index;
  ad_40_reg(temp);
}
/*--------------------------------------------------------------*/
RELEASE_INLINE void CrixPlayer::rix_C0_pro(uint16_t ctrl_l,uint16_t index)
{
  uint16_t i = index>=12?index-12:0;
  if(ctrl_l < 6 || rhythm == 0)
    {
      ad_a0b0l_reg(ctrl_l,i,1);
      return;
    }
  else
    {
      if(ctrl_l != 6)
	{
	  if(ctrl_l == 8)
	    {
	      ad_a0b0l_reg(ctrl_l,i,0);
	      ad_a0b0l_reg(7,i+7,0);
	    }
	}
      else ad_a0b0l_reg(ctrl_l,i,0);
      bd_modify |= bd_reg_data[ctrl_l];
      ad_bd_reg();
      return;
    }
}
/*--------------------------------------------------------------*/
RELEASE_INLINE void CrixPlayer::switch_ad_bd(uint16_t index)
{

  if(rhythm == 0 || index < 6) ad_a0b0l_reg(index,a0b0_data3[index],0);
  else
    {
      bd_modify &= (~bd_reg_data[index]),
	ad_bd_reg();
    }
}
/*--------------------------------------------------------------*/
RELEASE_INLINE void CrixPlayer::ins_to_reg(uint16_t index,uint16_t* insb,uint16_t value)
{
  uint16_t i;
  for(i=0;i<13;i++) reg_bufs[index].v[i] = insb[i];
  reg_bufs[index].v[13] = value&3;
  ad_bd_reg(),ad_08_reg(),
    ad_40_reg(index),ad_C0_reg(index),ad_60_reg(index),
    ad_80_reg(index),ad_20_reg(index),ad_E0_reg(index);
}
/*--------------------------------------------------------------*/
RELEASE_INLINE void CrixPlayer::ad_E0_reg(uint16_t index)
{
  uint16_t data = e0_reg_flag == 0?0:(reg_bufs[index].v[13]&3);
  ad_bop(0xE0+reg_data[index],data);
}
/*--------------------------------------------------------------*/
RELEASE_INLINE void CrixPlayer::ad_20_reg(uint16_t index)
{
  uint16_t data = (reg_bufs[index].v[9] < 1?0:0x80);
  data += (reg_bufs[index].v[10] < 1?0:0x40);
  data += (reg_bufs[index].v[5] < 1?0:0x20);
  data += (reg_bufs[index].v[11] < 1?0:0x10);
  data += (reg_bufs[index].v[1]&0x0F);
  ad_bop(0x20+reg_data[index],data);
}
/*--------------------------------------------------------------*/
RELEASE_INLINE void CrixPlayer::ad_80_reg(uint16_t index)
{
  uint16_t data = (reg_bufs[index].v[7]&0x0F),temp = reg_bufs[index].v[4];
  data |= (temp << 4);
  ad_bop(0x80+reg_data[index],data);
}
/*--------------------------------------------------------------*/
void CrixPlayer::ad_60_reg(uint16_t index)
{
  uint16_t data = reg_bufs[index].v[6]&0x0F,temp = reg_bufs[index].v[3];
  data |= (temp << 4);
  ad_bop(0x60+reg_data[index],data);
}
/*--------------------------------------------------------------*/
RELEASE_INLINE void CrixPlayer::ad_C0_reg(uint16_t index)
{
  uint16_t data = reg_bufs[index].v[2];
  if(adflag[index] == 1) return;
  data *= 2,
    data |= (reg_bufs[index].v[12] < 1?1:0);
  ad_bop(0xC0+ad_C0_offs[index],data);
}
/*--------------------------------------------------------------*/
RELEASE_INLINE void CrixPlayer::ad_40_reg(uint16_t index)
{
  uint32_t res = 0;
  uint16_t data = 0,temp = reg_bufs[index].v[0];
  data = 0x3F - (0x3F & reg_bufs[index].v[8]),
    data *= for40reg[index],
    data *= 2,
    data += 0x7F,
    res = data;
  data = res/0xFE,
    data -= 0x3F,
    data = -data,
    data |= temp<<6;
  ad_bop(0x40+reg_data[index],data);
}
/*--------------------------------------------------------------*/
RELEASE_INLINE void CrixPlayer::ad_bd_reg()
{
  uint16_t data = rhythm < 1? 0:0x20;
  data |= bd_modify;
  ad_bop(0xBD,data);
}
/*--------------------------------------------------------------*/
RELEASE_INLINE void CrixPlayer::ad_a0b0_reg(uint16_t index)
{
  ad_bop(0xA0+index,0);
  ad_bop(0xB0+index,0);
}
/*--------------------------------------------------------------*/
RELEASE_INLINE void CrixPlayer::music_ctrl()
{
  int i;
  for(i=0;i<11;i++)
    switch_ad_bd(i);
}
