/*
   Copyright 2013-2016 Skytechnology sp. z o.o.
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

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "common/chunk_connector.h"
#include "common/chunk_read_planner.h"
#include "common/chunk_type_with_address.h"
#include "common/connection_pool.h"
#include "common/massert.h"
#include "common/network_address.h"
#include "common/read_plan_executor.h"
#include "common/time_utils.h"
#include "mount/chunk_locator.h"

class ChunkReader {
public:
	ChunkReader(ChunkConnector& connector, ReadChunkLocator& _locator, double bandwidth_overuse);

	/**
	 * Uses a locator to locate the chunk and chooses chunkservers to read from.
	 * Doesn't do anything if the chunk given by (inode, index) is already known to the reader
	 * (ie. the last call to this method had the same inode and index) unless forcePrepare is true.
	 */
	void prepareReadingChunk(uint32_t inode, uint32_t index, bool forcePrepare);

	/**
	 * Reads data from the previously located chunk and appends it to the buffer
	 */
	uint32_t readData(std::vector<uint8_t>& buffer, uint32_t offset, uint32_t size,
			uint32_t connectTimeout_ms, uint32_t wave_timeout_ms,
			const Timeout& communicationTimeout, bool prefetchXorStripes);

	bool isChunkLocated() const {
		return (bool)location_;
	}
	uint32_t inode() const {
		return inode_;
	}
	uint32_t index() const {
		return index_;
	}
	uint64_t chunkId() const {
		return location_->chunkId;
	}
	uint32_t version() const {
		return location_->version;
	}

	/// Counter for the .saunafs_tweaks file.
	static std::atomic<uint64_t> preparations;

private:
	ChunkConnector& connector_;
	ReadChunkLocator *locator_;
	uint32_t inode_;
	uint32_t index_;
	std::shared_ptr<const ChunkLocationInfo> location_;
	ChunkReadPlanner planner_;
	ReadPlan::PartsContainer available_parts_;
	ReadPlanExecutor::ChunkTypeLocations chunk_type_locations_;
	std::vector<ChunkTypeWithAddress> crcErrors_;
	bool chunkAlreadyRead;
};
