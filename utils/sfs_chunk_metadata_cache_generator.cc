/*
   Copyright 2023 Leil Storage OÜ

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

#include "chunkserver-common/subfolder.h"
#include "chunkserver/chunk_filename_parser.h"
#include "chunkserver/metadata_cache.h"
#include "common/chunk_part_type.h"
#include "slogger/slogger.h"

#include <fcntl.h>
#include <getopt.h>
#include <unistd.h>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

void showHelpMessageAndExit(const std::string &progName, int status) {
	std::string text =
	    "Usage:\n" + progName +
	    " <--hdd-file CHUNKSERVER_HDD_FILE> <--cache-dir METADATA_CACHE_PATH> "
		"[--syslog] [--help]\n\n"
	    "Scan all Disks in CHUNKSERVER_HDD_FILE and generates the "
	    "chunk metadata cache files in METADATA_CACHE_PATH.\n\n";

	if (status == 0) {
		std::cout << text;
	} else {
		std::cerr << text;
	}

	exit(status);
}

/// Minimal information needed to work with a disk
struct SimplifiedDisk {
	std::string metaPath;  ///< Where the .met files are stored
	std::string dataPath;  ///< Where the .dat files are stored
	bool isZoned;  ///< If the disk is a zoned device

	/// Constructor with all the needed information
	SimplifiedDisk(std::string metaPath_, std::string dataPath_, bool isZoned_)
	    : metaPath(std::move(metaPath_)),
	      dataPath(std::move(dataPath_)),
	      isZoned(isZoned_) {}

	// Not needed
	SimplifiedDisk() = delete;

	// Useful for emplace_back
	SimplifiedDisk(SimplifiedDisk &&other) noexcept = default;

	// Not needed
	SimplifiedDisk(const SimplifiedDisk &other) = delete;
	SimplifiedDisk &operator=(const SimplifiedDisk &other) = delete;
	SimplifiedDisk &operator=(SimplifiedDisk &&other) = delete;

	// To comply with rule of five
	~SimplifiedDisk() = default;
};

bool processHddLine(const std::string &originalLine,
                    std::vector<SimplifiedDisk> &disks) {
	std::string line = originalLine;

	std::string metaPath;
	std::string dataPath;

	// Trim leading whitespaces
	auto forwardIt = std::find_if(line.begin(), line.end(), [](char symbol) {
		return !std::isspace<char>(symbol, std::locale::classic());
	});

	if (line.begin() != forwardIt) { line.erase(line.begin(), forwardIt); }

	// Check for empty or commented
	if (line.empty() || line.starts_with("#")) { return true; }

	// Trim trailing whitespaces
	auto reverseIt = std::find_if(line.rbegin(), line.rend(), [](char symbol) {
		return !std::isspace<char>(symbol, std::locale::classic());
	});
	line.erase(reverseIt.base(), line.end());

	// Check if marked for removal
	if (line.at(0) == '*') { line.erase(line.begin()); }

	static const std::string zonedToken = "zonefs:";
	bool isZoned = false;

	if (line.find(zonedToken) == 0) {
		isZoned = true;
		line.erase(0, zonedToken.size());
	}

	static std::string const delimiter = " | ";
	auto delimiterPos = line.find(delimiter);

	if (isZoned && delimiterPos == std::string::npos) {
		std::cerr
		    << "Parse hdd line: " << line
		    << " - zoned drives must contain two paths separated by ' | '."
		    << "\n";
		return false;  // Not valid yet
	}

	if (delimiterPos != std::string::npos) {
		metaPath = line.substr(0, delimiterPos);
		dataPath = line.substr(delimiterPos + delimiter.length());
	} else {
		metaPath = line;
		dataPath = line;
	}

	// Ensure / at the end for both paths
	if (metaPath.at(metaPath.size() - 1) != '/') { metaPath.append("/"); }
	if (dataPath.at(dataPath.size() - 1) != '/') { dataPath.append("/"); }

	if (isZoned) {
		std::cerr << "Zoned devices are not supported yet: " << originalLine
		          << "\n";
		return true;
	}

	disks.emplace_back(metaPath, dataPath, isZoned);

	return true;
}

bool extractDisksFromHddFile(
	const std::string &hddFilename, std::vector<SimplifiedDisk> &disks) {
	std::ifstream hddFile(hddFilename);

	if (!hddFile.is_open()) {
		std::cerr << "Failed to open file: " << hddFilename << "\n";
		exit(1);
	}

	std::string line;

	while (std::getline(hddFile, line)) {
		if (!processHddLine(line, disks)) {
			std::cerr << "Failed to process disk line: " << line << "\n";
			return false;
		}
	}

	return true;
}

void writeCacheFileForDisk(const SimplifiedDisk &disk,
                           const std::string &metadataCachePath) {
	safs::log_info("Scanning disk: {}", disk.metaPath);
	fs::path metaPath(disk.metaPath);

	constexpr size_t kChunksPerBulk = 1024;
	constexpr size_t kChunkBulkSize =
	    kChunksPerBulk * MetadataCache::kChunkSerializedSize;

	std::vector<uint8_t> diskChunks(kChunkBulkSize);
	uint8_t *buffer = diskChunks.data();
	uint64_t currentBulkChunks = 0;
	uint64_t totalChunks = 0;

	for (const auto &entry : fs::recursive_directory_iterator(metaPath)) {
		if (entry.path().extension() == ".met") {
			uint64_t chunkId = 0;
			uint32_t version = 0;
			uint16_t type = 0;
			uint16_t blocks = 0;

			std::string filename = entry.path().filename().string();
			ChunkFilenameParser parser(filename);

			if (parser.parse() == ChunkFilenameParser::OK) {
				chunkId = parser.chunkId();
				version = parser.chunkVersion();
				type = parser.chunkType().getId();
			}

			std::string dataBasename =
			    filename.replace(filename.find(".met"), 4, ".dat");
			std::string dataFilename =
			    disk.dataPath +
			    Subfolder::getSubfolderNameGivenChunkId(chunkId) + "/" +
			    dataBasename;

			buffer = diskChunks.data() +
			         totalChunks * MetadataCache::kChunkSerializedSize;
			put64bit(&buffer, chunkId);
			put32bit(&buffer, version);
			serialize(&buffer, type);
			put16bit(&buffer, blocks);

			++totalChunks;
			++currentBulkChunks;

			if (currentBulkChunks == kChunksPerBulk) {
				currentBulkChunks = 0;
				diskChunks.resize(diskChunks.size() + kChunkBulkSize);
			}
		}
	}

	diskChunks.resize(totalChunks * MetadataCache::kChunkSerializedSize);

	auto cachePath = MetadataCache::getMetadataCacheFilename(
	    disk.metaPath, metadataCachePath);
	safs::log_info("Writing metadata cache file: {} ({} chunks)", cachePath,
	               totalChunks);

	bool wasCacheFileWritten = false;
	bool wasControlFileWritten = false;

	try {
		wasCacheFileWritten =
		    MetadataCache::writeCacheFile(cachePath, diskChunks);

		if (!wasCacheFileWritten) {
			safs::log_err("Failed to write cache file {}", cachePath);
		}
	} catch (const std::exception &e) {
		safs::log_err("Failed to write cache file {} ({})", cachePath,
		              e.what());
	}

	try {
		wasControlFileWritten = MetadataCache::writeControlFile(
		    disk.metaPath, cachePath, diskChunks);

		if (!wasControlFileWritten) {
			safs::log_err("Failed to write control file for {}", cachePath);
		}
	} catch (const std::exception &e) {
		safs::log_err("Failed to write control file for {} ({})", cachePath,
		              e.what());
	}

	if (!wasCacheFileWritten || !wasControlFileWritten) {
		safs::log_err("Removing cache files for disk: {}", disk.metaPath);

		if (fs::exists(cachePath +
		               MetadataCache::kControlFileExtension.data())) {
			fs::remove(cachePath + MetadataCache::kControlFileExtension.data());
		}

		if (fs::exists(cachePath)) { fs::remove(cachePath); }
	}
}

void initializeLogger(std::shared_ptr<spdlog::logger> &logger, bool useSyslog) {
	if (useSyslog) {
		logger = spdlog::get("syslog");
		if (!logger) { logger = spdlog::syslog_logger_mt("syslog"); }
	} else {
		logger = spdlog::stdout_color_mt("console");
	}
}

struct CmdOptions {
	std::string chunkserverHddFile;
	std::string metadataCachePath;
	bool useSyslog = false;
	bool showHelp = false;
};

CmdOptions parseCmdOptions(int argc, char **argv) {
	CmdOptions options;

	const char *short_opts = "";
	const std::array<option, 5> long_opts = {
	    {{"cache-dir", required_argument, nullptr, 'c'},
	     {"hdd-file", required_argument, nullptr, 'f'},
	     {"syslog", no_argument, nullptr, 's'},
		 {"help", no_argument, nullptr, 'h'},
	     {nullptr, 0, nullptr, 0}}};

	while (true) {
		const auto opt =
		    getopt_long(argc, argv, short_opts, long_opts.data(), nullptr);

		if (opt == -1) { break; }

		switch (opt) {
		case 'c':
			options.metadataCachePath = optarg;
			break;
		case 'f':
			options.chunkserverHddFile = optarg;
			break;
		case 's':
			options.useSyslog = true;
			break;
		case 'h':
			options.showHelp = true;
			showHelpMessageAndExit(argv[0], 0);
			break;
		case '?':
		default:
			showHelpMessageAndExit(argv[0], 1);
			break;
		}
	}

	if (options.metadataCachePath.empty() ||
	    options.chunkserverHddFile.empty()) {
		showHelpMessageAndExit(argv[0], 1);
	}

	return options;
}

int main(int argc, char **argv) {
	CmdOptions options = parseCmdOptions(argc, argv);

	// Initialize the logger to have syslog output
	std::shared_ptr<spdlog::logger> logger;
	initializeLogger(logger, options.useSyslog);

	if (options.showHelp) {
		std::cout << "Show help message\n";
		showHelpMessageAndExit(argv[0], 0);
	}

	safs::log_info("Chunkserver HDD file: {}", options.chunkserverHddFile);
	safs::log_info("Metadata cache path: {}", options.metadataCachePath);

	// Get the disks information from the HDD file
	std::vector<SimplifiedDisk> disks;

	if (!extractDisksFromHddFile(options.chunkserverHddFile, disks)) {
		safs::log_err("Failed to extract disks from HDD file: {}",
		              options.chunkserverHddFile);
		return 1;
	}

	// Create the metadata cache directory if it doesn't exist
	if (!fs::exists(options.metadataCachePath)) {
		if (!fs::create_directories(options.metadataCachePath)) {
			safs::log_err("Failed to create cache directory {}",
			              options.metadataCachePath);
			return 1;
		}
	}

	// Create a thread for each disk to write the cache files
	std::vector<std::thread> diskThreads;
	diskThreads.reserve(disks.size());

	for (const auto &disk : disks) {
		diskThreads.emplace_back(writeCacheFileForDisk, std::ref(disk),
		                         options.metadataCachePath);
	}

	for (auto &thread : diskThreads) {
		thread.join();
	}

	return 0;
}
