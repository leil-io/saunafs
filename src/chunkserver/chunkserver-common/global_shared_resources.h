#pragma once

#include "chunkserver-common/chunk_map.h"
#include "chunkserver-common/disk_interface.h"
#include "chunkserver-common/indexed_resource_pool.h"
#include "chunkserver-common/open_chunk.h"

inline IndexedResourcePool<OpenChunk> gOpenChunks;

/// Protects access to the list of chunks of every Disk. This list contains the
/// chunks to be tested.
inline std::mutex gTestsMutex;

/// Global unordered_map of all chunks stored in this chunkserver.
inline ChunkMap gChunksMap;

/// Only guards access to gChunksMap.
/// Chunk objects stored in the registry have their own separate locks.
inline std::mutex gChunksMapMutex;

inline const int kOpenRetryCount = 4;
inline const int kOpenRetry_ms = 5;

inline bool gPunchHolesInFiles;

/// The collection of data Disks (directories where chunks are stored).
/// Protected by gDisksMutex.
inline std::vector<std::unique_ptr<IDisk>> gDisks;

/// Protects gDisks + all data in structures (except Disk::cstat)
inline std::mutex gDisksMutex;

/// Container to reuse free condition variables (guarded by `gChunksMapMutex`)
inline std::vector<std::unique_ptr<CondVarWithWaitCount>> gFreeCondVars;

/// Active Disks scans in progress.
/// Note: theoretically it would return a false positive if scans haven't
/// started yet, but it's a _very_ unlikely situation.
inline std::atomic_int gScansInProgress(0);

inline std::atomic_bool gPerformFsync(true);

inline std::atomic_bool gCheckCrcWhenWriting{true};
