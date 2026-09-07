/*
   Copyright 2026      Leil Storage OÜ

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

#include <cstdint>

#include "common/serialization_macros.h"

/// When and over what a cluster-wide chunk health answer was measured.
///
/// A backend that keeps chunk state outside the process answers from a scan of its records
/// rather than from what it currently holds in memory, so the answer is complete but older than
/// the request. These fields travel with the counters, because "nothing is lost, measured a
/// minute ago" and "nothing has been measured yet" are different answers and a monitor has to
/// be able to tell them apart. A backend that computes the counters as it goes reports no
/// freshness at all.
struct ChunkHealthFreshness {
	/// Scan that produced the counters. Zero means no scan has completed yet, so the counters
	/// are all zero because nothing has been measured, not because nothing is wrong.
	uint64_t generation = 0;

	/// Unix timestamps bounding the scan. The distance between them is how long the scan took.
	uint32_t scanStart = 0;
	uint32_t scanEnd = 0;

	/// Chunk records the scan read, and how many of those it could not classify and left out
	/// of the counters (a record being written while the scan passed it, for instance).
	uint64_t chunksScanned = 0;
	uint64_t chunksExcluded = 0;

	/// Chunkservers the scan counted as unreachable. Their recorded copies count as absent,
	/// exactly as they do for a server that has been disconnected, so a nonzero value here is
	/// the difference between a report that reads as data loss and one that reads as servers
	/// being down.
	uint32_t chunkserversDown = 0;

	bool measured() const { return generation != 0; }

	SAUNAFS_DEFINE_SERIALIZE_METHODS(generation, scanStart, scanEnd, chunksScanned, chunksExcluded,
	                                 chunkserversDown);
};
