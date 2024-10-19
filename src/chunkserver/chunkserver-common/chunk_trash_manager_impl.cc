/*
   Copyright 2023-2024  Leil Storage OÜ

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
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
#pragma GCC diagnostic ignored "-Wstringop-overflow"

#include <algorithm>

#pragma GCC diagnostic pop

#include <sys/syslog.h>
#include <sys/statvfs.h>
#include <stack>

#include "errors/saunafs_error_codes.h"
#include "slogger/slogger.h"

#include "chunk_trash_manager_impl.h"
#include "common/cfg.h"

namespace fs = std::filesystem;

size_t ChunkTrashManagerImpl::kAvailableThresholdGB = 10;
size_t ChunkTrashManagerImpl::kTrashTimeLimitSeconds = 259200;
size_t ChunkTrashManagerImpl::kTrashGarbageCollectorBulkSize = 1000;
size_t ChunkTrashManagerImpl::kGarbageCollectorSpaceRecoveryStep = 10;

void ChunkTrashManagerImpl::reloadConfig() {
	kAvailableThresholdGB = cfg_get("CHUNK_TRASH_AVAILABLE_THRESHOLD_GB", kAvailableThresholdGB);
	kTrashTimeLimitSeconds = cfg_get("CHUNK_TRASH_TIMEOUT_S", kTrashTimeLimitSeconds);
	kTrashGarbageCollectorBulkSize = cfg_get("CHUNK_TRASH_CG_BULK_SIZE", kTrashGarbageCollectorBulkSize);
	kGarbageCollectorSpaceRecoveryStep = cfg_get("CHUNK_TRASH_GC_RECOVERY_STEP", kGarbageCollectorSpaceRecoveryStep);
}

std::string
ChunkTrashManagerImpl::getTimeString(std::time_t time1) {
//
	std::tm *utcTime = std::gmtime(&time1);  // Convert to UTC

	std::ostringstream oss;
	oss << std::put_time(utcTime, kTimeStampFormat.c_str());
	return oss.str();
}

std::time_t ChunkTrashManagerImpl::getTimeFromString(
		const std::string &timeString) {
	std::tm time = {};
	std::istringstream stringReader(timeString);
	stringReader >> std::get_time(&time, kTimeStampFormat.c_str());
	if (stringReader.fail()) {
		throw std::invalid_argument("Invalid time string");
	}
	return std::mktime(&time);
}

int ChunkTrashManagerImpl::moveToTrash(
		const fs::path &filePath,
		const fs::path &diskPath,
		const std::time_t &deletionTime) {

	if (!fs::exists(filePath)) {
		safs_pretty_syslog(LOG_ERR, "File does not exist: %s",
		                   filePath.string().c_str());
		return SAUNAFS_ERROR_ENOENT;
	}

	const fs::path trashDir = diskPath / ChunkTrashManager::kTrashDirname;
	fs::create_directories(trashDir);

	if (!filePath.string().starts_with(diskPath.string())) {
		safs_pretty_syslog(LOG_ERR,
		                   "File path does not belong to the disk: %s",
		                   filePath.string().c_str());
		return SAUNAFS_ERROR_EINVAL;
	}

	const std::string deletionTimestamp = getTimeString(deletionTime);
	const fs::path fileTrashPath =
			(trashDir / fs::relative(filePath, diskPath)).string() + "." +
			deletionTimestamp;

	try {
		fs::create_directories(fileTrashPath.parent_path());
		fs::rename(filePath, fileTrashPath);
		trashIndex->add(deletionTime, fileTrashPath.string(),
		                diskPath.string());
	} catch (const fs::filesystem_error &e) {
		safs_pretty_syslog(LOG_ERR,
		                   "Failed to move file to trash: %s, error: %s",
		                   filePath.string().c_str(), e.what());
		return SAUNAFS_ERROR_IO;
	}

	return SAUNAFS_STATUS_OK;
}

void ChunkTrashManagerImpl::removeTrashFiles(
		const ChunkTrashIndex::TrashIndexDiskEntries &filesToRemove) {
	for (const auto &[diskPath, fileEntries]: filesToRemove) {
		for (const auto &fileEntry: fileEntries) {
			try {
				if (!fs::remove(fileEntry.second)) {
					safs_pretty_syslog(LOG_ERR,
					                   "Failed to remove file from trash: %s",
					                   fileEntry.second.c_str());
					continue;
				}
				trashIndex->remove(fileEntry.first, fileEntry.second, diskPath);
			} catch (const fs::filesystem_error &e) {
				safs_pretty_syslog(LOG_ERR,
				                   "Failed to remove file from trash: %s, error: %s",
				                   fileEntry.second.c_str(), e.what());
			}
		}
	}
}

fs::path ChunkTrashManagerImpl::getTrashDir(const fs::path &diskPath) {
	return diskPath / ChunkTrashManager::kTrashDirname;
}

int ChunkTrashManagerImpl::init(const std::string &diskPath) {
	reloadConfig();
	const fs::path trashDir = getTrashDir(diskPath);
	if (!fs::exists(trashDir)) {
		fs::create_directories(trashDir);
	}

	trashIndex->reset(diskPath);

	for (const auto &file: fs::recursive_directory_iterator(trashDir)) {
		if (fs::is_regular_file(file) &&
		    file.path().string().find(kTrashGuardString) != std::string::npos) {
			const std::string filename = file.path().filename().string();
			const std::string deletionTimeStr = filename.substr(
					filename.find_last_of('.') + 1);
			if (!isValidTimestampFormat(deletionTimeStr)) {
				safs_pretty_syslog(LOG_ERR,
				                   "Invalid timestamp format in file: %s, skipping.",
				                   file.path().string().c_str());
				continue;
			}
			try {
				const std::time_t deletionTime = getTimeFromString(
						deletionTimeStr);
				trashIndex->add(deletionTime, file.path().string(),
				                diskPath);
			} catch (const std::invalid_argument &e) {
				safs_pretty_syslog(LOG_ERR, "Failed to parse deletion time "
				                            "from file: %s, skipping.",
				                   file.path().string().c_str());
				continue;
			}
		}
	}

	return SAUNAFS_STATUS_OK;
}

bool ChunkTrashManagerImpl::isValidTimestampFormat(
		const std::string &timestamp) {
	return timestamp.size() == kTimeStampLength &&
	       std::all_of(timestamp.begin(), timestamp.end(), ::isdigit);
}

void ChunkTrashManagerImpl::removeExpiredFiles(
		const time_t &timeLimit, size_t bulkSize) {
	const auto expiredFilesCollection = trashIndex->getExpiredFiles(timeLimit,
	                                                                bulkSize);
	removeTrashFiles(expiredFilesCollection);
}

void ChunkTrashManagerImpl::cleanEmptyFolders(const std::string &directory) {
	if (!fs::exists(directory) || !fs::is_directory(directory)) {
		return; // Invalid path, so we do nothing
	}

	// Iterate through the directory's contents
	for (const auto& entry : fs::directory_iterator(directory)) {
		if (fs::is_directory(entry.path())) {
			// Recursively clean subdirectories
			cleanEmptyFolders(entry.path());
		}
	}

	if (!directory.contains(ChunkTrashManagerImpl::kTrashGuardString)) {
		return; // We only clean up trash directories
	}

	// After processing subdirectories, check if the directory is now empty
	if (fs::is_empty(directory)) {
		std::error_code ec;
		fs::remove(directory, ec); // Try to remove the directory
		if (ec) {
			safs_pretty_syslog(LOG_ERR, "Failed to remove empty directory: %s",
			                   directory.c_str());
		}
	}
}

size_t ChunkTrashManagerImpl::checkAvailableSpace(
		const std::string &diskPath) {
	struct statvfs stat{};
	if (statvfs(diskPath.c_str(), &stat) != 0) {
		safs_pretty_syslog(LOG_ERR, "Failed to get file system statistics");
		return 0;
	}
	size_t const kGiBMultiplier = 1 << 30;
	size_t const availableGb = stat.f_bavail * stat.f_frsize / kGiBMultiplier;
	return availableGb;
}

void ChunkTrashManagerImpl::makeSpace(
		const std::string &diskPath,
		const size_t spaceAvailabilityThreshold,
		const size_t recoveryStep) {
	size_t availableSpace = checkAvailableSpace(diskPath);
	while (availableSpace < spaceAvailabilityThreshold) {
		const auto olderFilesCollection = trashIndex->getOlderFiles(diskPath,
		                                                            recoveryStep);
		if (olderFilesCollection.empty()) {
			break;
		}
		removeTrashFiles({{diskPath, olderFilesCollection}});
		availableSpace = checkAvailableSpace(diskPath);
	}
}

void ChunkTrashManagerImpl::makeSpace(
		const size_t spaceAvailabilityThreshold, const size_t recoveryStep) {
	for (const auto &diskPath: trashIndex->getDiskPaths()) {
		makeSpace(diskPath, spaceAvailabilityThreshold, recoveryStep);
	}
}

void ChunkTrashManagerImpl::cleanEmptyFolders() {
	for (const auto &diskPath: trashIndex->getDiskPaths()) {
		fs::path trashDir = getTrashDir(diskPath);
		cleanEmptyFolders(trashDir.string());
	}
}

void ChunkTrashManagerImpl::collectGarbage() {
	if (!ChunkTrashManager::isEnabled) {
		return;
	}
	std::time_t const currentTime = std::time(nullptr);
	std::time_t const expirationTime = currentTime - kTrashTimeLimitSeconds;
	removeExpiredFiles(expirationTime, kTrashGarbageCollectorBulkSize);
	makeSpace(kAvailableThresholdGB,
	          kGarbageCollectorSpaceRecoveryStep);
	cleanEmptyFolders();
}
