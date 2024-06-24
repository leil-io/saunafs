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

#pragma once

#include "common/platform.h"

#include <vector>

#include "chunkserver-common/chunk_interface.h"
#include "chunkserver/output_buffer.h"
#include "common/chunk_part_type.h"
#include "common/chunk_with_version_and_type.h"
#include "protocol/chunks_with_type.h"

uint32_t hddGetAndResetErrorCounter();

void hddGetDamagedChunks(std::vector<ChunkWithType>& chunks, std::size_t limit);
void hddGetLostChunks(std::vector<ChunkWithType>& chunks, std::size_t limit);
void hddGetNewChunks(std::vector<ChunkWithVersionAndType>& chunks,
                     std::size_t limit);

/* lock/unlock pair */
uint32_t hddGetSerializedSizeOfAllDiskInfosV2();
void hddSerializeAllDiskInfosV2(uint8_t *buff);

const std::size_t kChunkBulkSize = 1000;
using BulkFunction = std::function<void(std::vector<ChunkWithVersionAndType>&)>;
/// Executes the callback for each bulk of at most \p kChunkBulkSize chunks.
void hddForeachChunkInBulks(BulkFunction bulkCallback,
                            std::size_t bulkSize = kChunkBulkSize);

int hddGetAndResetSpaceChanged();
void hddGetTotalSpace(uint64_t *usedSpace, uint64_t *totalSpace,
                      uint32_t *chunkCount, uint64_t *toDelUsedSpace,
                      uint64_t *toDelTotalSpace, uint32_t *toDelChunkCount);
int hddGetLoadFactor();

/* I/O operations */
int hddOpen(IChunk *chunk);
int hddOpen(uint64_t chunkId, ChunkPartType chunkType);
int hddClose(IChunk *chunk);
int hddClose(uint64_t chunkId, ChunkPartType chunkType);
int hddPrefetchBlocks(uint64_t chunkId, ChunkPartType chunkType,
                      uint32_t firstBlock, uint16_t numberOfBlocks);
int hddRead(uint64_t chunkId, uint32_t version, ChunkPartType chunkType,
            uint32_t offset, uint32_t size,
            [[maybe_unused]] uint32_t maxBlocksToBeReadBehind,
            [[maybe_unused]] uint32_t blocksToBeReadAhead,
            OutputBuffer *outputBuffer);
int hddChunkWriteBlock(uint64_t chunkId, uint32_t version,
                       ChunkPartType chunkType, uint16_t blocknum,
                       uint32_t offset, uint32_t size, uint32_t crc,
                       const uint8_t *buffer);

/* chunk info */
int hddChunkGetNumberOfBlocks(uint64_t chunkId, ChunkPartType chunkType,
                              uint32_t version, uint16_t *blocks);

/* chunk operations */

/* all chunk operations in one call */
// chunkNewVersion>0 && length==0xFFFFFFFF && chunkIdCopy==0   -> change version
// chunkNewVersion>0 && length==0xFFFFFFFF && chunkIdCopy>0     -> duplicate
// chunkNewVersion>0 && length<=SFSCHUNKSIZE && chunkIdCopy==0  -> truncate
// chunkNewVersion>0 && length<=SFSCHUNKSIZE && chunkIdCopy>0   -> dup and trun
// chunkNewVersion==0 && length==0                              -> delete
// chunkNewVersion==0 && length==1                              -> create
// chunkNewVersion==0 && length==2                              -> test
int hddChunkOperation(uint64_t chunkId, uint32_t chunkVersion,
                      ChunkPartType chunkType, uint32_t chunkNewVersion,
                      uint64_t chunkIdCopy, uint32_t chunkVersionCopy,
                      uint32_t length);

/* chunk testing */
void hddAddChunkToTestQueue(ChunkWithVersionAndType chunk);

/* initialization */
int initDiskManager();
int loadPlugins();
int hddLateInit();
int hddInit();

// Chunk low-level operations
// The following functions shouldn't be used, unless for specific implementation
// i.e. \see ChunkFileCreator
// In most cases functions above are prefered.

/// Deletes the chunk from the registry and from the disk's testlist in a
/// thread-safe way.
void hddDeleteChunkFromRegistry(IChunk *chunk);

/** \brief Creates a new chunk on disk
 *
 * \param chunkid - id of created chunk
 * \param version - version of created chunk
 * \param chunkType - type of created chunk
 * \return On success returns pair of SAUNAFS_STATUS_OK and created chunk in
 *         locked state. On failure, returns pair of error code and nullptr.
 */
std::pair<int, IChunk *> hddInternalCreateChunk(uint64_t chunkId,
                                                uint32_t version,
                                                ChunkPartType chunkType);
int hddInternalCreate(uint64_t chunkId, uint32_t version,
                      ChunkPartType chunkType);
int hddInternalDelete(IChunk *chunk, uint32_t version);
int hddInternalDelete(uint64_t chunkId, uint32_t version,
                      ChunkPartType chunkType);
int hddInternalUpdateVersion(IChunk *chunk, uint32_t version,
                             uint32_t newversion);
int hddInternalUpdateVersion(uint64_t chunkId, uint32_t version,
                             uint32_t newversion, ChunkPartType chunkType);
