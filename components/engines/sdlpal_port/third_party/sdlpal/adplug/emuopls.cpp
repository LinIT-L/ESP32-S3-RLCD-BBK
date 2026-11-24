/*
 * Adplug - Replayer for many OPL2/OPL3 audio file formats.
 * Copyright (C) 1999 - 2005 Simon Peter, <dn.tlp@gmx.net>, et al.
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
 * -------------------------------------------------------------------------
 * SDLPAL
 * Copyright (c) 2011-2024, SDLPAL development team.
 * All rights reserved.
 *
 * This file is part of SDLPAL.
 *
 * SDLPAL is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 * -------------------------------------------------------------------------
 *
 * wrappers.cpp - Wrapper of several OPL2 emulators by Lou Yihua.
 *
 */

/* LINTOS: 只用 emu8950 (MIT, 轻量)。dosbox/mame/nuked 三个重核不再编译,
 * 它们在本机实测跑不到实时 (见 docs/架构与工程约定.md §9 R4)。 */
#include "emu8950_opl.h"

Copl* CEmuopl::CreateEmuopl(OPLCORE::TYPE core, Copl::ChipType type, int rate) {

	if (type != Copl::TYPE_OPL2 && type != Copl::TYPE_DUAL_OPL2 && type != Copl::TYPE_OPL3) {
		return NULL;
	}

	switch (core) {
	case OPLCORE::EMU8950:
		/* 只有单芯片 OPL2 能力: DUAL_OPL2 需要两份独立实例 (CEmuopl 会 Duplicate) */
		return new CEmuopl(new EMU8950OPL2(rate), type == Copl::TYPE_DUAL_OPL2 ? Copl::TYPE_DUAL_OPL2 : Copl::TYPE_OPL2);
	default:
		return NULL;
	}
}
