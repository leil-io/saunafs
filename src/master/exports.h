/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA
   Copyright 2013-2014 EditShare
   Copyright 2013-2019 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ


   SaunaFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   SaunaFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with SaunaFS  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "common/platform.h"

#include <cstdint>

uint32_t exports_info_size(uint8_t versmode);
void exports_info_data(uint8_t versmode, uint8_t *buff);
uint8_t exports_check(uint32_t ip, uint32_t version, uint8_t meta,
		const uint8_t *path, const uint8_t rndcode[32],
		const uint8_t passcode[16], uint8_t *sesflags,
		uint32_t *rootuid, uint32_t *rootgid, uint32_t *mapalluid,
		uint32_t *mapallgid, uint8_t *mingoal, uint8_t *maxgoal,
		uint32_t *mintrashtime, uint32_t
		*maxtrashtime);
int exports_init(void);
