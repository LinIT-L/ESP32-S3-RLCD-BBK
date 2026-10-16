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
** Any permitted reproduction of these routines, in whole or in part,
** must bear this legend.
**
**
** map164.c
**
** mapper 164 interface (Henggedianzi / Waixing, used by Chinese bootleg games)
** $Id: map164.c $
*/

#include <noftypes.h>
#include <nes_mmc.h>

/* Mapper 164: Simple PRG bank switching.
 * - Write to $8000-$FFFF:
 *   bits 0-4: select 16KB PRG bank at $8000 (5 bits = 32 banks, 512KB max)
 *   $C000 fixed to last bank (for reset vector)
 * - CHR: 8KB VRAM (no CHR ROM)
 */

static void map164_write(uint32 address, uint8 value)
{
   UNUSED(address);

   /* bits 0-4: 16KB PRG bank at $8000 (32 banks for 512KB) */
   mmc_bankrom(16, 0x8000, value & 0x1F);

   /* $C000 fixed to last bank (calculated from rom_banks) */
   mmc_bankrom(16, 0xC000, MMC_LASTBANK);
}

static map_memwrite map164_memwrite[] =
{
   { 0x8000, 0xFFFF, map164_write },
   {     -1,     -1, NULL }
};

mapintf_t map164_intf =
{
   164, /* mapper number */
   "Henggedianzi", /* mapper name */
   NULL, /* init routine */
   NULL, /* vblank callback */
   NULL, /* hblank callback */
   NULL, /* get state (snss) */
   NULL, /* set state (snss) */
   NULL, /* memory read structure */
   map164_memwrite, /* memory write structure */
   NULL /* external sound device */
};