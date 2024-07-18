/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA
   Copyright 2013-2014 EditShare
   Copyright 2013-2015 Skytechnology sp. z o.o.
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

#include "common/platform.h"

#include "hddspacemgr.h"

#ifdef SAUNAFS_HAVE_FALLOC_FL_PUNCH_HOLE_IN_LINUX_FALLOC_H
#  define SAUNAFS_HAVE_FALLOC_FL_PUNCH_HOLE
#endif

#if defined(SAUNAFS_HAVE_FALLOCATE)
#if defined(SAUNAFS_HAVE_FALLOC_FL_PUNCH_HOLE) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#endif

#include <dirent.h>
#include <fcntl.h>
#include <cerrno>
#ifdef SAUNAFS_HAVE_FALLOC_FL_PUNCH_HOLE_IN_LINUX_FALLOC_H
#  include <linux/falloc.h>
#endif
#include <cinttypes>
#include <cmath>
#ifndef SAUNAFS_HAVE_THREAD_LOCAL
#include <pthread.h>
#endif // SAUNAFS_HAVE_THREAD_LOCAL
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#ifdef SAUNAFS_HAVE_THREAD_LOCAL
#include <array>
#endif // SAUNAFS_HAVE_THREAD_LOCAL
#include <atomic>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "chunkserver-common/chunk_interface.h"
#include "chunkserver-common/chunk_with_fd.h"
#include "chunkserver-common/cmr_disk.h"
#include "chunkserver-common/default_disk_manager.h"
#include "chunkserver-common/global_shared_resources.h"
#include "chunkserver-common/hdd_stats.h"
#include "chunkserver-common/hdd_utils.h"
#include "chunkserver-common/iostat.h"
#include "chunkserver-common/plugin_manager.h"
#include "chunkserver-common/subfolder.h"
#include "chunkserver/chartsdata.h"
#include "chunkserver/chunk_filename_parser.h"
#include "common/cfg.h"
#include "common/chunk_version_with_todel_flag.h"
#include "common/crc.h"
#include "common/datapack.h"
#include "common/disk_info.h"
#include "common/event_loop.h"
#include "common/exceptions.h"
#include "common/massert.h"
#include "common/legacy_vector.h"
#include "errors/saunafs_error_codes.h"
#include "common/serialization.h"
#include "common/slice_traits.h"
#include "slogger/slogger.h"
#include "common/time_utils.h"
#include "common/unique_queue.h"
#include "devtools/TracePrinter.h"
#include "devtools/request_log.h"
#include "protocol/SFSCommunication.h"

constexpr int kErrorLimit = 2;
constexpr int kLastErrorTime = 60;

static std::atomic<unsigned> gHDDTestFreq_ms(10 * 1000);

inline std::atomic_bool gCheckCrcWhenReading{true};

/// Value of HDD_ADVISE_NO_CACHE from config
static std::atomic_bool gAdviseNoCache;

void hddGetDamagedChunks(std::vector<ChunkWithType>& chunks,
                         std::size_t limit) {
	TRACETHIS();
	std::lock_guard lockGuard(gMasterReportsLock);
	std::size_t size = std::min(gDamagedChunks.size(), limit);
	chunks.assign(gDamagedChunks.begin(), gDamagedChunks.begin() + size);
	gDamagedChunks.erase(gDamagedChunks.begin(), gDamagedChunks.begin() + size);
}

void hddReportLostChunk(uint64_t chunkid, ChunkPartType chunk_type) {
	TRACETHIS1(chunkid);
	std::lock_guard lockGuard(gMasterReportsLock);
	gLostChunks.push_back({chunkid, chunk_type});
}

void hddGetLostChunks(std::vector<ChunkWithType> &chunks, std::size_t limit) {
	TRACETHIS();
	std::lock_guard lockGuard(gMasterReportsLock);
	std::size_t size = std::min(gLostChunks.size(), limit);
	chunks.assign(gLostChunks.begin(), gLostChunks.begin() + size);
	gLostChunks.erase(gLostChunks.begin(), gLostChunks.begin() + size);
}

void hddReportNewChunkToMaster(uint64_t id, uint32_t version, bool todel,
                               ChunkPartType type) {
	TRACETHIS();
	uint32_t versionWithTodelFlag =
	    common::combineVersionWithTodelFlag(version, todel);
	std::lock_guard lockGuard(gMasterReportsLock);
	gNewChunks.push_back(
	    ChunkWithVersionAndType(id, versionWithTodelFlag, type));
}

void hddGetNewChunks(std::vector<ChunkWithVersionAndType> &chunks,
                     std::size_t limit) {
	TRACETHIS();
	std::lock_guard lockGuard(gMasterReportsLock);
	std::size_t size = std::min(gNewChunks.size(), limit);
	chunks.assign(gNewChunks.begin(), gNewChunks.begin() + size);
	gNewChunks.erase(gNewChunks.begin(), gNewChunks.begin() + size);
}

uint32_t hddGetAndResetErrorCounter() {
	TRACETHIS();
	return gErrorCounter.exchange(0);
}

int hddGetAndResetSpaceChanged() {
	TRACETHIS();
	return gHddSpaceChanged.exchange(false);
}

uint32_t hddGetSerializedSizeOfAllDiskInfosV2() {
	TRACETHIS();
	uint32_t serializedSizeOfAllDisks = 0;
	static constexpr uint32_t kMaxDiskInfoSerializedSizeWithoutPath = (2 + 226);

	gDisksMutex.lock();  // Will be unlocked by hddSerializeAllDiskInfosV2

	for (const auto& disk : gDisks) {
		serializedSizeOfAllDisks += kMaxDiskInfoSerializedSizeWithoutPath
		                            + std::min(disk->dataPath().size(), 255UL);
	}

	return serializedSizeOfAllDisks;
}

void hddSerializeAllDiskInfosV2(uint8_t *buff) {
	TRACETHIS();

	if (buff) {
		LegacyVector<DiskInfo> diskInfoVector;

		for (const auto& disk : gDisks) {
			diskInfoVector.emplace_back(disk->toDiskInfo());
		}

		serialize(&buff, diskInfoVector);
	}

	gDisksMutex.unlock();  //Locked by hddGetSerializedSizeOfAllDiskInfosV2
}

void hddDiskInfoRotateStats() {
	TRACETHIS();

	std::lock_guard diskLockGuard(gDisksMutex);

	for (auto &disk : gDisks) {
		auto &diskStats = disk->getCurrentStats();
		if (disk->statsPos() == 0) {
			disk->setStatsPos(disk::kStatsHistoryIn24Hours - 1);
		} else {
			disk->setStatsPos(disk->statsPos() - 1);
		}
		disk->stats()[disk->statsPos()] = diskStats;
		diskStats.clear();
	}
}

static IChunk *hddChunkCreate(IDisk *disk, uint64_t chunkId,
                              ChunkPartType chunkType, uint32_t version) {
	TRACETHIS();

	auto *chunk = hddChunkFindOrCreatePlusLock(disk, chunkId, chunkType,
	                                           disk::ChunkGetMode::kCreateOnly);
	if (chunk == ChunkNotFound) {
		return ChunkNotFound;
	}

	chunk->setVersion(version);
	disk->setNeedRefresh(true);
	chunk->updateFilenamesFromVersion(version);

	std::lock_guard testsLockGuard(gTestsMutex);
	disk->chunks().insert(chunk);

	return chunk;
}

void hddSendDataToMaster(IDisk *disk, bool isForRemoval) {
	TRACETHIS();
	bool markedForDeletion = disk->isMarkedForDeletion();

	std::scoped_lock lock(gChunksMapMutex, gTestsMutex);

	// Until C++14 the order of the elements that are not erased is not
	// guaranteed to be preserved in std::unordered_map. Thus, to be truly
	// portable, all elements to be removed from gChunksMap are first stored
	// in an auxiliary container and then each is erased from gChunksMap
	// outside the loop.
	std::vector<IChunk *> chunksToRemove;

	if (isForRemoval) {
		chunksToRemove.reserve(disk->chunks().size());
	}

	for (const auto &chunkEntry : gChunksMap) {
		IChunk *chunk = chunkEntry.second.get();

		if (chunk->owner() == disk) {
			if (isForRemoval) {
				chunksToRemove.push_back(chunk);
			} else {
				hddReportNewChunkToMaster(chunk->id(), chunk->version(),
				                          markedForDeletion, chunk->type());
			}
		}
	}

	for (auto chunk : chunksToRemove) {
		hddReportLostChunk(chunk->id(), chunk->type());

		if (chunk->state() == ChunkState::Available) {
			gOpenChunks.purge(chunk->metaFD());
			chunk->owner()->chunks().remove(chunk);
			gChunksMap.erase(chunkToKey(*chunk));
		} else if (chunk->state() == ChunkState::Locked) {
			chunk->setState(ChunkState::ToBeDeleted);
		}
	}
}

/// Assigned to each Disk
void hddDiskScanThread(IDisk *disk);

// Run every second
void hddCheckDisks() {
	TRACETHIS();
	uint32_t i;
	uint32_t now;
	int changed, err;
	struct timeval tv;

	gettimeofday(&tv, NULL);
	now = tv.tv_sec;

	changed = 0;

	std::unique_lock disksUniqueLock(gDisksMutex);

	if (gDiskActions == 0) {
		return;
	}

	std::vector<IDisk *> disksToRemove;

	for (auto &disk : gDisks) {
		if (disk->wasRemovedFromConfig()) {
			switch (disk->scanState()) {
			case IDisk::ScanState::kInProgress:
				disk->setScanState(IDisk::ScanState::kTerminate);
				break;
			case IDisk::ScanState::kThreadFinished:
				disk->scanThread().join();
				/* fallthrough */
			case IDisk::ScanState::kSendNeeded:
			case IDisk::ScanState::kNeeded:
				disk->setScanState(IDisk::ScanState::kWorking);
				/* fallthrough */
			case IDisk::ScanState::kWorking:
				hddSendDataToMaster(disk.get(), true);
				changed = 1;
				disk->setWasRemovedFromConfig(false);
				break;
			case IDisk::ScanState::kTerminate:
				break;
			}
			// At this point, this is only true if it was already sent to master
			if (!disk->wasRemovedFromConfig()) {
				// Delay the deletion after the loop
				safs_pretty_syslog(LOG_NOTICE, "Disk %s successfully removed",
				                   disk->getPaths().c_str());

				disksToRemove.push_back(disk.get());
				gResetTester = true;
			}
		}
	}

	for (const auto &diskToDel : disksToRemove) {
		for (auto it = gDisks.begin(); it != gDisks.end(); ++it) {
			if (diskToDel == it->get()) {
				gDisks.erase(it);
				break;
			}
		}
	}

	disksToRemove.clear();

	for (auto &disk : gDisks) {
		if (disk->isDamaged() || disk->wasRemovedFromConfig()) {
			continue;
		}
		switch (disk->scanState()) {
		case IDisk::ScanState::kNeeded:
			disk->setScanState(IDisk::ScanState::kInProgress);
			disk->setScanThread(std::thread(hddDiskScanThread, disk.get()));
			break;
		case IDisk::ScanState::kThreadFinished:
			disk->scanThread().join();
			disk->setScanState(IDisk::ScanState::kWorking);
			disk->refreshDataDiskUsage();
			disk->setNeedRefresh(false);
			disk->setLastRefresh(now);
			changed = 1;
			break;
		case IDisk::ScanState::kSendNeeded:
			hddSendDataToMaster(disk.get(), false);
			disk->setScanState(IDisk::ScanState::kWorking);
			disk->refreshDataDiskUsage();
			disk->setNeedRefresh(false);
			disk->setLastRefresh(now);
			changed = 1;
			break;
		case IDisk::ScanState::kWorking:
			err = 0;
			for (i = 0; i < disk::kLastErrorSize; ++i) {
				if (disk->lastErrorTab()[i].timestamp + kLastErrorTime >= now
				    && (disk->lastErrorTab()[i].errornumber == EIO
				        || disk->lastErrorTab()[i].errornumber == EROFS)) {
					err++;
				}
			}
			if (err >= kErrorLimit &&
			    !(disk->isMarkedForRemoval() && disk->isReadOnly())) {
				safs_pretty_syslog(
				    LOG_WARNING, "%u errors occurred in %u seconds on disk: %s",
				    err, kLastErrorTime, disk->getPaths().c_str());
				hddSendDataToMaster(disk.get(), true);
				disk->setIsDamaged(true);
				changed = 1;
			} else {
				if (disk->needRefresh() ||
				    disk->lastRefresh() + disk::kSecondsInOneMinute < now) {
					disk->refreshDataDiskUsage();
					disk->setNeedRefresh(false);
					disk->setLastRefresh(now);
					changed = 1;
				}
			}
		case IDisk::ScanState::kInProgress:
		case IDisk::ScanState::kTerminate:
			break;
		}
	}

	gDiskManager->updateSpaceUsage();

	disksUniqueLock.unlock();

	if (changed) {
		gHddSpaceChanged = true;
	}
}

void hddForeachChunkInBulks(BulkFunction bulkCallback, std::size_t bulkSize) {
	TRACETHIS();

	std::vector<ChunkWithVersionAndType> bulk;
	std::vector<ChunkWithType> recheckList;
	bulk.reserve(bulkSize);

	enum class BulkReadyWhen { FULL, NONEMPTY };
	auto handleBulkIfReady = [&bulk, &bulkCallback,
	                          bulkSize](BulkReadyWhen whatIsReady) {
		if ((whatIsReady == BulkReadyWhen::FULL && bulk.size() >= bulkSize) ||
		    (whatIsReady == BulkReadyWhen::NONEMPTY && !bulk.empty())) {
			bulkCallback(bulk);
			bulk.clear();
		}
	};
	auto addChunkToBulk = [&bulk](const IChunk *chunk) {
		common::chunk_version_t versionWithTodelFlag =
		    common::combineVersionWithTodelFlag(
		    chunk->version(), chunk->owner()->isMarkedForDeletion());
		bulk.push_back(ChunkWithVersionAndType(
		    chunk->id(), versionWithTodelFlag, chunk->type()));
	};

	{
		// do the operation for all immediately available (not-locked) chunks
		// add all other chunks to recheckList
		std::lock_guard chunksMapLockGuard(gChunksMapMutex);

		for (const auto &chunkEntry : gChunksMap) {
			const IChunk *chunk = chunkEntry.second.get();
			if (chunk->state() != ChunkState::Available) {
				recheckList.push_back(
				    ChunkWithType(chunk->id(), chunk->type()));
				continue;
			}
			handleBulkIfReady(BulkReadyWhen::FULL);
			addChunkToBulk(chunk);
		}
		handleBulkIfReady(BulkReadyWhen::NONEMPTY);
	}

	// wait till each chunk from recheckList becomes available,
	// lock (acquire) it and then do the operation
	for (const auto &chunkWithType : recheckList) {
		handleBulkIfReady(BulkReadyWhen::FULL);
		auto *chunk =
		    hddChunkFindAndLock(chunkWithType.id, chunkWithType.type);
		if (chunk) {
			addChunkToBulk(chunk);
			hddChunkRelease(chunk);
		}
	}
	handleBulkIfReady(BulkReadyWhen::NONEMPTY);
}

void hddGetTotalSpace(uint64_t *usedSpace, uint64_t *totalSpace,
                      uint32_t *chunkCount, uint64_t *toDelUsedSpace,
                      uint64_t *toDelTotalSpace, uint32_t *toDelChunkCount) {
	TRACETHIS();
	uint64_t available = 0, total = 0;
	uint64_t toDelAvailable = 0, toDelTotal = 0;
	uint32_t chunks = 0, toDelChunks = 0;

	{
		std::lock_guard disksLockGuard(gDisksMutex);

		for (const auto &disk : gDisks) {
			if (disk->isDamaged() || disk->wasRemovedFromConfig()) {
				continue;
			}
			if (!disk->isMarkedForDeletion()) {
				if (disk->scanState() == IDisk::ScanState::kWorking) {
					available += disk->availableSpace();
					total += disk->totalSpace();
				}

				chunks += disk->chunks().size();
			} else {
				if (disk->scanState() == IDisk::ScanState::kWorking) {
					toDelAvailable += disk->availableSpace();
					toDelTotal += disk->totalSpace();
				}

				toDelChunks += disk->chunks().size();
			}
		}
	}

	*usedSpace = total - available;
	*totalSpace = total;
	*chunkCount = chunks;
	*toDelUsedSpace = toDelTotal - toDelAvailable;
	*toDelTotalSpace = toDelTotal;
	*toDelChunkCount = toDelChunks;
}

int hddGetLoadFactor() {
	return gIoStat.getLoadFactor();
}

/* I/O operations */
int hddOpen(IChunk *chunk) {
	assert(chunk);
	LOG_AVG_TILL_END_OF_SCOPE0("hddOpen");
	TRACETHIS1(chunk->id());

	int status = hddIOBegin(chunk, 0);
	PRINTTHIS(status);
	if (status != SAUNAFS_STATUS_OK) {
		hddAddErrorAndPreserveErrno(chunk);
		hddReportDamagedChunk(chunk->id(), chunk->type());
	}

	return status;
}

int hddOpen(uint64_t chunkId, ChunkPartType chunkType) {
	auto *chunk = hddChunkFindAndLock(chunkId, chunkType);
	if (chunk == ChunkNotFound) {
		return SAUNAFS_ERROR_NOCHUNK;
	}

	int status = hddOpen(chunk);
	hddChunkRelease(chunk);

	return status;
}

int hddClose(IChunk *chunk) {
	assert(chunk);
	TRACETHIS1(chunk->id());
	int status = hddIOEnd(chunk);
	PRINTTHIS(status);
	if (status != SAUNAFS_STATUS_OK) {
		hddAddErrorAndPreserveErrno(chunk);
		hddReportDamagedChunk(chunk->id(), chunk->type());
	}
	return status;
}

int hddClose(uint64_t chunkId, ChunkPartType chunkType) {
	auto *chunk = hddChunkFindAndLock(chunkId, chunkType);
	if (chunk == NULL) {
		return SAUNAFS_ERROR_NOCHUNK;
	}
	int status = hddClose(chunk);
	hddChunkRelease(chunk);
	return status;
}

int hddReadCrcAndBlock(IChunk *chunk, uint16_t blockNumber,
                       OutputBuffer *outputBuffer) {
	LOG_AVG_TILL_END_OF_SCOPE0("hddReadCrcAndBlock");
	assert(chunk);
	TRACETHIS2(chunk->id(), blockNumber);

	int bytesRead = 0;

	if (blockNumber >= SFSBLOCKSINCHUNK) {
		return SAUNAFS_ERROR_BNUMTOOBIG;
	}

	if (blockNumber >= chunk->blocks()) {
		bytesRead = outputBuffer->copyIntoBuffer(&gEmptyBlockCrc, kCrcSize);
		static const std::vector<uint8_t> zeros_block(SFSBLOCKSIZE, 0);
		bytesRead += outputBuffer->copyIntoBuffer(zeros_block);
		if (static_cast<uint32_t>(bytesRead) != kHddBlockSize) {
			return SAUNAFS_ERROR_IO;
		}
	} else {
		int32_t toBeRead = SFSBLOCKSIZE;
		off_t off = chunk->getBlockOffset(blockNumber);

		const uint8_t *crcData =
		    gOpenChunks.getResource(chunk->metaFD()).crcData() +
		    blockNumber * kCrcSize;
		outputBuffer->copyIntoBuffer(crcData, kCrcSize);
		bytesRead = outputBuffer->copyIntoBuffer(chunk, SFSBLOCKSIZE, off);

		if (bytesRead != toBeRead) {
			hddAddErrorAndPreserveErrno(chunk);
			safs_silent_errlog(LOG_WARNING,
			                   "hddReadCrcAndBlock: file:%s"
			                   " - read error on block: %d",
			                   chunk->dataFilename().c_str(), blockNumber);
			hddReportDamagedChunk(chunk->id(), chunk->type());
			return SAUNAFS_ERROR_IO;
		}
	}

	return SAUNAFS_STATUS_OK;
}

int hddPrefetchBlocks(uint64_t chunkId, ChunkPartType chunkType,
                      uint32_t firstBlock, uint16_t numberOfBlocks) {
	LOG_AVG_TILL_END_OF_SCOPE0("hddPrefetchBlocks");

	auto *chunk = hddChunkFindAndLock(chunkId, chunkType);
	if (chunk == ChunkNotFound) {
		safs_pretty_syslog(LOG_WARNING, "error finding chunk for prefetching: %"
		                   PRIu64, chunkId);
		return SAUNAFS_ERROR_NOCHUNK;
	}

	int status = hddOpen(chunk);
	if (status != SAUNAFS_STATUS_OK) {
		safs_pretty_syslog(LOG_WARNING, "error opening chunk for prefetching: %"
		                   PRIu64 " - %s",
				chunkId, saunafs_error_string(status));
		hddChunkRelease(chunk);
		return status;
	}

	chunk->owner()->prefetchChunkBlocks(*chunk, firstBlock, numberOfBlocks);

	safs_silent_syslog(LOG_DEBUG, "chunkserver.hddPrefetchBlocks chunk: %"
	                   PRIu64 "status: %u firstBlock: %u nrOfBlocks: %u",
	                   chunkId, status, firstBlock, numberOfBlocks);

	status = hddClose(chunk);
	if (status != SAUNAFS_STATUS_OK) {
		safs_pretty_syslog(LOG_WARNING,
		                   "error closing prefetched chunk: %" PRIu64 " - %s",
		                   chunkId, saunafs_error_string(status));
	}

	hddChunkRelease(chunk);

	return status;
}

static void hddReadAheadAndBehind(IChunk *chunk, uint16_t block,
                                  uint32_t maxBlocksToBeReadBehind,
                                  uint32_t blocksToBeReadAhead) {
	// Ask OS for an appropriate read ahead and (if requested and needed)
	// read some blocks that were possibly skipped in a sequential file read
	if (chunk->blockExpectedToBeReadNext() < block &&
	    maxBlocksToBeReadBehind > 0) {
		// We were asked to read some possibly skipped blocks.
		uint16_t firstBlockToRead = chunk->blockExpectedToBeReadNext();
		// Try to prevent all possible overflows:
		if (firstBlockToRead + maxBlocksToBeReadBehind < block) {
			firstBlockToRead = block - maxBlocksToBeReadBehind;
		}
		sassert(firstBlockToRead < block);
		chunk->owner()->prefetchChunkBlocks(
		    *chunk, firstBlockToRead,
		    blocksToBeReadAhead + block - firstBlockToRead);
		OutputBuffer buffer =
		    OutputBuffer(kHddBlockSize * (block - firstBlockToRead));
		for (uint16_t b = firstBlockToRead; b < block; ++b) {
			hddReadCrcAndBlock(chunk, b, &buffer);
		}
	} else {
		chunk->owner()->prefetchChunkBlocks(*chunk, block, blocksToBeReadAhead);
	}

	chunk->setBlockExpectedToBeReadNext(
	    std::max<uint16_t>(block + 1, chunk->blockExpectedToBeReadNext()));
}

/**
* Checks the CRC for the requested full block.
* The check may be skipped if the forceCheck is false and the configuration
* option HDD_CHECK_CRC_WHEN_READING is set to 0 (false).
* @param chunk Chunk to read from.
* @param block Block to check.
* @param outputBuffer Assumes the outputBuffer is already filled with data.
* @param forceCheck If true, the CRC is checked even if the option is disabled
                    from the configuration. This is needed to keep integrity of
                    partial reads.
*/
int hddCheckCrcForFullBlock(IChunk *chunk, uint16_t block,
                            OutputBuffer *outputBuffer, bool forceCheck) {
	if (!forceCheck && (!gCheckCrcWhenReading || block >= chunk->blocks())) {
		return SAUNAFS_STATUS_OK;
	}

	const uint8_t *crcData =
	    gOpenChunks.getResource(chunk->metaFD()).crcData() + block * kCrcSize;
	if (!outputBuffer->checkCRC(SFSBLOCKSIZE, get32bit(&crcData))) {
		hddAddChunkToTestQueue(ChunkWithVersionAndType{
		    chunk->id(), chunk->version(), chunk->type()});
		return SAUNAFS_ERROR_CRC;
	}

	return SAUNAFS_STATUS_OK;
};

int hddRead(uint64_t chunkId, uint32_t version, ChunkPartType chunkType,
            uint32_t offset, uint32_t size,
            [[maybe_unused]] uint32_t maxBlocksToBeReadBehind,
            [[maybe_unused]] uint32_t blocksToBeReadAhead,
            OutputBuffer *outputBuffer) {
	LOG_AVG_TILL_END_OF_SCOPE0("hddRead");
	TRACETHIS3(chunkId, offset, size);

	safs_silent_syslog(LOG_DEBUG, "chunkserver.hddRead");

	uint32_t offsetWithinBlock = offset % SFSBLOCKSIZE;

	if ((size == 0) || ((offsetWithinBlock + size) > SFSBLOCKSIZE)) {
		return SAUNAFS_ERROR_WRONGSIZE;
	}

	auto* chunk = hddChunkFindAndLock(chunkId, chunkType);

	if (chunk == ChunkNotFound) {
		return SAUNAFS_ERROR_NOCHUNK;
	}

	if (chunk->version() != version && version > 0) {
		hddChunkRelease(chunk);
		return SAUNAFS_ERROR_WRONGVERSION;
	}

	uint16_t block = offset / SFSBLOCKSIZE;

	// Zoned devices use direct_io, so prefetched data is not cached
	if (!chunk->owner()->isZonedDevice()) {
		hddReadAheadAndBehind(chunk, block, maxBlocksToBeReadBehind,
		                      blocksToBeReadAhead);
	}

	// Put checksum of the requested data followed by data itself into buffer.
	// If possible (in case when whole block is read) try to put data directly
	// into passed outputBuffer, otherwise use temporary buffer to recompute
	// the checksum

	int status = SAUNAFS_STATUS_OK;

	if (size == SFSBLOCKSIZE) {  // Full block
		status = hddReadCrcAndBlock(chunk, block, outputBuffer);

		if (status == SAUNAFS_STATUS_OK) {
			status = hddCheckCrcForFullBlock(chunk, block, outputBuffer, false);
		}
	} else {  // Partial block
		OutputBuffer tmp(kHddBlockSize);
		status = hddReadCrcAndBlock(chunk, block, &tmp);

		if (status == SAUNAFS_STATUS_OK) {  // Succesful read of the full block
			status = hddCheckCrcForFullBlock(chunk, block, &tmp, true);

			if (status == SAUNAFS_STATUS_OK) {  // CRC is OK or check disabled
				uint8_t crcBuff[kCrcSize];
				uint8_t *crcBuffPointer = crcBuff;
				put32bit(&crcBuffPointer,
				         mycrc32(0, tmp.data() + kCrcSize + offsetWithinBlock,
				                 size));
				outputBuffer->copyIntoBuffer(crcBuff, kCrcSize);
				outputBuffer->copyIntoBuffer(
				    tmp.data() + kCrcSize + offsetWithinBlock, size);
			}
		}
	}

	PRINTTHIS(status);
	hddChunkRelease(chunk);
	return status;
}

/// A way of handling sparse files. If block is filled with zeros and crcBuffer
/// is filled with zeros as well, rewrite the crcBuffer so that it stores proper
/// CRC.
void hddRecomputeCrcIfBlockEmpty(uint8_t *block, uint8_t *crcBuffer) {
	const uint8_t* tmpPtr = crcBuffer;
	uint32_t crc = get32bit(&tmpPtr);

	recompute_crc_if_block_empty(block, crc);
	uint8_t* tmpPtr2 = crcBuffer;
	put32bit(&tmpPtr2, crc);
}

int hddChunkWriteBlock(uint64_t chunkId, uint32_t version,
                       ChunkPartType chunkType, uint16_t blocknum,
                       uint32_t offset, uint32_t size, uint32_t crc,
                       const uint8_t *buffer) {
	auto *chunk = hddChunkFindAndLock(chunkId, chunkType);

	if (chunk == ChunkNotFound) {
		return SAUNAFS_ERROR_NOCHUNK;
	}

	auto *crcData = gOpenChunks.getResource(chunk->metaFD()).crcData();
	int status = chunk->owner()->writeChunkBlock(
	    chunk, version, blocknum, offset, size, crc, crcData, buffer);
	hddChunkRelease(chunk);

	return status;
}

/* chunk info */

int hddChunkGetNumberOfBlocks(uint64_t chunkId, ChunkPartType chunkType,
                              uint32_t version, uint16_t *blocks) {
	TRACETHIS1(chunkId);

	auto *chunk = hddChunkFindAndLock(chunkId, chunkType);
	*blocks = 0;
	if (chunk == ChunkNotFound) {
		return SAUNAFS_ERROR_NOCHUNK;
	}

	if (chunk->version() != version && version > 0) {
		hddChunkRelease(chunk);
		return SAUNAFS_ERROR_WRONGVERSION;
	}

	*blocks = chunk->blocks();
	hddChunkRelease(chunk);

	return SAUNAFS_STATUS_OK;
}

std::pair<int, IChunk *> hddInternalCreateChunk(uint64_t chunkId,
                                                uint32_t version,
                                                ChunkPartType chunkType) {
	TRACETHIS2(chunkId, version);
	IDisk *disk;
	int status;

	IChunk *chunk = ChunkNotFound;

	{
		std::scoped_lock disksLockGuard(gDisksMutex);

		disk = gDiskManager->getDiskForNewChunk(chunkType);

		if (disk == DiskNotFound) {
			return {SAUNAFS_ERROR_NOSPACE, ChunkNotFound};
		}

		chunk = hddChunkCreate(disk, chunkId, chunkType, version);
	}

	if (chunk == ChunkNotFound) {
		return {SAUNAFS_ERROR_CHUNKEXIST, ChunkNotFound};
	}

	status = hddIOBegin(chunk, 1);
	PRINTTHIS(status);

	if (status != SAUNAFS_STATUS_OK) {
		hddAddErrorAndPreserveErrno(chunk);
		hddDeleteChunkFromRegistry(chunk);
		return {SAUNAFS_ERROR_IO, ChunkNotFound};
	}

	uint8_t *ptr = chunk->getChunkHeaderBuffer();
	memset(ptr, 0, chunk->getHeaderSize());

	{
		std::unique_ptr<ChunkSignature> signature =
		    disk->createChunkSignature(chunk);
		signature->serialize(&ptr);
	}

	{
		DiskWriteStatsUpdater updater(chunk->owner(), chunk->getHeaderSize());

		if (disk->writeChunkHeader(chunk) != SAUNAFS_STATUS_OK) {
			hddAddErrorAndPreserveErrno(chunk);
			safs_silent_errlog(LOG_WARNING,
			                   "create_newchunk: file:%s - write error",
			                   chunk->metaFilename().c_str());
			hddIOEnd(chunk);
			disk->unlinkChunk(chunk);
			hddDeleteChunkFromRegistry(chunk);
			updater.markWriteAsFailed();
			return {SAUNAFS_ERROR_IO, ChunkNotFound};
		}
	}

	HddStats::overheadWrite(chunk->getHeaderSize());

	status = hddIOEnd(chunk);

	PRINTTHIS(status);
	if (status != SAUNAFS_STATUS_OK) {
		hddAddErrorAndPreserveErrno(chunk);
		disk->unlinkChunk(chunk);
		hddDeleteChunkFromRegistry(chunk);
		return {status, ChunkNotFound};
	}

	return {SAUNAFS_STATUS_OK, chunk};
}

int hddInternalCreate(uint64_t chunkId, uint32_t version,
                      ChunkPartType chunkType) {
	TRACETHIS2(chunkId, version);

	HddStats::gStatsOperationsCreate++;

	auto [creationStatus, chunk] =
	    hddInternalCreateChunk(chunkId, version, chunkType);

	if (creationStatus == SAUNAFS_STATUS_OK) {
		hddChunkRelease(chunk);
	}

	return creationStatus;
}

static int hddInternalTestChunk(uint64_t chunkId, uint32_t version,
                                ChunkPartType chunkType) {
	TRACETHIS2(chunkId, version);
	uint16_t block;

	HddStats::gStatsOperationsTest++;

	auto *chunk = hddChunkFindAndLock(chunkId, chunkType);

	if (chunk == ChunkNotFound) {
		return SAUNAFS_ERROR_NOCHUNK;
	}

	if (chunk->version() != version && version > 0) {
		hddChunkRelease(chunk);
		return SAUNAFS_ERROR_WRONGVERSION;
	}

	int status = hddIOBegin(chunk, 0);
	PRINTTHIS(status);
	if (status != SAUNAFS_STATUS_OK) {
		hddAddErrorAndPreserveErrno(chunk);
		hddChunkRelease(chunk);
		return status;
	}

	uint8_t *blockbuffer = getChunkBlockBuffer();
	// will be overwritten in the loop below if the test fails
	status = SAUNAFS_STATUS_OK;

	auto *crcData = gOpenChunks.getResource(chunk->metaFD()).crcData();
	for (block = 0; block < chunk->blocks(); ++block) {
		auto readBytes = chunk->owner()->readBlockAndCrc(
		    chunk, blockbuffer, crcData, block, "testChunk");
		uint8_t *dataInBuffer = blockbuffer + kCrcSize; // Skip crc

		if (readBytes < 0) {
			status = SAUNAFS_ERROR_IO;
			break;
		}

		HddStats::overheadRead(readBytes);

		const uint8_t* crcBuffPointer = blockbuffer;
		uint32_t crc = get32bit(&crcBuffPointer);

		if (crc != mycrc32(0, dataInBuffer, SFSBLOCKSIZE)) {
			errno = 0; // set anything to errno
			hddAddErrorAndPreserveErrno(chunk);
			safs_pretty_syslog(LOG_WARNING,
			                   "testChunk: file:%s - crc error on block: %d",
			                   chunk->metaFilename().c_str(), block);
			status = SAUNAFS_ERROR_CRC;
			break;
		}
	}
#ifdef SAUNAFS_HAVE_POSIX_FADVISE
	// Always advise the OS that tested chunks should not be cached. Don't rely
	// on hdd_delayed_ops to do it for us, because it may be disabled using a
	// config file.
	posix_fadvise(chunk->metaFD(), 0, 0, POSIX_FADV_DONTNEED);
#endif /* SAUNAFS_HAVE_POSIX_FADVISE */
	if (status != SAUNAFS_STATUS_OK) {
		// test failed -- chunk is damaged
		hddIOEnd(chunk);
		hddChunkRelease(chunk);
		return status;
	}
	status = hddIOEnd(chunk);
	if (status != SAUNAFS_STATUS_OK) {
		hddAddErrorAndPreserveErrno(chunk);
		hddChunkRelease(chunk);
		return status;
	}
	hddChunkRelease(chunk);
	return SAUNAFS_STATUS_OK;
}

static int hddInternalDuplicate(uint64_t chunkId, uint32_t chunkVersion,
                                uint32_t chunkNewVersion,
                                ChunkPartType chunkType, uint64_t copyChunkId,
                                uint32_t copyChunkVersion) {
	TRACETHIS();
	uint16_t block;
	int32_t retSize;
	int status;
	IChunk *dupChunk, *originalChunk;
	IDisk *dupDisk, *originalDisk;

	HddStats::gStatsOperationsDuplicate++;

	uint8_t *blockBuffer = getChunkBlockBuffer() + kCrcSize;

	originalChunk = hddChunkFindAndLock(chunkId, chunkType);

	if (originalChunk == ChunkNotFound) {
		return SAUNAFS_ERROR_NOCHUNK;
	}
	if (originalChunk->version() != chunkVersion && chunkVersion > 0) {
		hddChunkRelease(originalChunk);
		return SAUNAFS_ERROR_WRONGVERSION;
	}
	if (copyChunkVersion == 0) {
		copyChunkVersion = chunkNewVersion;
	}

	{
		std::unique_lock disksUniqueLock(gDisksMutex);
		dupDisk = gDiskManager->getDiskForNewChunk(chunkType);
		if (dupDisk == DiskNotFound) {
			disksUniqueLock.unlock();
			hddChunkRelease(originalChunk);
			return SAUNAFS_ERROR_NOSPACE;
		}

		dupChunk = hddChunkCreate(dupDisk, copyChunkId, chunkType,
		                          copyChunkVersion);
	}

	if (dupChunk == ChunkNotFound) {
		hddChunkRelease(originalChunk);
		return SAUNAFS_ERROR_CHUNKEXIST;
	}

	sassert(dupChunk->chunkFormat() == originalChunk->chunkFormat());

	originalDisk = originalChunk->owner();

	if (chunkNewVersion != chunkVersion) {
		if (dupChunk->renameChunkFile(chunkNewVersion) < 0) {
			hddAddErrorAndPreserveErrno(originalChunk);
			safs_silent_errlog(LOG_WARNING, "duplicate: file:%s - rename error",
			                   originalChunk->metaFilename().c_str());
			hddDeleteChunkFromRegistry(dupChunk);
			hddChunkRelease(originalChunk);
			return SAUNAFS_ERROR_IO;
		}

		status = hddIOBegin(originalChunk, 0, chunkVersion);
		if (status != SAUNAFS_STATUS_OK) {
			hddAddErrorAndPreserveErrno(originalChunk);
			hddDeleteChunkFromRegistry(dupChunk);
			hddChunkRelease(originalChunk);
			return status;  //can't change file version
		}

		status = originalDisk->overwriteChunkVersion(originalChunk,
		                                             chunkNewVersion);
		if (status != SAUNAFS_STATUS_OK) {
			hddAddErrorAndPreserveErrno(originalChunk);
			safs_silent_errlog(LOG_WARNING, "duplicate: file:%s - write error",
			                   dupChunk->metaFilename().c_str());
			hddDeleteChunkFromRegistry(dupChunk);
			hddIOEnd(originalChunk);
			hddChunkRelease(originalChunk);
			return SAUNAFS_ERROR_IO;
		}
	} else {  // chunkNewVersion == chunkVersion
		status = hddIOBegin(originalChunk, 0);
		if (status != SAUNAFS_STATUS_OK) {
			hddAddErrorAndPreserveErrno(originalChunk);
			hddDeleteChunkFromRegistry(dupChunk);
			hddReportDamagedChunk(chunkId, chunkType);
			hddChunkRelease(originalChunk);
			return status;
		}
	}

	status = hddIOBegin(dupChunk, 1);
	if (status != SAUNAFS_STATUS_OK) {
		hddAddErrorAndPreserveErrno(dupChunk);
		hddDeleteChunkFromRegistry(dupChunk);
		hddIOEnd(originalChunk);
		hddChunkRelease(originalChunk);
		return status;
	}

	int32_t blockSize = SFSBLOCKSIZE;

	// Clean the header buffer and copy the signature
	uint8_t *ptr = dupChunk->getChunkHeaderBuffer();
	memset(ptr, 0, dupChunk->getHeaderSize());

	dupDisk->serializeEmptyChunkSignature(&ptr,
	                                      copyChunkId,
	                                      copyChunkVersion,
	                                      chunkType);

	uint8_t *dupCrcData = gOpenChunks.getResource(dupChunk->metaFD()).crcData();
	uint8_t *origCrcData =
	    gOpenChunks.getResource(originalChunk->metaFD()).crcData();
	// Copy the CRC to the in-memory OpenChunk
	memcpy(dupCrcData, origCrcData, dupChunk->getCrcBlockSize());
	// and to the header buffer to save it to device
	memcpy(dupChunk->getChunkHeaderBuffer() + dupChunk->getCrcOffset(),
	       origCrcData, dupChunk->getCrcBlockSize());

	{
		DiskWriteStatsUpdater updater(dupDisk, dupChunk->getHeaderSize());

		if (dupDisk->writeChunkHeader(dupChunk) != SAUNAFS_STATUS_OK) {
			hddAddErrorAndPreserveErrno(dupChunk);
			safs_silent_errlog(LOG_WARNING,
			                   "duplicate: file:%s - hdr write error",
			                   dupChunk->metaFilename().c_str());
			hddIOEnd(dupChunk);
			dupDisk->unlinkChunk(dupChunk);
			hddDeleteChunkFromRegistry(dupChunk);
			hddIOEnd(originalChunk);
			hddChunkRelease(originalChunk);
			updater.markWriteAsFailed();
			return SAUNAFS_ERROR_IO;
		}
	}

	HddStats::overheadWrite(dupChunk->getHeaderSize());

	originalDisk->lseekData(originalChunk, dupChunk->getBlockOffset(0),
	                        SEEK_SET);

	// Read each original block and write it to the duplicated chunk
	for (block = 0; block < originalChunk->blocks(); block++) {
		{
			DiskReadStatsUpdater updater(originalDisk, blockSize);

			retSize = originalDisk->preadData(originalChunk, blockBuffer,
			                                  blockSize, block * SFSBLOCKSIZE);

			if (retSize != blockSize) {
				hddAddErrorAndPreserveErrno(originalChunk);
				safs_silent_errlog(LOG_WARNING,
				                   "duplicate: file:%s - data read error",
				                   dupChunk->metaFilename().c_str());
				hddIOEnd(dupChunk);
				dupDisk->unlinkChunk(dupChunk);
				hddDeleteChunkFromRegistry(dupChunk);
				hddIOEnd(originalChunk);
				hddReportDamagedChunk(chunkId, chunkType);
				hddChunkRelease(originalChunk);
				updater.markReadAsFailed();
				return SAUNAFS_ERROR_IO;
			}
		}
		HddStats::overheadRead(blockSize);

		{
			DiskWriteStatsUpdater updater(dupDisk, blockSize);

			if (dupDisk->isZonedDevice()) {
				const uint8_t *dupCrcBlock = dupCrcData + kCrcSize * block;
				uint32_t crc = get32bit(&dupCrcBlock);
				if (dupDisk->writeChunkBlock(
				        dupChunk, dupChunk->version(), block, 0, SFSBLOCKSIZE,
				        crc, dupCrcData, blockBuffer) == SAUNAFS_STATUS_OK) {
					retSize = SFSBLOCKSIZE;
				} else {
					retSize = SAUNAFS_ERROR_IO;
				}
			} else {
				retSize = dupDisk->writeChunkData(dupChunk, blockBuffer,
				                                  blockSize, 0);
			}

			if (retSize != blockSize) {
				hddAddErrorAndPreserveErrno(dupChunk);
				safs_silent_errlog(LOG_WARNING,
				                   "duplicate: file:%s - data write error",
				                   dupChunk->metaFilename().c_str());
				hddIOEnd(dupChunk);
				dupDisk->unlinkChunk(dupChunk);
				hddDeleteChunkFromRegistry(dupChunk);
				hddIOEnd(originalChunk);
				hddChunkRelease(originalChunk);
				updater.markWriteAsFailed();
				return SAUNAFS_ERROR_IO;        //write error
			}
		}
		HddStats::overheadWrite(blockSize);
	}

	status = hddIOEnd(originalChunk);
	if (status != SAUNAFS_STATUS_OK) {
		hddAddErrorAndPreserveErrno(originalChunk);
		hddIOEnd(dupChunk);
		dupDisk->unlinkChunk(dupChunk);
		hddDeleteChunkFromRegistry(dupChunk);
		hddReportDamagedChunk(chunkId, chunkType);
		hddChunkRelease(originalChunk);
		return status;
	}

	status = hddIOEnd(dupChunk);
	if (status != SAUNAFS_STATUS_OK) {
		hddAddErrorAndPreserveErrno(dupChunk);
		dupDisk->unlinkChunk(dupChunk);
		hddDeleteChunkFromRegistry(dupChunk);
		hddChunkRelease(originalChunk);
		return status;
	}

	dupChunk->setBlocks(originalChunk->blocks());
	dupDisk->setNeedRefresh(true);
	hddChunkRelease(dupChunk);
	hddChunkRelease(originalChunk);

	return SAUNAFS_STATUS_OK;
}

int hddInternalUpdateVersion(IChunk *chunk, uint32_t version,
                             uint32_t newversion) {
	TRACETHIS();
	int status;
	assert(chunk);
	if (chunk->version() != version && version > 0) {
		return SAUNAFS_ERROR_WRONGVERSION;
	}
	if (chunk->renameChunkFile(newversion) < 0) {
		hddAddErrorAndPreserveErrno(chunk);
		safs_silent_errlog(LOG_WARNING,
		                   "hddInternalUpdateVersion: file:%s - rename error",
		                   chunk->metaFilename().c_str());
		return SAUNAFS_ERROR_IO;
	}
	status = hddIOBegin(chunk, 0, version);
	if (status != SAUNAFS_STATUS_OK) {
		hddAddErrorAndPreserveErrno(chunk);
		safs_silent_errlog(LOG_WARNING,
		                   "hddInternalUpdateVersion: file:%s - open error",
		                   chunk->metaFilename().c_str());
		return status;
	}
	status = chunk->owner()->overwriteChunkVersion(chunk, newversion);
	if (status != SAUNAFS_STATUS_OK) {
		hddAddErrorAndPreserveErrno(chunk);
		safs_silent_errlog(LOG_WARNING,
		                   "hddInternalUpdateVersion: file:%s - write error",
		                   chunk->metaFilename().c_str());
		hddIOEnd(chunk);
		return SAUNAFS_ERROR_IO;
	}
	status = hddIOEnd(chunk);
	if (status != SAUNAFS_STATUS_OK) {
		hddAddErrorAndPreserveErrno(chunk);
	}
	return status;
}

int hddInternalUpdateVersion(uint64_t chunkId, uint32_t version,
                             uint32_t newversion, ChunkPartType chunkType) {
	TRACETHIS();

	HddStats::gStatsOperationsVersion++;

	auto *chunk = hddChunkFindAndLock(chunkId, chunkType);
	if (chunk == ChunkNotFound) {
		return SAUNAFS_ERROR_NOCHUNK;
	}

	int status = hddInternalUpdateVersion(chunk, version, newversion);
	hddChunkRelease(chunk);

	return status;
}

static int hddInternalTruncate(uint64_t chunkId, ChunkPartType chunkType,
                               uint32_t oldVersion, uint32_t newVersion,
                               uint32_t length) {
	TRACETHIS4(chunkId, oldVersion, newVersion, length);
	int status;
	IChunk *chunk;
	uint32_t blocks;
	uint32_t crc;

	HddStats::gStatsOperationsTruncate++;

	if (length > SFSCHUNKSIZE) {
		return SAUNAFS_ERROR_WRONGSIZE;
	}

	chunk = hddChunkFindAndLock(chunkId, chunkType);

	// step 1 - change version
	if (chunk == ChunkNotFound) {
		return SAUNAFS_ERROR_NOCHUNK;
	}
	if (chunk->version() != oldVersion && oldVersion > 0) {
		hddChunkRelease(chunk);
		return SAUNAFS_ERROR_WRONGVERSION;
	}

	auto *disk = chunk->owner();
	uint8_t *blockBuffer = getChunkBlockBuffer() + kCrcSize;
	auto originalBlocks = chunk->blocks();

	if (chunk->renameChunkFile(newVersion) < 0) {
		hddAddErrorAndPreserveErrno(chunk);
		safs_silent_errlog(LOG_WARNING,
		                   "truncate: file:%s - rename error",
		                   chunk->metaFilename().c_str());
		hddChunkRelease(chunk);
		return SAUNAFS_ERROR_IO;
	}

	status = hddIOBegin(chunk, 0, oldVersion);
	if (status != SAUNAFS_STATUS_OK) {
		hddAddErrorAndPreserveErrno(chunk);
		hddChunkRelease(chunk);
		return status;  //can't change file version
	}

	status = disk->overwriteChunkVersion(chunk, newVersion);
	if (status != SAUNAFS_STATUS_OK) {
		hddAddErrorAndPreserveErrno(chunk);
		safs_silent_errlog(LOG_WARNING,
		                   "truncate: file:%s - write error",
		                   chunk->metaFilename().c_str());
		hddIOEnd(chunk);
		hddChunkRelease(chunk);
		return SAUNAFS_ERROR_IO;
	}
	chunk->setWasChanged(true);

	// step 2. truncate
	blocks = ((length + SFSBLOCKSIZE - 1) / SFSBLOCKSIZE);

	if (blocks > chunk->blocks()) {  //Expanding
		// Fill new blocks with empty CRC
		uint8_t *crcData = gOpenChunks.getResource(chunk->metaFD()).crcData();
		for (auto block = chunk->blocks(); block < blocks; block++) {
			memcpy(crcData + block * kCrcSize, &gEmptyBlockCrc, kCrcSize);
		}

		// Do the actual truncation to the aligned block size
		if (disk->ftruncateData(chunk,
		                        chunk->getFileSizeFromBlockCount(blocks)) < 0) {
			hddAddErrorAndPreserveErrno(chunk);
			safs_silent_errlog(LOG_WARNING,
			                   "truncate: file:%s - ftruncate error",
			                   chunk->dataFilename().c_str());
			hddIOEnd(chunk);
			hddChunkRelease(chunk);
			return SAUNAFS_ERROR_IO;
		}
	} else {  //Shrinking
		uint32_t fullBlocks = length / SFSBLOCKSIZE;
		uint32_t lastPartialBlockSize = length - fullBlocks * SFSBLOCKSIZE;

		if (lastPartialBlockSize > 0) {
			auto len = chunk->getFileSizeFromBlockCount(fullBlocks) +
			           lastPartialBlockSize;
			if (disk->ftruncateData(chunk, len) < 0) {
				hddAddErrorAndPreserveErrno(chunk);
				safs_silent_errlog(LOG_WARNING,
				    "truncate: file:%s - ftruncate error",
				    chunk->metaFilename().c_str());
				hddIOEnd(chunk);
				hddChunkRelease(chunk);
				return SAUNAFS_ERROR_IO;
			}
		}

		if (disk->ftruncateData(chunk,
		                        chunk->getFileSizeFromBlockCount(blocks)) < 0) {
			hddAddErrorAndPreserveErrno(chunk);
			safs_silent_errlog(LOG_WARNING,
			                   "truncate: file:%s - ftruncate error",
			                   chunk->dataFilename().c_str());
			hddIOEnd(chunk);
			hddChunkRelease(chunk);
			return SAUNAFS_ERROR_IO;
		}

		// remove unneeded blocks
		if (disk->isZonedDevice()) {
			chunk->shrinkToBlocks(static_cast<uint16_t>(blocks));
		}

		if (lastPartialBlockSize > 0) {
			auto offset = chunk->getBlockOffset(fullBlocks);

			auto toBeRead =
			    disk->isZonedDevice() ? SFSBLOCKSIZE : lastPartialBlockSize;

			{
				DiskReadStatsUpdater updater(disk, toBeRead);

				// Check that we can read the truncated file
				if (disk->preadData(chunk, blockBuffer, toBeRead, offset) !=
				    static_cast<ssize_t>(toBeRead)) {
					hddAddErrorAndPreserveErrno(chunk);
					safs_silent_errlog(LOG_WARNING,
					                   "truncate: file:%s - read error",
					                   chunk->metaFilename().c_str());
					hddIOEnd(chunk);
					hddChunkRelease(chunk);
					updater.markReadAsFailed();
					return SAUNAFS_ERROR_IO;
				}
			}

			HddStats::overheadRead(toBeRead);

			if (disk->isZonedDevice()) {
				memset(blockBuffer + lastPartialBlockSize, 0,
				       SFSBLOCKSIZE - lastPartialBlockSize);
			}

			crc = mycrc32_zeroexpanded(0, blockBuffer, lastPartialBlockSize,
			                           SFSBLOCKSIZE - lastPartialBlockSize);

			uint8_t crcBuff[kCrcSize];
			uint8_t* crcBuffPointer = crcBuff;
			put32bit(&crcBuffPointer, crc);

			uint8_t *crData = gOpenChunks.getResource(chunk->metaFD()).crcData();
			memcpy(crData + fullBlocks * kCrcSize, crcBuff, kCrcSize);

			uint32_t jump = disk->isZonedDevice() ? 2 : 1;

			for (auto block = fullBlocks + jump; block < originalBlocks;
			     block++) {
				memcpy(crData + block * kCrcSize, &gEmptyBlockCrc, kCrcSize);
			}

			if (disk->isZonedDevice()) {
				{
					DiskWriteStatsUpdater updater(disk, SFSBLOCKSIZE);

					int32_t retSize = SFSBLOCKSIZE;
					auto *crcData =
					    gOpenChunks.getResource(chunk->metaFD()).crcData();

					if (disk->writeChunkBlock(chunk, chunk->version(),
					                          fullBlocks, 0, SFSBLOCKSIZE, crc,
					                          crcData, blockBuffer) !=
					    SAUNAFS_STATUS_OK) {
						retSize = SAUNAFS_ERROR_IO;
					}

					if (retSize != SFSBLOCKSIZE) {
						hddAddErrorAndPreserveErrno(chunk);
						safs_silent_errlog(LOG_WARNING,
						    "truncate: file:%s - data write error",
						    chunk->metaFilename().c_str());
						hddIOEnd(chunk);
						hddChunkRelease(chunk);
						updater.markWriteAsFailed();

						return SAUNAFS_ERROR_IO;
					}
				}

				HddStats::overheadWrite(SFSBLOCKSIZE);
			}
		}
	}

	if (chunk->blocks() != blocks) {
		disk->setNeedRefresh(true);
	}

	disk->setChunkBlocks(chunk, chunk->blocks(), blocks);

	status = hddIOEnd(chunk);
	if (status != SAUNAFS_STATUS_OK) {
		hddAddErrorAndPreserveErrno(chunk);
	}

	hddChunkRelease(chunk);

	return status;
}

static int hddInternalDuplicateTruncate(uint64_t chunkId, uint32_t chunkVersion,
                                        uint32_t chunkNewVersion,
                                        ChunkPartType chunkType,
                                        uint64_t copyChunkId,
                                        uint32_t copyChunkVersion,
                                        uint32_t copyChunkLength) {
	TRACETHIS();
	uint16_t block;
	uint16_t blocks;
	int32_t retSize;
	int status;
	IChunk *dupChunk, *originalChunk;
	IDisk *dupDisk, *origDisk;

	HddStats::gStatsOperationsDupTrunc++;

	if (copyChunkLength > SFSCHUNKSIZE) {
		return SAUNAFS_ERROR_WRONGSIZE;
	}

	originalChunk = hddChunkFindAndLock(chunkId, chunkType);

	if (originalChunk == nullptr) {
		return SAUNAFS_ERROR_NOCHUNK;
	}
	if (originalChunk->version() != chunkVersion && chunkVersion > 0) {
		hddChunkRelease(originalChunk);
		return SAUNAFS_ERROR_WRONGVERSION;
	}

	if (copyChunkVersion == 0) {
		copyChunkVersion = chunkNewVersion;
	}

	uint8_t *blockBuffer = getChunkBlockBuffer() + kCrcSize;

	{
		std::unique_lock disksUniqueLock(gDisksMutex);

		dupDisk = gDiskManager->getDiskForNewChunk(chunkType);

		if (dupDisk == DiskNotFound) {
			disksUniqueLock.unlock();
			hddChunkRelease(originalChunk);
			return SAUNAFS_ERROR_NOSPACE;
		}

		dupChunk = hddChunkCreate(dupDisk, copyChunkId, chunkType,
		                          copyChunkVersion);
	}

	if (dupChunk == ChunkNotFound) {
		hddChunkRelease(originalChunk);
		return SAUNAFS_ERROR_CHUNKEXIST;
	}

	uint8_t *headerBuffer = dupChunk->getChunkHeaderBuffer();
	origDisk = originalChunk->owner();

	if (chunkNewVersion != chunkVersion) { // Different versions
		if (originalChunk->renameChunkFile(chunkNewVersion) < 0) {
			hddAddErrorAndPreserveErrno(originalChunk);
			safs_silent_errlog(LOG_WARNING,
			                   "duptrunc: file:%s - rename error",
			                   originalChunk->metaFilename().c_str());
			hddDeleteChunkFromRegistry(dupChunk);
			hddChunkRelease(originalChunk);
			return SAUNAFS_ERROR_IO;
		}

		status = hddIOBegin(originalChunk, 0, chunkVersion);
		if (status != SAUNAFS_STATUS_OK) {
			hddAddErrorAndPreserveErrno(originalChunk);
			hddDeleteChunkFromRegistry(dupChunk);
			hddChunkRelease(originalChunk);
			return status;  //can't change file version
		}

		status = origDisk->overwriteChunkVersion(originalChunk,
		                                         chunkNewVersion);
		if (status != SAUNAFS_STATUS_OK) {
			hddAddErrorAndPreserveErrno(originalChunk);
			safs_silent_errlog(LOG_WARNING,
			                   "duptrunc: file:%s - write error",
			                   dupChunk->metaFilename().c_str());
			hddDeleteChunkFromRegistry(dupChunk);
			hddIOEnd(originalChunk);
			hddChunkRelease(originalChunk);
			return SAUNAFS_ERROR_IO;
		}
	} else { // It is the same version
		status = hddIOBegin(originalChunk, 0);
		if (status != SAUNAFS_STATUS_OK) {
			hddAddErrorAndPreserveErrno(originalChunk);
			hddDeleteChunkFromRegistry(dupChunk);
			hddReportDamagedChunk(chunkId, chunkType);
			hddChunkRelease(originalChunk);
			return status;
		}
	}

	status = hddIOBegin(dupChunk, 1);
	if (status != SAUNAFS_STATUS_OK) {
		hddAddErrorAndPreserveErrno(dupChunk);
		hddDeleteChunkFromRegistry(dupChunk);
		hddIOEnd(originalChunk);
		hddChunkRelease(originalChunk);
		return status;
	}

	sassert((dupChunk == nullptr && originalChunk == nullptr) ||
	        (dupChunk != nullptr && originalChunk != nullptr));

	blocks = (copyChunkLength + SFSBLOCKSIZE - 1) / SFSBLOCKSIZE;
	int32_t blockSize = SFSBLOCKSIZE;

	uint8_t *crcDataOriginal = nullptr;
	uint8_t *crcDataDup = nullptr;

	if (dupChunk) {
		memset(headerBuffer, 0, dupChunk->getHeaderSize());
		uint8_t *ptr = headerBuffer;

		dupDisk->serializeEmptyChunkSignature(&ptr, copyChunkId,
		                                      copyChunkVersion, chunkType);

		crcDataOriginal = gOpenChunks.getResource(originalChunk->metaFD()).crcData();
		memcpy(headerBuffer + dupChunk->getCrcOffset(), crcDataOriginal,
		       dupChunk->getCrcBlockSize());
		crcDataDup = gOpenChunks.getResource(dupChunk->metaFD()).crcData();
	}

	// Seek to the beggining of data block on both chunks
	if (!dupDisk->isZonedDevice()) {
		dupDisk->lseekData(dupChunk, dupChunk->getBlockOffset(0), SEEK_SET);
	}

	if (!origDisk->isZonedDevice()) {
		origDisk->lseekData(originalChunk, originalChunk->getBlockOffset(0),
		                    SEEK_SET);
	}

	if (blocks > originalChunk->blocks()) { // expanding
		for (block = 0 ; block < originalChunk->blocks() ; block++) {
			{
				DiskReadStatsUpdater updater(origDisk, blockSize);

				retSize = origDisk->preadData(originalChunk, blockBuffer,
				                              blockSize, block * SFSBLOCKSIZE);

				if (retSize != blockSize) {
					hddAddErrorAndPreserveErrno(originalChunk);
					safs_silent_errlog(LOG_WARNING,
					    "duptrunc: file:%s - data read error",
					    originalChunk->metaFilename().c_str());
					hddIOEnd(dupChunk);
					dupDisk->unlinkChunk(dupChunk);
					hddDeleteChunkFromRegistry(dupChunk);
					hddIOEnd(originalChunk);
					hddReportDamagedChunk(chunkId, chunkType);
					hddChunkRelease(originalChunk);
					updater.markReadAsFailed();
					return SAUNAFS_ERROR_IO;
				}
			}
			HddStats::overheadRead(blockSize);

			{
				DiskWriteStatsUpdater updater(dupDisk, blockSize);

				if (dupDisk->isZonedDevice()) {
					const uint8_t *dupCrcBlock = crcDataOriginal + kCrcSize * block;
					uint32_t crc = get32bit(&dupCrcBlock);

					if (dupDisk->writeChunkBlock(dupChunk, dupChunk->version(),
					                             block, 0, SFSBLOCKSIZE, crc,
					                             crcDataDup, blockBuffer) ==
					    SAUNAFS_STATUS_OK) {
						retSize = SFSBLOCKSIZE;
					} else {
						retSize = SAUNAFS_ERROR_IO;
					}
				} else {
					retSize = dupDisk->writeChunkData(dupChunk, blockBuffer,
					                                  blockSize, 0);
				}

				if (retSize != blockSize) {
					hddAddErrorAndPreserveErrno(dupChunk);
					safs_silent_errlog(LOG_WARNING,
					    "duptrunc: file:%s - data write error",
					    dupChunk->metaFilename().c_str());
					hddIOEnd(dupChunk);
					dupDisk->unlinkChunk(dupChunk);
					hddDeleteChunkFromRegistry(dupChunk);
					hddIOEnd(originalChunk);
					hddChunkRelease(originalChunk);
					updater.markWriteAsFailed();
					return SAUNAFS_ERROR_IO;
				}
			}
			HddStats::overheadWrite(blockSize);
		}

		if (dupChunk) {
			for (block = originalChunk->blocks(); block < blocks; block++) {
				memcpy(headerBuffer + dupChunk->getCrcOffset() +
				       kCrcSize * block, &gEmptyBlockCrc, kCrcSize);
			}
		}

		if (dupDisk->ftruncateData(
		        dupChunk, dupChunk->getFileSizeFromBlockCount(blocks)) < 0) {
			hddAddErrorAndPreserveErrno(dupChunk);
			safs_silent_errlog(LOG_WARNING,
			                   "duptrunc: file:%s - ftruncate error",
			                   dupChunk->metaFilename().c_str());
			hddIOEnd(dupChunk);
			dupDisk->unlinkChunk(dupChunk);
			hddDeleteChunkFromRegistry(dupChunk);
			hddIOEnd(originalChunk);
			hddChunkRelease(originalChunk);
			return SAUNAFS_ERROR_IO;        //write error
		}
	} else { // shrinking
		uint32_t lastBlockSize =
		    copyChunkLength - (copyChunkLength / SFSBLOCKSIZE) * SFSBLOCKSIZE;

		if (lastBlockSize == 0) { // aligned shrink
			for (block = 0; block < blocks; block++) {
				{
					DiskReadStatsUpdater updater(origDisk, blockSize);

					retSize = origDisk->preadData(originalChunk, blockBuffer,
					                              blockSize,
					                              block * SFSBLOCKSIZE);

					if (retSize != blockSize) {
						hddAddErrorAndPreserveErrno(originalChunk);
						safs_silent_errlog(LOG_WARNING,
						    "duptrunc: file:%s - data read error",
						    originalChunk->metaFilename().c_str());
						hddIOEnd(dupChunk);
						dupDisk->unlinkChunk(dupChunk);
						hddDeleteChunkFromRegistry(dupChunk);
						hddIOEnd(originalChunk);
						hddReportDamagedChunk(chunkId, chunkType);
						hddChunkRelease(originalChunk);
						updater.markReadAsFailed();
						return SAUNAFS_ERROR_IO;
					}
				}
				HddStats::overheadRead(blockSize);

				{
					DiskWriteStatsUpdater updater(dupDisk, blockSize);

					if (dupDisk->isZonedDevice()) {
						const uint8_t *dupCrcBlock = crcDataOriginal + kCrcSize * block;
						uint32_t crc = get32bit(&dupCrcBlock);

						if (dupDisk->writeChunkBlock(
						        dupChunk, dupChunk->version(), block, 0,
						        SFSBLOCKSIZE, crc, crcDataDup,
						        blockBuffer) == SAUNAFS_STATUS_OK) {
							retSize = SFSBLOCKSIZE;
						} else {
							retSize = SAUNAFS_ERROR_IO;
						}
					} else {
						retSize = dupDisk->writeChunkData(dupChunk, blockBuffer,
						                                  blockSize, 0);
					}

					if (retSize != blockSize) {
						hddAddErrorAndPreserveErrno(dupChunk);
						safs_silent_errlog(LOG_WARNING,
						    "duptrunc: file:%s - data write error",
						    dupChunk->metaFilename().c_str());
						hddIOEnd(dupChunk);
						dupDisk->unlinkChunk(dupChunk);
						hddDeleteChunkFromRegistry(dupChunk);
						hddIOEnd(originalChunk);
						hddChunkRelease(originalChunk);
						updater.markWriteAsFailed();
						return SAUNAFS_ERROR_IO;
					}
				}
				HddStats::overheadWrite(blockSize);
			}
		} else { // misaligned shrink
			for (block = 0; block < blocks - 1; block++) {
				{
					DiskReadStatsUpdater updater(origDisk, blockSize);

					retSize = origDisk->preadData(originalChunk, blockBuffer,
					                              blockSize,
					                              block * SFSBLOCKSIZE);

					if (retSize != blockSize) {
						hddAddErrorAndPreserveErrno(originalChunk);
						safs_silent_errlog(LOG_WARNING,
						    "duptrunc: file:%s - data read error",
						    originalChunk->metaFilename().c_str());
						hddIOEnd(dupChunk);
						dupDisk->unlinkChunk(dupChunk);
						hddDeleteChunkFromRegistry(dupChunk);
						hddIOEnd(originalChunk);
						hddReportDamagedChunk(chunkId, chunkType);
						hddChunkRelease(originalChunk);
						updater.markReadAsFailed();
						return SAUNAFS_ERROR_IO;
					}
				}
				HddStats::overheadRead(blockSize);

				{
					DiskWriteStatsUpdater updater(dupDisk, blockSize);

					if (dupDisk->isZonedDevice()) {
						const uint8_t *dupCrcBlock = crcDataOriginal + kCrcSize * block;
						uint32_t crc = get32bit(&dupCrcBlock);

						if (dupDisk->writeChunkBlock(
						        dupChunk, dupChunk->version(), block, 0,
						        SFSBLOCKSIZE, crc, crcDataDup,
						        blockBuffer) == SAUNAFS_STATUS_OK) {
							retSize = SFSBLOCKSIZE;
						} else {
							retSize = SAUNAFS_ERROR_IO;
						}
					} else {
						retSize = dupDisk->writeChunkData(dupChunk, blockBuffer,
						                                  blockSize, 0);
					}

					if (retSize != blockSize) {
						hddAddErrorAndPreserveErrno(dupChunk);
						safs_silent_errlog(LOG_WARNING,
						    "duptrunc: file:%s - data write error",
						    dupChunk->metaFilename().c_str());
						hddIOEnd(dupChunk);
						dupDisk->unlinkChunk(dupChunk);
						hddDeleteChunkFromRegistry(dupChunk);
						hddIOEnd(originalChunk);
						hddChunkRelease(originalChunk);
						updater.markWriteAsFailed();
						return SAUNAFS_ERROR_IO;        //write error
					}
				}
				HddStats::overheadWrite(blockSize);
			}

			block = blocks - 1;
			auto toBeRead = lastBlockSize;

			{
				DiskReadStatsUpdater updater(origDisk, SFSBLOCKSIZE);

				retSize = origDisk->preadData(originalChunk, blockBuffer,
				                              SFSBLOCKSIZE,
				                              block * SFSBLOCKSIZE);

				if (retSize != (signed)SFSBLOCKSIZE) {
					hddAddErrorAndPreserveErrno(originalChunk);
					safs_silent_errlog(LOG_WARNING,
					    "duptrunc: file:%s - data read error",
					    originalChunk->metaFilename().c_str());
					hddIOEnd(dupChunk);
					dupDisk->unlinkChunk(dupChunk);
					hddDeleteChunkFromRegistry(dupChunk);
					hddIOEnd(originalChunk);
					hddReportDamagedChunk(chunkId, chunkType);
					hddChunkRelease(originalChunk);
					updater.markReadAsFailed();
					return SAUNAFS_ERROR_IO;
				}
			}
			HddStats::overheadRead(SFSBLOCKSIZE);

			auto* ptr = headerBuffer + dupChunk->getCrcOffset()
			            + kCrcSize * block;
			auto crc = mycrc32_zeroexpanded(0, blockBuffer, lastBlockSize,
			                                SFSBLOCKSIZE - lastBlockSize);
			put32bit(&ptr, crc);

			// Fill with zeros the remaining part of the block
			memset(blockBuffer + toBeRead, 0, SFSBLOCKSIZE - lastBlockSize);

			{
				DiskWriteStatsUpdater updater(dupDisk, blockSize);

				if (dupDisk->isZonedDevice()) {

					if (dupDisk->writeChunkBlock(dupChunk, dupChunk->version(),
					                             block, 0, SFSBLOCKSIZE, crc,
					                             crcDataDup, blockBuffer) ==
					    SAUNAFS_STATUS_OK) {
						retSize = SFSBLOCKSIZE;
					} else {
						retSize = 0;
					}
				} else {
					retSize = dupDisk->writeChunkData(dupChunk, blockBuffer,
					                                  blockSize, 0);
				}

				if (retSize != blockSize) {
					hddAddErrorAndPreserveErrno(dupChunk);
					safs_silent_errlog(LOG_WARNING,
					    "duptrunc: file:%s - data write error",
					    dupChunk->metaFilename().c_str());
					hddIOEnd(dupChunk);
					dupDisk->unlinkChunk(dupChunk);
					hddDeleteChunkFromRegistry(dupChunk);
					hddIOEnd(originalChunk);
					hddChunkRelease(originalChunk);
					updater.markWriteAsFailed();
					return SAUNAFS_ERROR_IO;
				}
			}
			HddStats::overheadWrite(blockSize);
		}
	}

	if (dupChunk) {
		memcpy(crcDataDup, headerBuffer + dupChunk->getCrcOffset(),
		       dupChunk->getCrcBlockSize());

		dupDisk->lseekMetadata(dupChunk, 0, SEEK_SET);

		{
			DiskWriteStatsUpdater updater(dupDisk,
			                              dupChunk->getHeaderSize());

			if (dupDisk->writeChunkHeader(dupChunk) != SAUNAFS_STATUS_OK) {
				hddAddErrorAndPreserveErrno(dupChunk);
				safs_silent_errlog(LOG_WARNING,
				                   "duptrunc: file:%s - hdr write error",
				                   dupChunk->metaFilename().c_str());
				hddIOEnd(dupChunk);
				dupDisk->unlinkChunk(dupChunk);
				hddDeleteChunkFromRegistry(dupChunk);
				hddIOEnd(originalChunk);
				hddChunkRelease(originalChunk);
				updater.markWriteAsFailed();
				return SAUNAFS_ERROR_IO;
			}
		}
		HddStats::overheadWrite(dupChunk->getHeaderSize());
	}

	status = hddIOEnd(originalChunk);
	if (status != SAUNAFS_STATUS_OK) {
		hddAddErrorAndPreserveErrno(originalChunk);
		hddIOEnd(dupChunk);
		dupDisk->unlinkChunk(dupChunk);
		hddDeleteChunkFromRegistry(dupChunk);
		hddReportDamagedChunk(chunkId, chunkType);
		hddChunkRelease(originalChunk);
		return status;
	}

	status = hddIOEnd(dupChunk);
	if (status != SAUNAFS_STATUS_OK) {
		hddAddErrorAndPreserveErrno(dupChunk);
		dupDisk->unlinkChunk(dupChunk);
		hddDeleteChunkFromRegistry(dupChunk);
		hddChunkRelease(originalChunk);
		return status;
	}

	dupDisk->setChunkBlocks(dupChunk, originalChunk->blocks(), blocks);
	dupDisk->setNeedRefresh(true);

	hddChunkRelease(dupChunk);
	hddChunkRelease(originalChunk);

	return SAUNAFS_STATUS_OK;
}

int hddInternalDelete(IChunk *chunk, uint32_t version) {
	TRACETHIS();
	assert(chunk);
	if (chunk->version() != version && version > 0) {
		hddChunkRelease(chunk);
		return SAUNAFS_ERROR_WRONGVERSION;
	}
	if (chunk->owner()->unlinkChunk(chunk) < 0) {
		uint8_t err = errno;
		hddAddErrorAndPreserveErrno(chunk);
		safs_silent_errlog(LOG_WARNING,
		                   "hddInternalDelete: file: %s - unlink error",
		                   chunk->metaFilename().c_str());
		if (err == ENOENT) {
			hddDeleteChunkFromRegistry(chunk);
		} else {
			hddChunkRelease(chunk);
		}
		return SAUNAFS_ERROR_IO;
	}

	hddDeleteChunkFromRegistry(chunk);

	return SAUNAFS_STATUS_OK;
}

int hddInternalDelete(uint64_t chunkId, uint32_t version,
                      ChunkPartType chunkType) {
	TRACETHIS();

	HddStats::gStatsOperationsDelete++;

	auto *chunk = hddChunkFindAndLock(chunkId, chunkType);
	if (chunk == ChunkNotFound) {
		return SAUNAFS_ERROR_NOCHUNK;
	}

	return hddInternalDelete(chunk, version);
}

/* all chunk operations in one call */
// newversion>0 && length==0xFFFFFFFF && copychunkid==0    -> change version
// newversion>0 && length==0xFFFFFFFF && copycnunkid>0     -> duplicate
// newversion>0 && length<=SFSCHUNKSIZE && copychunkid==0  -> truncate
// newversion>0 && length<=SFSCHUNKSIZE && copychunkid>0   -> dup and truncate
// newversion==0 && length==0                             -> delete
// newversion==0 && length==1                             -> create
// newversion==0 && length==2                             -> check chunk content
int hddChunkOperation(uint64_t chunkId, uint32_t chunkVersion,
                      ChunkPartType chunkType, uint32_t chunkNewVersion,
                      uint64_t chunkIdCopy, uint32_t chunkVersionCopy,
                      uint32_t length) {
	TRACETHIS();

	if (chunkNewVersion > 0) {
		if (length == 0xFFFFFFFF) {
			if (chunkIdCopy == 0) {
				return hddInternalUpdateVersion(chunkId, chunkVersion,
				                                chunkNewVersion, chunkType);
			} else {
				return hddInternalDuplicate(chunkId, chunkVersion,
				                            chunkNewVersion, chunkType,
				                            chunkIdCopy, chunkVersionCopy);
			}
		} else if (length <= SFSCHUNKSIZE) {
			if (chunkIdCopy == 0) {
				return hddInternalTruncate(chunkId, chunkType, chunkVersion,
				                           chunkNewVersion, length);
			} else {
				return hddInternalDuplicateTruncate(
				    chunkId, chunkVersion, chunkNewVersion, chunkType,
				    chunkIdCopy, chunkVersionCopy, length);
			}
		} else {
			return SAUNAFS_ERROR_EINVAL;
		}
	} else {
		if (length == 0) {
			return hddInternalDelete(chunkId, chunkVersion, chunkType);
		} else if (length == 1) {
			return hddInternalCreate(chunkId, chunkVersion, chunkType);
		} else if (length == 2) {
			return hddInternalTestChunk(chunkId, chunkVersion, chunkType);
		} else {
			return SAUNAFS_ERROR_EINVAL;
		}
	}
}

static UniqueQueue<ChunkWithVersionAndType> gTestChunkQueue;

static void hddTestChunkThread() {
	pthread_setname_np(pthread_self(), "testChunkThread");

	bool terminate = false;

	while (!terminate) {
		Timeout time(std::chrono::seconds(1));

		try {
			ChunkWithVersionAndType chunk = gTestChunkQueue.get();
			std::string name = chunk.toString();
			if (hddInternalTestChunk(chunk.id, chunk.version, chunk.type) !=
			    SAUNAFS_STATUS_OK) {
				safs_pretty_syslog(LOG_NOTICE,
				                   "Chunk %s corrupted (detected by a client)",
				                   name.c_str());
				hddReportDamagedChunk(chunk.id, chunk.type);
			} else {
				safs_pretty_syslog(LOG_NOTICE,
				                   "Chunk %s spuriously reported as corrupted",
				                   name.c_str());
			}
		} catch (UniqueQueueEmptyException &) {
			// hooray, nothing to do
		}

		// rate-limit to 1/sec
		usleep(time.remaining_us());
		terminate = gTerminate;
	};
}

void hddAddChunkToTestQueue(ChunkWithVersionAndType chunk) {
	gTestChunkQueue.put(chunk);
}

void hddTesterThread() {
	TRACETHIS();

	pthread_setname_np(pthread_self(), "testerThread");

	IChunk *chunk;
	uint64_t chunkId;
	uint32_t version;
	ChunkPartType chunkType = slice_traits::standard::ChunkPartType();
	uint32_t cnt = 0;
	uint64_t startMicroSecs, endMicroSecs;

	auto disksIt = gDisks.begin();
	auto previousDiskIt = disksIt;

	while (!gTerminate) {
		startMicroSecs = getMicroSecsTime();
		chunkId = 0;
		version = 0;

		{
			std::scoped_lock lock(gDisksMutex, gChunksMapMutex, gTestsMutex);

			bool testerResetExpected = true;
			if (gResetTester.compare_exchange_strong(testerResetExpected,
			                                         false)) {
				disksIt = gDisks.begin();
				cnt = 0;
			}

			cnt += std::min(gHDDTestFreq_ms.load(), 1000U);

			if (cnt < gHDDTestFreq_ms || gDiskActions == 0 ||
			    disksIt == gDisks.end()) {
				chunkId = 0;
			} else {
				cnt = 0;
				previousDiskIt = disksIt;

				if (gDisks.size()) {
					do {
						++disksIt;
						if (disksIt == gDisks.end()) {
							disksIt = gDisks.begin();
						}
					} while (
					    ((*disksIt)->isDamaged() ||
					     (*disksIt)->isMarkedForDeletion() ||
					     (*disksIt)->wasRemovedFromConfig() ||
					     (*disksIt)->scanState() != IDisk::ScanState::kWorking) &&
					    previousDiskIt != disksIt);
				}

				if (previousDiskIt == disksIt
				    && ((*disksIt)->isDamaged()
				        || (*disksIt)->isMarkedForDeletion()
				        || (*disksIt)->wasRemovedFromConfig()
				        || (*disksIt)->scanState()
				               != IDisk::ScanState::kWorking)) {
					chunkId = 0;
				} else {
					chunk = (*disksIt)->chunks().chunkToTest();

					if (chunk && chunk->state() == ChunkState::Available) {
						chunkId = chunk->id();
						version = chunk->version();
						chunkType = chunk->type();
					}
				}
			}
		}

		if (chunkId > 0 && hddInternalTestChunk(chunkId, version, chunkType) !=
		                       SAUNAFS_STATUS_OK) {
			hddReportDamagedChunk(chunkId, chunkType);
		}

		endMicroSecs = getMicroSecsTime();

		if (endMicroSecs > startMicroSecs) {
			unsigned usToSleep = 1000 * std::min(gHDDTestFreq_ms.load(), 1000U);
			endMicroSecs -= startMicroSecs;

			if (endMicroSecs < usToSleep) {
				usleep(usToSleep - endMicroSecs);
			}
		}
	}
}

void hddDiskRandomizeChunksForTests(IDisk *disk) {
	TRACETHIS();

	std::lock_guard testsLockGuard(gTestsMutex);
	safs_pretty_syslog(LOG_NOTICE, "Randomizing chunks for disk: %s",
	                   disk->getPaths().c_str());
	disk->chunks().shuffle();
}

/* initialization */

static inline void hddAddChunkFromDiskScan(IDisk *disk,
                                           const std::string &fullname,
                                           uint64_t chunkId, uint32_t version,
                                           ChunkPartType chunkType) {
	TRACETHIS();

	auto *chunk = hddChunkFindOrCreatePlusLock(
	    disk, chunkId, chunkType, disk::ChunkGetMode::kFindOrCreate);

	if (chunk == ChunkNotFound) {
		safs_pretty_syslog(LOG_ERR, "Can't use file %s as chunk",
		                   fullname.c_str());
		return;
	}

	bool isNewChunk = chunk->metaFilename().empty();

	if (!isNewChunk) {
		// already have this chunk
		if (version <= chunk->version()) {
			// current chunk is older
			if (!disk->isReadOnly()) {
				unlink(fullname.c_str());
			}
			hddChunkRelease(chunk);
			return;
		}

		if (!disk->isReadOnly()) {
			chunk->owner()->unlinkChunk(chunk);
		}
	}

	if (!isNewChunk) {
		std::lock_guard chunksMapLockGuard(gChunksMapMutex);
		chunk = hddRecreateChunk(disk, chunk, chunkId, chunkType);
	}

	chunk->setVersion(version);
	chunk->updateFilenamesFromVersion(version);
	sassert(chunk->metaFilename() == fullname);

	{
		disk->updateChunkAttributes(chunk, true);
		chunk->setValidAttr(0);
	}

	{
		std::lock_guard testsLockGuard(gTestsMutex);
		disk->chunks().insert(chunk);
	}

	if (isNewChunk) {
		hddReportNewChunkToMaster(chunk->id(), chunk->version(),
		                          chunk->owner()->isMarkedForDeletion(),
		                          chunk->type());
	}

	hddChunkRelease(chunk);
}

/// Scans the Disk for new Chunks in bulks of 1000 Chunks
void hddDiskScan(IDisk *disk, uint32_t beginTime) {
	std::unique_lock uniqueLock(gDisksMutex);
	IDisk::ScanState scanState = disk->scanState();
	uniqueLock.unlock();

	if (scanState == IDisk::ScanState::kTerminate) {
		return;
	}

	DIR *dd;
	struct dirent *dirEntry;
	uint32_t totalCheckCount = 0;
	uint8_t lastPercent = 0, currentPercent = 0;
	bool terminateScan = false;
	uint32_t lastTime = time(nullptr), currentTime;

	for (unsigned subfolderNumber = 0;
	     subfolderNumber < Subfolder::kNumberOfSubfolders && !terminateScan;
	     ++subfolderNumber) {
		std::string subfolderPath = disk->metaPath()
		    + Subfolder::getSubfolderNameGivenNumber(subfolderNumber) + "/";
		dd = opendir(subfolderPath.c_str());
		if (!dd) {
			continue;
		}

		while (!terminateScan) {
			dirEntry = readdir(dd);
			if (!dirEntry) {
				break;
			}

			const std::string filename = dirEntry->d_name;
			ChunkFilenameParser filenameParser(filename);

			if (filenameParser.parse() != ChunkFilenameParser::Status::OK) {
				if (filename != "." && filename != ".." &&
				    filename.find(CHUNK_DATA_FILE_EXTENSION) ==
				        std::string::npos) {
					safs_pretty_syslog(LOG_WARNING,
					                   "Invalid file %s placed in chunks "
					                   "directory %s; skipping it.",
					                   dirEntry->d_name, subfolderPath.c_str());
				}
				continue;
			}

			if (Subfolder::getSubfolderNumber(filenameParser.chunkId()) !=
			    subfolderNumber) {
				safs_pretty_syslog(LOG_WARNING,
				    "Chunk %s%s placed in a wrong directory; skipping it.",
				    subfolderPath.c_str(), dirEntry->d_name);
				continue;
			}

			std::string chunkName = dirEntry->d_name;

			if(chunkName.empty()) {
				continue;
			}

			hddAddChunkFromDiskScan(
			    disk, subfolderPath + chunkName, filenameParser.chunkId(),
			    filenameParser.chunkVersion(), filenameParser.chunkType());

			totalCheckCount++;

			if (totalCheckCount >= 1000) {
				uniqueLock.lock();

				if (disk->scanState() == IDisk::ScanState::kTerminate) {
					terminateScan = true;
				}

				uniqueLock.unlock();

				totalCheckCount = 0;
			}
		}

		closedir(dd);

		currentTime = time(nullptr);

		static constexpr float kMaxSubfolderFloat = 256.0f;
		currentPercent = (subfolderNumber * 100.0) / kMaxSubfolderFloat;

		if (currentPercent > lastPercent && currentTime > lastTime) {
			lastPercent = currentPercent;
			lastTime = currentTime;

			uniqueLock.lock();
			disk->setScanProgress(currentPercent);
			uniqueLock.unlock();

			gHddSpaceChanged = true;  // report chunk count to master

			safs_pretty_syslog(
			    LOG_NOTICE, "scanning disk %s: %" PRIu8 "%% (%" PRIu32 "s)",
			    disk->getPaths().c_str(), lastPercent, currentTime - beginTime);
		}
	}

	if (disk->isZonedDevice()) {
		// Check for dirty zones and update conventional zones' write head
		disk->updateAfterScan();
	}
}

void hddDiskScanThread(IDisk *disk) {
	TRACETHIS();
	uint32_t beginTime = static_cast<uint32_t>(time(nullptr));

	gScansInProgress++;

	{
		std::lock_guard disksLockGuard(gDisksMutex);
		disk->refreshDataDiskUsage();
	}

	gHddSpaceChanged = true;

	hddDiskScan(disk, beginTime);
	hddDiskRandomizeChunksForTests(disk);
	gScansInProgress--;

	std::lock_guard disksLockGuard(gDisksMutex);

	if (disk->scanState() == IDisk::ScanState::kTerminate) {
		safs_pretty_syslog(LOG_NOTICE, "scanning disk %s: interrupted",
		                   disk->getPaths().c_str());
	} else {
		safs_pretty_syslog(LOG_NOTICE,
		                   "scanning disk %s: complete (%" PRIu32 "s)",
		                   disk->getPaths().c_str(),
		                   static_cast<uint32_t>(time(nullptr)) - beginTime);
	}

	disk->setScanState(IDisk::ScanState::kThreadFinished);
	disk->setScanProgress(100);
}

void hddDisksThread() {
	TRACETHIS();

	pthread_setname_np(pthread_self(), "disksThread");

	while (!gTerminate) {
		hddCheckDisks();
		sleep(1);
	}
}

void hddFreeResourcesThread() {
	static const int kDelayedStep = 2;
	static const int kMaxFreeUnused = 1024;
	TRACETHIS();

	pthread_setname_np(pthread_self(), "freeResThread");

	while (!gTerminate) {
		gOpenChunks.freeUnused(eventloop_time(), gChunksMapMutex,
		                       kMaxFreeUnused);
		sleep(kDelayedStep);
	}
}

void hddTerminate(void) {
	TRACETHIS();

	// if gTerminate is true here, then it means that threads have not been
	// started, so do not join with them
	uint32_t terminate = gTerminate.exchange(true);

	if (terminate == 0) {
		gTesterThread.join();
		gDisksThread.join();
		gDelayedThread.join();

		try {
			gChunkTesterThread.join();
		} catch (std::system_error &e) {
			safs_pretty_syslog(
			    LOG_NOTICE, "Failed to join test chunk thread: %s", e.what());
		}
	}

	{
		std::lock_guard disksLockGuard(gDisksMutex);
		terminate = 0;

		for (auto &disk : gDisks) {
			if (disk->scanState() == IDisk::ScanState::kInProgress) {
				disk->setScanState(IDisk::ScanState::kTerminate);
			}
			if (disk->scanState() == IDisk::ScanState::kTerminate
			    || disk->scanState() == IDisk::ScanState::kThreadFinished) {
				terminate++;
			}
		}
	}

	while (terminate > 0) {
		usleep(10000); // not very elegant solution.

		std::lock_guard disksLockGuard(gDisksMutex);

		for (auto &disk : gDisks) {
			if (disk->scanState() == IDisk::ScanState::kThreadFinished) {
				disk->scanThread().join();
				// any state - to prevent calling join again
				disk->setScanState(IDisk::ScanState::kWorking);
				terminate--;
			}
		}
	}

	for (auto &chunkEntry : gChunksMap) {
		IChunk *chunk = chunkEntry.second.get();

		if (chunk->state() == ChunkState::Available) {
			if (chunk->wasChanged()) {
				safs_pretty_syslog(LOG_WARNING, "hddTerminate: CRC not flushed "
				                   "- writing now");

				if (chunkWriteCrc(chunk) != SAUNAFS_STATUS_OK) {
					safs_silent_errlog(LOG_WARNING,
					                   "hddTerminate: file: %s - write error",
					                   chunk->metaFilename().c_str());
				}
			}
			gOpenChunks.purge(chunk->metaFD());
		} else {
			safs::log_warn("hddTerminate: locked chunk !!! (chunkid: {:#04x}, "
			               "chunktype: {})",
			               chunk->id(),
			               chunk->type().toString());
		}
	}

	// Delete chunks even not in AVAILABLE state here, as all threads using
	// chunk objects should already be joined (by this function and other
	// cleanup functions of other chunkserver modules that are registered on
	// eventloop termination) This function should always be executed after all
	// other chunkserver modules' (that use chunk objects) cleanup functions
	// were executed.
	gChunksMap.clear();
	gOpenChunks.freeUnused(eventloop_time(), gChunksMapMutex);
	gDisks.clear();
}

void hddReload(void) {
	TRACETHIS();

	gAdviseNoCache = cfg_getuint32("HDD_ADVISE_NO_CACHE", 0);
	gPerformFsync = cfg_getuint32("PERFORM_FSYNC", 1);
	gHDDTestFreq_ms =
	    cfg_ranged_get("HDD_TEST_FREQ", 10., 0.001, 1000000.) * 1000;
	gCheckCrcWhenReading = cfg_getuint8("HDD_CHECK_CRC_WHEN_READING", 1) != 0U;
	gCheckCrcWhenWriting = cfg_getuint8("HDD_CHECK_CRC_WHEN_WRITING", 1) != 0U;
	gPunchHolesInFiles = cfg_getuint32("HDD_PUNCH_HOLES", 0);

	char *leaveFreeStr = cfg_getstr("HDD_LEAVE_SPACE_DEFAULT",
	                                disk::gLeaveSpaceDefaultDefaultStrValue);
	auto parsedLeaveFree = cfg_parse_size(leaveFreeStr);

	if (parsedLeaveFree < 0) {
		safs_pretty_syslog(LOG_NOTICE,
		                   "hdd space manager: HDD_LEAVE_SPACE_DEFAULT parse "
		                   "error - left unchanged");
	} else {
		disk::gLeaveFree = parsedLeaveFree;
	}

	free(leaveFreeStr);

	if (disk::gLeaveFree < static_cast<int64_t>(SFSCHUNKSIZE)) {
		safs_pretty_syslog(LOG_NOTICE,
		    "hdd space manager: HDD_LEAVE_SPACE_DEFAULT < chunk size - leaving "
		    "so small space on hdd is not recommended");
	}

	safs_pretty_syslog(LOG_NOTICE,"reloading hdd data ...");

	try {
		gDiskManager->reloadConfiguration();
		gDiskManager->reloadDisksFromCfg();
	} catch (const Exception& ex) {
		safs_pretty_syslog(LOG_ERR, "%s", ex.what());
	}
}

int hddLateInit() {
	TRACETHIS();
	gTerminate = false;
	gTesterThread = std::thread(hddTesterThread);
	gDisksThread = std::thread(hddDisksThread);
	gDelayedThread = std::thread(hddFreeResourcesThread);

	try {
		gChunkTesterThread = std::thread(hddTestChunkThread);
	} catch (std::system_error &e) {
		safs_pretty_syslog(LOG_ERR, "Failed to create test chunk thread: %s",
		                   e.what());
		abort();
	}

	return 0;
}

/// Initializes the default disk manager and reads the disk manager type from
/// configuration. This function must be called before plugins initialization.
int initDiskManager() {
	// Initialize the default disk manager and set the disk manager type.
	// The default will be overwritten if the DISK_MANAGER_TYPE is set in
	// the configuration file.
	gDiskManager = std::make_unique<DefaultDiskManager>();

	std::string diskManagerType = cfg_get("DISK_MANAGER_TYPE", "default");
	std::transform(diskManagerType.begin(), diskManagerType.end(),
	               diskManagerType.begin(), ::tolower);
	gDiskManagerType = std::move(diskManagerType);

	return SAUNAFS_STATUS_OK;
}

int loadPlugins() {
	std::string pluginsInstallDirPath = PLUGINS_PATH "/chunkserver";
	std::string pluginsBuildDirPath = BUILD_PATH "/plugins/chunkserver";

	// Try to load plugins first from the installation directory
	if (!pluginManager.loadPlugins(pluginsInstallDirPath)) {
		safs_pretty_syslog(LOG_NOTICE,
		                   "PluginManager: No plugins loaded from: %s",
		                   pluginsInstallDirPath.c_str());

		// If no plugins were loaded from the installation directory,
		// try to load them from the build directory (useful for development)
		if (!pluginManager.loadPlugins(pluginsBuildDirPath)) {
			safs_pretty_syslog(LOG_NOTICE,
			                   "PluginManager: No plugins loaded from: %s",
			                   pluginsBuildDirPath.c_str());
		}
	}

	pluginManager.showLoadedPlugins();

	return SAUNAFS_STATUS_OK;
}

int hddInit() {
	TRACETHIS();

	initializeEmptyBlockCrcForDisks();

	gPerformFsync = cfg_getuint32("PERFORM_FSYNC", 1);

	int64_t leaveSpaceDefaultDefaultValue =
	    cfg_parse_size(disk::gLeaveSpaceDefaultDefaultStrValue);
	sassert(leaveSpaceDefaultDefaultValue > 0);

	char *leaveFreeStr = cfg_getstr("HDD_LEAVE_SPACE_DEFAULT",
	                                disk::gLeaveSpaceDefaultDefaultStrValue);
	auto parsedLeaveFree = cfg_parse_size(leaveFreeStr);

	if (parsedLeaveFree < 0) {
		safs_pretty_syslog(
		    LOG_WARNING,
		    "%s: HDD_LEAVE_SPACE_DEFAULT parse error - using default (%s)",
		    cfg_filename().c_str(), disk::gLeaveSpaceDefaultDefaultStrValue);
		disk::gLeaveFree = leaveSpaceDefaultDefaultValue;
	} else {
		disk::gLeaveFree = parsedLeaveFree;
	}

	free(leaveFreeStr);

	if (disk::gLeaveFree < static_cast<int64_t>(SFSCHUNKSIZE)) {
		safs_pretty_syslog(LOG_WARNING,
		                   "%s: HDD_LEAVE_SPACE_DEFAULT < chunk size - "
		                   "leaving so small space on hdd is not recommended",
		                   cfg_filename().c_str());
	}

	try {
		gDiskManager->reloadConfiguration();
		gDiskManager->reloadDisksFromCfg();
	} catch (const Exception& ex) {
		safs_pretty_syslog(LOG_ERR, "%s", ex.what());
	}

	{
		std::lock_guard disksLockGuard(gDisksMutex);
		for (const auto& disk : gDisks) {
			safs_pretty_syslog(LOG_INFO, "hdd space manager: disk to scan: %s",
			                   disk->getPaths().c_str());
		}
	}

	safs_pretty_syslog(LOG_INFO,
	                   "hdd space manager: start background hdd scanning "
	                   "(searching for available chunks)");

	gAdviseNoCache = cfg_getuint32("HDD_ADVISE_NO_CACHE", 0);
	gHDDTestFreq_ms =
	    cfg_ranged_get("HDD_TEST_FREQ", 10., 0.001, 1000000.) * 1000;
	gCheckCrcWhenReading = cfg_getuint8("HDD_CHECK_CRC_WHEN_READING", 1) != 0U;
	gCheckCrcWhenWriting = cfg_getuint8("HDD_CHECK_CRC_WHEN_WRITING", 1) != 0U;

	gPunchHolesInFiles = cfg_getuint32("HDD_PUNCH_HOLES", 0);

	eventloop_reloadregister(hddReload);
	eventloop_timeregister(TIMEMODE_RUN_LATE, SECONDS_IN_ONE_MINUTE, 0,
	                       hddDiskInfoRotateStats);
	eventloop_destructregister(hddTerminate);

	gTerminate = true;

	return 0;
}
