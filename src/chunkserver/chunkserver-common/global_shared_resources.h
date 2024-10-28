#pragma once

#include "chunkserver-common/chunk_map.h"
#include "chunkserver-common/disk_interface.h"
#include "chunkserver-common/disk_manager_interface.h"
#include "chunkserver-common/indexed_resource_pool.h"
#include "chunkserver-common/iostat.h"
#include "chunkserver-common/open_chunk.h"
#include "chunkserver-common/plugin_manager.h"

inline IndexedResourcePool<OpenChunk> gOpenChunks;

/// Protects access to the list of chunks of every Disk. This list contains the
/// chunks to be tested.
inline std::mutex gTestsMutex;

/// Maximum frequency for chunk testing in milliseconds.
constexpr uint32_t kMaxTestFreqMs = 1000U;

/// Frequency for chunk testing in milliseconds.
/// Can be changed via the configuration file (HDD_TEST_FREQ).
inline std::atomic<unsigned> gHDDTestFreq_ms(10 * 1000);

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

/// Configuration variable to define the type of DiskManager to be used.
/// The default is an instance of DefaultDiskManager, but can be be changed from
/// plugins offering custom implementations of IDiskManager.
inline std::string gDiskManagerType = "default";

/// The DiskManager instance to be used by the Chunkserver.
inline std::unique_ptr<IDiskManager> gDiskManager;

/// Container to reuse free condition variables (guarded by `gChunksMapMutex`)
inline std::vector<std::unique_ptr<CondVarWithWaitCount>> gFreeCondVars;

/// Active Disks scans in progress.
/// Note: theoretically it would return a false positive if scans haven't
/// started yet, but it's a _very_ unlikely situation.
inline std::atomic_int gScansInProgress(0);

inline std::atomic_bool gPerformFsync(true);

inline std::atomic_bool gCheckCrcWhenWriting{true};

/// Value of HDD_ADVISE_NO_CACHE from config
inline std::atomic_bool gAdviseNoCache = false;

inline IoStat gIoStat;

inline PluginManager pluginManager;
