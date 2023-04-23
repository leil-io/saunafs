/*
   Copyright 2013-2014 EditShare
   Copyright 2013-2015 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ

   This file is part of SaunaFS.

   SaunaFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   SaunaFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with SaunaFS. If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "common/platform.h"

#include "protocol/packet.h"
#include "common/serialization_macros.h"

// SAU_MATOTS_REGISTER_TAPESERVER
SAUNAFS_DEFINE_PACKET_VERSION(matots, registerTapeserver, kStatusPacketVersion, 0)
SAUNAFS_DEFINE_PACKET_VERSION(matots, registerTapeserver, kResponsePacketVersion, 1)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matots, registerTapeserver, SAU_MATOTS_REGISTER_TAPESERVER, kStatusPacketVersion,
		uint8_t, status)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matots, registerTapeserver, SAU_MATOTS_REGISTER_TAPESERVER, kResponsePacketVersion,
		uint32_t, version)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matots, putFiles, SAU_MATOTS_PUT_FILES, 0,
		std::vector<TapeKey>, tapeContents)
