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
		safs::log_info("Metadata cache path set to: {}", path);
	} else if (!path.empty()) {
		safs::log_err("Metadata cache path {} does not exist", path);
	}
}

bool MetadataCache::diskCanLoadMetadataFromCache(IDisk *disk) {
	if (!isValidPath) {
		safs::log_err("Metadata cache path is not valid");
		return false;
	}

	// TODO(Guillex): Add support for zoned devices
	if (disk->isZonedDevice()) {
		safs::log_warn(
		    "Metadata cache for zoned devices is not supported "
		    "yet. Metadata will be loaded from: {}",
		    disk->metaPath());
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
	using Clock = std::chrono::system_clock;
	using Seconds = std::chrono::seconds;

	auto startTime = Clock::now();

	constexpr int kFilePermissions = 0644;
	int cacheFD = ::open(cachePath.c_str(), O_WRONLY | O_CREAT | O_TRUNC,
	                     kFilePermissions);

	if (cacheFD == -1) {
		safs::log_err("Failed to open cache file {}", cachePath);
		return false;
	}

	ssize_t bytesWritten = ::write(cacheFD, chunks.data(), chunks.size());

	if (static_cast<size_t>(bytesWritten) != chunks.size()) {
		safs::log_err("Failed to write entire cache file {}", cachePath);
		::close(cacheFD);
		return false;
	}

	if (::fsync(cacheFD) == -1) {
		safs::log_err("Failed to flush cache file {}", cachePath);
		::close(cacheFD);
		return false;
	}

	if (::close(cacheFD) == -1) {
		safs::log_err("Failed to close cache file {}", cachePath);
		return false;
	}

	auto endTime = Clock::now();
	Seconds duration = std::chrono::duration_cast<Seconds>(endTime - startTime);
	safs::log_info(
	    "Chunk metadata cache file written: {} ({} chunks, {} seconds)",
	    cachePath, chunks.size() / kChunkSerializedSize, duration.count());

	return true;
}

bool MetadataCache::writeControlFile(const std::string &diskPath,
                                     const std::string &cachePath,
                                     const std::vector<uint8_t> &chunks) {
	std::string controlPath = cachePath + kControlFileExtension.data();
	std::ofstream controlFile(controlPath);

	if (!controlFile.is_open()) {
		safs::log_err("Failed to create control file {}", controlPath);
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

	std::string metadataCachePath = getMetadataCachePath();

	if (!fs::exists(metadataCachePath)) {
		if (!fs::create_directories(metadataCachePath)) {
			safs::log_err("Failed to create cache directory {}",
			              metadataCachePath);
			return;
		}
	}

	for (const auto &[diskPath, chunks] : diskChunks) {
		std::string cachePath = getMetadataCacheFilename(diskPath);

		bool wasCacheFileWritten = false;
		bool wasControlFileWritten = false;

		try {
			wasCacheFileWritten = writeCacheFile(cachePath, chunks);

			if (!wasCacheFileWritten) {
				safs::log_err("Failed to write cache file {}", cachePath);
			}
		} catch (const std::exception &e) {
			safs::log_err("Failed to write cache file {} ({})", cachePath,
			              e.what());
		}

		try {
			wasControlFileWritten =
			    writeControlFile(diskPath, cachePath, chunks);

			if (!wasControlFileWritten) {
				safs::log_err("Failed to write control file for {}", cachePath);
			}
		} catch (const std::exception &e) {
			safs::log_err("Failed to write control file for {} ({})", cachePath,
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
	}
}

