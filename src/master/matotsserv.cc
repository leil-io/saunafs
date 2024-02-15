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

#include "common/platform.h"


#include <list>

#include "common/event_loop.h"
#include "common/media_label.h"
#include "common/network_address.h"
#include "common/output_packet.h"
#include "common/sockets.h"
#include "common/time_utils.h"
#include "protocol/input_packet.h"

/// Maximum allowed length of a network packet
static constexpr uint32_t kMaxPacketSize = 500000000;

