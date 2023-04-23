#pragma once

#include <deque>

#include "chunkserver-common/chunk_interface.h"
#include "common/chunk_with_version_and_type.h"
#include "protocol/chunks_with_type.h"

// master reports
inline std::deque<ChunkWithType> gDamagedChunks;
inline std::deque<ChunkWithType> gLostChunks;
inline std::deque<ChunkWithVersionAndType> gNewChunks;
inline std::atomic<uint32_t> gErrorCounter = 0;
inline std::atomic_bool gHddSpaceChanged = false;

inline std::thread gDisksThread, gDelayedThread, gTesterThread;
inline std::thread gChunkTesterThread;

inline std::atomic_bool gTerminate = false;
inline uint8_t gDiskActions = 0;  // no need for atomic; guarded by gDisksMutex
inline std::atomic_bool gResetTester = false;

// master reports = damaged chunks, lost chunks, new chunks
inline std::mutex gMasterReportsLock;

/// Avoid deadlock by waiting for specified time
static constexpr uint8_t kSecondsToWaitForLockedChunk_ = 2;

/// Adds the error to the Disk owner of chunk in a thread sage way and preserves
/// the errno to be used in later checks.
void hddAddErrorAndPreserveErrno(IChunk *chunk);

/// Adds the chunk to the damaged chunks container.
/// The damaged chunks are reported periodically to master in the event loop.
void hddReportDamagedChunk(uint64_t chunkId, ChunkPartType chunkType);

bool hddChunkTryLock(IChunk *chunk);

/// Removes the Chunk from the registry and from the disk's testlist.
void hddRemoveChunkFromContainers(IChunk *chunk);

/// Sets the chunk's state to Available or Deleted and notifies other waiting
/// threads. If the chunk was deleted, and there are no threads waiting, it also
/// removes it from the registry and from the disk's testlist.
void hddChunkRelease(IChunk *chunk);
