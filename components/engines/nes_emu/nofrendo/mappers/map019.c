/*
** Nofrendo (c) 1998-2000 Matthew Conte (matt@conte.com)
**
**
** This program is free software; you can redistribute it and/or
** modify it under the terms of version 2 of the GNU Library General 
** Public License as published by the Free Software Foundation.
**
** This program is distributed in the hope that it will be useful, 
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU 
** Library General Public License for more details.  To obtain a 
** copy of the GNU Library General Public License, write to the Free 
** Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
**
**
** map019.c — mapper 19 (Namcot 163 / N-105) 完整支持
**
** 移植自 FCEUmm boards/n106.c (NamcoIRQHook / DoNTARAMROM / Mapper19_write).
** 原 nofrendo map019 仅做了 banking, IRQ(hblank=NULL) 与 nametable 镜像缺失,
** 导致《三国志2 霸王的大陆》首帧写 $2007(nametable) 时 page[] 指向野指针崩溃.
**
** 本项目在 nofrendo 里补全:
**   - CHR 1KB 分页 (pattern 8 窗) + 4 nametable 镜像 (可指向 CHR 或内部 RAM)
**   - IRQ: CPU 周期累加计数, >=0x7FFF 触发 (nofrendo 用 hblank 每扫描线加 NES_SCANLINE_CYCLES 近似)
**   - IRAM($4800) 读写 + IRQ 计数读 ($5000/$5800)
**   - WRAM($6000-$7FFF) 8KB
**   - NAMCO 163 音频暂缓 (画面/操作优先, 音效后续)
*/
#include <noftypes.h>
#include <nes_mmc.h>
#include <nes_ppu.h>
#include <nes.h>

/* Special mirroring macro for mapper 19:
 *  nametable <0xE0 指向大 CHR (可写 vrom); >=0xE0 指向内部 nametab RAM.
 *  (原实现用 mmc_getinfo()->vram, 在无 VRAM 卡(N106 用 CHR-RAM)上为 NULL 导致野指针) */
#define N_BANK1(table, value) \
{ \
   if ((value) < 0xE0) \
      ppu_setpage(1, (table) + 8, &mmc_getinfo()->vrom[((value) % (mmc_getinfo()->vrom_banks * 8)) << 10] - (0x2000 + ((table) << 10))); \
   else \
      ppu_setpage(1, (table) + 8, &nes_getcontextptr()->ppu->nametab[((value) & 3) << 10] - (0x2000 + ((table) << 10))); \
   ppu_mirrorhipages(); \
}

static struct
{
   uint16_t count, latch;
   int enabled;
} irq;

static uint8_t s_iram[128];          /* $4800 波形/Ram (N163 内部 128B) */
static uint8_t s_dopol;              /* IRAM 读写指针 */

/* nofrendo 无周期钩子, 用 hblank 每扫描线累加 NES_SCANLINE_CYCLES(≈113) 近似.
 * 对齐 FCEUmm NamcoIRQHook: IRQCount += a; >=0x7FFF 触发并禁用. */
static void map19_hblank(int vblank)
{
   (void)vblank;
   if (!irq.enabled) return;
   irq.count += 113;   /* NES_SCANLINE_CYCLES 近似 */
   if (irq.count >= 0x7FFF)
   {
      nes_irq();
      irq.enabled = 0;
      irq.count = 0x7FFF;
   }
}

/* $5000: IRQ 低 8 位 + 清零 IRQ */
static uint8 map19_read_5000(uint32 address) { (void)address; return (uint8)(irq.count & 0xFF); }
/* $5800: IRQ 高 7 位 (bit7 读回 0) */
static uint8 map19_read_5800(uint32 address) { (void)address; return (uint8)((irq.count >> 8) & 0x7F); }
/* $4800: IRAM 读, dopol 自增 */
static uint8 map19_read_4800(uint32 address)
{
   (void)address;
   uint8_t ret = s_iram[s_dopol & 0x7F];
   if (s_dopol & 0x80)
      s_dopol = (s_dopol & 0x80) | ((s_dopol + 1) & 0x7F);
   return ret;
}

static void map19_init(void)
{
   irq.count = irq.latch = 0;
   irq.enabled = 0;
   s_dopol = 0;
}

/* mapper 19: Namcot 106 */
static void map19_write(uint32 address, uint8 value)
{
   int reg = address >> 11;
   switch (reg)
   {
   case 0x9:   /* $4800: IRAM 写 */
      s_iram[s_dopol & 0x7F] = value;
      if (s_dopol & 0x80)
         s_dopol = (s_dopol & 0x80) | ((s_dopol + 1) & 0x7F);
      break;

   case 0xA:   /* $5000: 计数器低 8 位, 清 IRQ */
      irq.latch &= 0xFF00; irq.latch |= value;
      irq.count = irq.latch;
      break;

   case 0xB:   /* $5800: 高 7 位 + bit7 使能 */
      irq.latch = (irq.latch & 0x00FF) | ((uint16_t)(value & 0x7F) << 8);
      irq.enabled = (value & 0x80) ? 1 : 0;
      irq.count = irq.latch;
      break;

   case 0x10:
   case 0x11:
   case 0x12:
   case 0x13:
   case 0x14:
   case 0x15:
   case 0x16:
   case 0x17:
      /* PPU pattern 表: 1KB 窗口 -> 大 CHR 任意 1KB */
      mmc_bankvrom(1, (reg & 7) << 10, value);
      break;

   case 0x18:
   case 0x19:
   case 0x1A:
   case 0x1B:
      /* Nametable: 指向 CHR(<0xE0) 或内部 RAM(>=0xE0) */
      N_BANK1(reg & 3, value);
      break;

   case 0x1C:
   case 0x1D:
   case 0x1E:
      /* PRG: 8KB 窗 *3 */
      mmc_bankrom(8, 0x8000 + ((reg - 0x1C) << 13), value);
      break;

   case 0x1F:   /* $F800: IRAM 指针 */
      s_dopol = value;
      break;

   default:
      break;
   }
}

static map_memwrite map19_memwrite[] =
{
   { 0x4800, 0x4FFF, map19_write },
   { 0x5000, 0x5FFF, map19_write },
   { 0x8000, 0xFFFF, map19_write },
   {     -1,     -1, NULL }
};

static map_memread map19_memread[] =
{
   { 0x4800, 0x4FFF, map19_read_4800 },
   { 0x5000, 0x57FF, map19_read_5000 },
   { 0x5800, 0x5FFF, map19_read_5800 },
   {     -1,     -1, NULL }
};

mapintf_t map19_intf =
{
   19, /* mapper number */
   "Namcot 106", /* mapper name */
   map19_init, /* init routine */
   NULL, /* vblank callback */
   map19_hblank, /* hblank callback */
   NULL, /* get state (snss) */
   NULL, /* set state (snss) */
   map19_memread, /* memory read structure */
   map19_memwrite, /* memory write structure */
   NULL /* external sound device */
};