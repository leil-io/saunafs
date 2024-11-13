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

#include "common/platform.h"

#include "chunkserver-common/global_shared_resources.h"
#include "chunkserver-common/subfolder.h"
#include "chunkserver/metadata_cache.h"
#include "devtools/TracePrinter.h"

#include <filesystem>
#include <fstream>
#include <regex>
#include <string>

namespace fs = std::filesystem;

std::string MetadataCache::metadataCachePath = "";
bool MetadataCache::isValidPath = false;

void MetadataCache::setMetadataCachePath(const std::string &path){
	if (fs::exists(path)) {
		metadataCachePath = path;
		isValidPath = true;
		safs_pretty_syslog(LOG_INFO, "Metadata cache path set to: %s",
		                   path.c_str());
	} else if (!path.empty()) {
		safs_pretty_syslog(LOG_ERR, "Metadata cache path %s does not exist",
		                   path.c_str());
	}
}

bool MetadataCache::diskCanLoadMetadataFromCache(IDisk *disk) {
	if (!isValidPath) {
		safs_pretty_syslog(LOG_ERR, "Metadata cache path is not valid");
		return false;
	}

	// TODO(Guillex): Add support for zoned devices
	if (disk->isZonedDevice()) {
		safs_pretty_syslog(LOG_WARNING,
		                   "Metadata cache for zoned devices is not supported "
		                   "yet. Metadata will be loaded from: %s",
		                   disk->metaPath().c_str());
		return false;
	}

	auto existsMetaFile = fs::exists(getMetadataCacheFilename(disk));
	auto existsControlFile = fs::exists(getMetadataCacheFilename(disk) +
	                                    kControlFileExtension.data());

	// TODO(Guillex): Add better consistency check here

	return existsMetaFile && existsControlFile;
}

std::string MetadataCache::getMetadataCacheFilename(IDisk *disk) {
	return getMetadataCacheFilename(disk->metaPath());
}

std::string MetadataCache::getMetadataCacheFilename(
    const std::string &diskPath) {
	// Remove the leading and trailing slashes from the path
	std::string filteredPath =
	    std::regex_replace(diskPath, std::regex("^/+|/+$"), "");
	// Replace the remaining slashes with dots
	filteredPath = std::regex_replace(filteredPath, std::regex("/"), ".");

	return metadataCachePath + "/" + filteredPath + kCacheFileExtension.data();
}

bool MetadataCache::writeCacheFile(const std::string &cachePath,
                                   const std::vector<uint8_t> &chunks) {
	safs_pretty_syslog(LOG_INFO, "Cache file: %s", cachePath.c_str());

	std::ofstream cacheFile(cachePath, std::ios::binary);

	if (!cacheFile.is_open()) {
		safs_pretty_syslog(LOG_ERR, "Failed to open cache file %s",
		                   cachePath.c_str());
		return false;
	}

	cacheFile.write(reinterpret_cast<const char *>(chunks.data()),
	                chunks.size());

	// Check if the entire file was written
	if (cacheFile.tellp() != static_cast<std::streampos>(chunks.size())) {
		safs_pretty_syslog(LOG_ERR, "Failed to write entire cache file %s",
		                   cachePath.c_str());
		return false;
	}

	cacheFile.flush();

	return true;
}

bool MetadataCache::writeControlFile(const std::string &diskPath,
                                     const std::string &cachePath,
                                     const std::vector<uint8_t> &chunks) {
	std::string controlPath = cachePath + kControlFileExtension.data();
	std::ofstream controlFile(controlPath);

	if (!controlFile.is_open()) {
		safs_pretty_syslog(LOG_ERR, "Failed to create control file %s",
		                   controlPath.c_str());
		return false;
	}

	controlFile << "version: " << kMetadataCacheVersion << '\n';

	controlFile << "timestamp: "
	            << std::chrono::system_clock::now().time_since_epoch().count()
	            << '\n';

	controlFile << "disk: " << diskPath << '\n';
	// TODO(Guillex): Will not work for zoned devices (Fragments)
	controlFile << "chunks: " << chunks.size() / kChunkSerializedSize << '\n';

	controlFile.flush();

	return true;
}

void MetadataCache::hddWriteBinaryMetadataCache() {
	TRACETHIS();

	if (!isValidPath) { return; }

	std::lock_guard disksLockGuard(gDisksMutex);

	std::map<std::string, std::vector<uint8_t>> diskChunks;

	for (const auto &disk : gDisks) {
		diskChunks.emplace(disk->metaPath(), std::vector<uint8_t>());
	}

	static std::vector<uint8_t> currentChunk(kChunkSerializedSize);

	for (const auto &chunkEntry : gChunksMap) {
		IChunk *chunk = chunkEntry.second.get();
		IDisk *disk = chunk->owner();
		std::string diskPath = disk->metaPath();

		disk->serializeChunkMetadataIntoBuffer(currentChunk.data(), chunk);
		diskChunks[diskPath].insert(diskChunks[diskPath].end(),
		                            currentChunk.begin(), currentChunk.end());
	}

	if (!fs::exists(MetadataCache::getMetadataCachePath())) {
		if (!fs::create_directories(MetadataCache::getMetadataCachePath())) {
			safs_pretty_syslog(LOG_ERR, "Failed to create cache directory %s",
			                   MetadataCache::getMetadataCachePath().c_str());
			return;
		}
	}

	for (const auto &[diskPath, chunks] : diskChunks) {
		safs_pretty_syslog(LOG_INFO, "Disk: %s: size: %zu", diskPath.c_str(),
		                   chunks.size());

		std::string cachePath = getMetadataCacheFilename(diskPath);

		bool wasCacheFileWritten = false;
		bool wasControlFileWritten = false;

		try {
			wasCacheFileWritten = writeCacheFile(cachePath, chunks);

			if (!wasCacheFileWritten) {
				safs_pretty_syslog(LOG_ERR, "Failed to write cache file %s",
				                   cachePath.c_str());
			}
		} catch (const std::exception &e) {
			safs_pretty_syslog(LOG_ERR, "Failed to write cache file: %s",
			                   e.what());
		}

		try {
			wasControlFileWritten =
			    writeControlFile(diskPath, cachePath, chunks);

			if (!wasControlFileWritten) {
				safs_pretty_syslog(LOG_ERR,
				                   "Failed to write control file for %s",
				                   cachePath.c_str());
			}
		} catch (const std::exception &e) {
			safs_pretty_syslog(LOG_ERR, "Failed to write control file: %s",
			                   e.what());
		}

		if (!wasCacheFileWritten || !wasControlFileWritten) {
			// There was an error, remove both files
			if (fs::exists(cachePath + kControlFileExtension.data())) {
				fs::remove(cachePath + kControlFileExtension.data());
			}

			if (fs::exists(cachePath)) { fs::remove(cachePath); }

			continue;
		}

		safs_pretty_syslog(LOG_INFO, "Chunk metadata cache file written to: %s",
		                   cachePath.c_str());
	}
}

