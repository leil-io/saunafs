/*
   Copyright 2013-2015 Skytechnology sp. z o.o.

   This file is part of LizardFS.

   LizardFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   LizardFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with LizardFS. If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "common/platform.h"

#include <cstdint>
#include <cstdlib>
#include <string>
#include <sys/types.h>

#include <condition_variable>
#include <thread>

#include "chunkserver/chunk_format.h"
#include "chunkserver/folder.h"
#include "common/chunk_part_type.h"

// Block data and crc summaric size.
constexpr uint32_t kHddBlockSize = MFSBLOCKSIZE + 4;

enum ChunkState {
	CH_AVAIL,
	CH_LOCKED,
	CH_DELETED,
	CH_TOBEDELETED
};

/// Condition variable with a count of threads which are waiting on it.
struct CondVarWithWaitCount {
	std::condition_variable condVar;
	uint32_t numberOfWaitingThreads = 0;
};

class Chunk {
public:
	static const uint32_t kNumberOfSubfolders = 256;

	enum { kCurrentDirectoryLayout = 0, kMooseFSDirectoryLayout };

	Chunk(uint64_t chunkId, ChunkPartType type, ChunkState state);

	// We use unique_ptr and we do not need to copy or move the chunks
	Chunk(const Chunk&) = delete;
	Chunk(Chunk&&) = default;
	Chunk& operator=(const Chunk&) = delete;
	Chunk& operator=(Chunk&&) = default;

	virtual ~Chunk() = default;

	std::string filename() const {
		return filename_layout_ >= kCurrentDirectoryLayout
		               ? generateFilenameForVersion(version, filename_layout_)
		               : std::string();
	}

	std::string generateFilenameForVersion(uint32_t version, int layout_version = kCurrentDirectoryLayout) const;
	int renameChunkFile(uint32_t new_version, int new_layout_version = kCurrentDirectoryLayout);
	void setFilenameLayout(int layout_version) { filename_layout_ = layout_version; }

	virtual off_t getBlockOffset(uint16_t blockNumber) const = 0;
	virtual off_t getFileSizeFromBlockCount(uint32_t blockCount) const = 0;
	virtual bool isFileSizeValid(off_t fileSize) const = 0;
	virtual ChunkFormat chunkFormat() const { return ChunkFormat::IMPROPER; }
	uint32_t maxBlocksInFile() const;
	virtual void setBlockCountFromFizeSize(off_t fileSize) = 0;
	ChunkPartType type() const { return type_; }
	static uint32_t getSubfolderNumber(uint64_t chunkId, int layout_version = 0);
	static std::string getSubfolderNameGivenNumber(uint32_t subfolderNumber, int layout_version = 0);
	static std::string getSubfolderNameGivenChunkId(uint64_t chunkId, int layout_version = 0);

	/// The pointer to the condition variable on which threads wait until this
	/// chunk is unlocked.
	///
	/// We only assign a condition variable to a chunk which is locked
	/// and another thread attempts to lock it.
	/// Guarded by `gChunkRegistryLock`.
	std::unique_ptr<CondVarWithWaitCount> condVar;

	struct Folder *owner;
	uint64_t chunkid;
	uint32_t version;
	int32_t  fd;
	uint16_t blocks;
	uint16_t refcount;
	uint16_t blockExpectedToBeReadNext;

protected:
	ChunkPartType type_;
	int8_t filename_layout_; /*!< <0 - no valid name (empty string)
	                               0 - current directory layout
	                              >0 - older directory layouts */
public:
	uint8_t validattr;
	uint8_t state;
	uint8_t wasChanged;

	/// The index of the chunk within the folder.
	///
	/// This value may change during the lifetime of the chunk, and corresponds
	/// to the order in which the chunk will be subject to checksum testing.
	///
	/// This is `kInvalidIndex` if the value
	/// is not valid, i.e., when the chunk is not assigned into a folder yet.
	///
	/// This value is governed by the owning folder's `chunks` collection.
	size_t indexInFolder = FolderChunks::kInvalidIndex;
};

class MooseFSChunk : public Chunk {
public:
	static const size_t kMaxSignatureBlockSize = 1024;
	static const size_t kMaxCrcBlockSize = MFSBLOCKSINCHUNK * sizeof(uint32_t);
	static const size_t kMaxPaddingBlockSize = 4096;
	static const size_t kMaxHeaderSize =
			kMaxSignatureBlockSize + kMaxCrcBlockSize + kMaxPaddingBlockSize;
	static const size_t kDiskBlockSize = 4096; // 4kB

	typedef std::array<uint8_t, kMaxCrcBlockSize> CrcDataContainer;

	MooseFSChunk(uint64_t chunkId, ChunkPartType type, ChunkState state);
	off_t getBlockOffset(uint16_t blockNumber) const override;
	off_t getFileSizeFromBlockCount(uint32_t blockCount) const override;
	bool isFileSizeValid(off_t fileSize) const override;
	void setBlockCountFromFizeSize(off_t fileSize) override;
	ChunkFormat chunkFormat() const override { return ChunkFormat::MOOSEFS; }
	off_t getSignatureOffset() const;
	void readaheadHeader() const;
	size_t getHeaderSize() const;
	off_t getCrcOffset() const;
	size_t getCrcBlockSize() const;
};

class InterleavedChunk : public Chunk {
public:
	InterleavedChunk(uint64_t chunkId, ChunkPartType type, ChunkState state);
	off_t getBlockOffset(uint16_t blockNumber) const override;
	off_t getFileSizeFromBlockCount(uint32_t blockCount) const override;
	bool isFileSizeValid(off_t fileSize) const override;
	void setBlockCountFromFizeSize(off_t fileSize) override;
	ChunkFormat chunkFormat() const override { return ChunkFormat::INTERLEAVED; }
};

#define IF_MOOSEFS_CHUNK(mc, chunk) \
	if (MooseFSChunk *mc = dynamic_cast<MooseFSChunk *>(chunk))

#define IF_INTERLEAVED_CHUNK(lc, chunk) \
	if (InterleavedChunk *lc = dynamic_cast<InterleavedChunk *>(chunk))
