#include "disk_plugin.h"
#include <spdlog/sinks/syslog_sink.h>

#include "chunkserver-common/disk_with_fd.h"

DiskPlugin::DiskPlugin() {}

DiskPlugin::~DiskPlugin() {}

bool DiskPlugin::initialize() {
	// Needed after upgrading to c++23
	// https://github.com/gabime/spdlog/wiki/How-to-use-spdlog-in-DLLs
	initializeLogger();

	// Also needed after upgrading to c++23
	initializeEmptyBlockCrcForDisks();

	return true;
}

void DiskPlugin::initializeLogger() {
		logger_ = spdlog::get("syslog");

		if (!logger_) {
			logger_ = spdlog::syslog_logger_mt("syslog");
		}
	}

std::string DiskPlugin::toString() {
	return name() + " v" + SAUNAFS_PACKAGE_VERSION;
}
