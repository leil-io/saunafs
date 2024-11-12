/*
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

#pragma once

#include "common/platform.h"

#include <string_view>

#include "chunkserver-common/disk_interface.h"

class MetadataCache {
	static std::string metadataCachePath;
	static bool isValidPath;

	static bool writeCacheFile(const std::string &cachePath,
	                           const std::vector<uint8_t> &chunks);

	static bool writeControlFile(const std::string &diskPath,
	                             const std::string &cachePath,
	                             const std::vector<uint8_t> &chunks);

public:
	// 8 bytes for chunk id, 4 bytes for version, 2 bytes for type, 2 bytes
	// for blocks
	static constexpr size_t kChunkSerializedSize = 8 + 4 + 2 + 2;
	static constexpr int kMetadataCacheVersion = 1;
	static constexpr std::string_view kCacheFileExtension = ".cache";
	static constexpr std::string_view kControlFileExtension = ".control";

	static void setMetadataCachePath(const std::string &path);

	static bool diskCanLoadMetadataFromCache(IDisk *disk);

	static std::string getMetadataCacheFilename(IDisk *disk);
	static std::string getMetadataCacheFilename(const std::string &diskPath);

	static std::string getMetadataCachePath() { return metadataCachePath; }

	static void hddWriteBinaryMetadataCache();

	static std::string generateChunkMetaFilename(IDisk *disk, uint64_t chunkId,
	                                             uint32_t chunkVersion,
	                                             ChunkPartType chunkType);
};