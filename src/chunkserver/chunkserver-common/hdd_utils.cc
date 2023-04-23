#include "hdd_utils.h"

#include <sys/time.h>

#include "chunkserver-common/global_shared_resources.h"
#include "devtools/TracePrinter.h"

void hddAddErrorAndPreserveErrno(IChunk *chunk) {
	TRACETHIS();
	assert(chunk);
	uint32_t index;
	struct timeval tv;
	int errmem = errno;

	{
		std::lock_guard disksLockGuard(gDisksMutex);

		gettimeofday(&tv, nullptr);

		auto *disk = chunk->owner();
		index = disk->lastErrorIndex();
		disk->lastErrorTab()[index].chunkid = chunk->id();
		disk->lastErrorTab()[index].errornumber = errmem;
		disk->lastErrorTab()[index].timestamp = tv.tv_sec;
		index = (index + 1) % disk::kLastErrorSize;
		disk->setLastErrorIndex(index);
	}

	++gErrorCounter;

	errno = errmem;
}

void hddReportDamagedChunk(uint64_t chunkId, ChunkPartType chunkType) {
	TRACETHIS1(chunkId);
	std::lock_guard lockGuard(gMasterReportsLock);
	gDamagedChunks.push_back({chunkId, chunkType});
}

bool hddChunkTryLock(IChunk *chunk) {
	assert(gChunksMapMutex.try_lock() == false);
	assert(chunk);
	TRACETHIS1(chunk->chunkid);
	bool ret = false;

	if (chunk != nullptr && chunk->state() == ChunkState::Available) {
		chunk->setState(ChunkState::Locked);
		ret = true;
	}

	return ret;
}

void hddChunkRelease(IChunk *chunk) {
	TRACETHIS();
	assert(chunk);

	std::unique_lock chunksMapUniqueLock(gChunksMapMutex);

	if (chunk->state() == ChunkState::Locked) {
		chunk->setState(ChunkState::Available);
		if (chunk->condVar()) {
			chunksMapUniqueLock.unlock();
			chunk->condVar()->condVar.notify_one();
		}
	} else if (chunk->state() == ChunkState::ToBeDeleted) {
		if (chunk->condVar()) {
			chunk->setState(ChunkState::Deleted);
			chunksMapUniqueLock.unlock();
			chunk->condVar()->condVar.notify_one();
		} else {
			hddRemoveChunkFromContainers(chunk);
		}
	}
}

void hddRemoveChunkFromContainers(IChunk *chunk) {
	TRACETHIS();
	assert(chunk);

	auto chunkIter = gChunksMap.find(chunkToKey(*chunk));

	if (chunkIter == gChunksMap.end()) {
		safs::log_warn(
		    "Chunk to be removed wasn't found on the chunkserver. "
		    "(chunkid: {:#04x}, chunktype: {})",
		    chunk->id(), chunk->type().toString());
		return;
	}

	const auto *chunkSmartPointer = chunkIter->second.get();
	gOpenChunks.purge(chunkSmartPointer->metaFD());

	auto *disk = chunkSmartPointer->owner();

	if (disk != nullptr) {
		// remove this chunk from its disk's testlist
		const std::lock_guard testsLockGuard(gTestsMutex);
		disk->chunks().remove(chunk);
		disk->setNeedRefresh(true);
	}

	gChunksMap.erase(chunkIter);
}
