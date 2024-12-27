/*
   Copyright 2013-2014 EditShare
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

#include <expected>

#include "common/syslog_defs.h"

#ifndef _WIN32
#define SPDLOG_ENABLE_SYSLOG
#endif
#include "common/small_vector.h"
#include "spdlog/spdlog.h"
#include "spdlog/sinks/rotating_file_sink.h"
#ifndef _WIN32
#include "spdlog/sinks/syslog_sink.h"
#endif
#include "spdlog/sinks/stdout_color_sinks.h"

typedef std::shared_ptr<spdlog::logger> LoggerPtr;

namespace safs {
namespace log_level {
enum LogLevel {
	trace = spdlog::level::trace,
	debug = spdlog::level::debug,
	info = spdlog::level::info,
	warn = spdlog::level::warn,
	err = spdlog::level::err,
	critical = spdlog::level::critical,
	off = spdlog::level::off
};
} // namespace level

template<typename FormatType, typename... Args>
void log(log_level::LogLevel log_level, const FormatType &format, Args&&... args) {
	//NOTICE(sarna): Workaround for old GCC, which has issues with args... inside lambdas
	small_vector<LoggerPtr, 8> loggers;
	spdlog::apply_all([&loggers](LoggerPtr l) {
		loggers.push_back(l);
	});
	for (LoggerPtr &logger : loggers) {
		logger->log((spdlog::level::level_enum)log_level, format, std::forward<Args>(args)...);
	}
}

template<typename FormatType, typename... Args>
void log_trace(const FormatType &format, Args&&... args) {
	log(log_level::trace, fmt::runtime(format), std::forward<Args>(args)...);
}

template<typename FormatType, typename... Args>
void log_debug(const FormatType &format, Args&&... args) {
	log(log_level::debug, fmt::runtime(format), std::forward<Args>(args)...);
}

template<typename FormatType, typename... Args>
void log_info(const FormatType &format, Args&&... args) {
	log(log_level::info, fmt::runtime(format), std::forward<Args>(args)...);
}

template<typename FormatType, typename... Args>
void log_warn(const FormatType &format, Args&&... args) {
	log(log_level::warn, fmt::runtime(format), std::forward<Args>(args)...);
}

template<typename FormatType, typename... Args>
void log_err(const FormatType &format, Args&&... args) {
	log(log_level::err, fmt::runtime(format), std::forward<Args>(args)...);
}

template<typename FormatType, typename... Args>
void log_critical(const FormatType &format, Args&&... args) {
	log(log_level::critical, fmt::runtime(format), std::forward<Args>(args)...);
}

bool add_log_file(const char *path, log_level::LogLevel level, int max_file_size, int max_file_count);
void set_log_flush_on(log_level::LogLevel level);
void drop_all_logs();
bool add_log_syslog();
bool add_log_stderr(log_level::LogLevel level);

// Returns the appropriate log level from the level string, or an error message
// indicating that it's not a valid level.
std::expected<log_level::LogLevel, std::string> log_level_from_string(const std::string &level);

// Opposite of the log_level_from_string, returns the string representation of
// safs::log_level::LogLevel
std::string log_level_to_string(log_level::LogLevel level);

} // namespace safs

// NOTICE(sarna) Old interface, don't use unless extern-C is needed
extern "C" {

/// Adds custom logging file
bool safs_add_log_file(const char *path, int priority, int max_file_size, int max_file_count);

/// Sets which level triggers immediate log flush (default: CRITICAL)
void safs_set_log_flush_on(int priority);

/// Removes all log files
void safs_drop_all_logs();

bool safs_add_log_syslog();

bool safs_add_log_stderr(int priority);

/*
 * function names may contain following words:
 *   "pretty" -> write pretty prefix to stderr
 *   "silent" -> do not write anything to stderr
 *   "errlog" -> append strerr(errno) to printed message
 *   "attempt" -> instead of pretty prefix based on priority, write prefix suggesting
 *      that something is starting
 */

void safs_pretty_syslog(int priority, const char* format, ...)
		__attribute__ ((__format__ (__printf__, 2, 3)));

void safs_pretty_syslog_attempt(int priority, const char* format, ...)
		__attribute__ ((__format__ (__printf__, 2, 3)));

void safs_pretty_errlog(int priority, const char* format, ...)
		__attribute__ ((__format__ (__printf__, 2, 3)));

void safs_silent_syslog(int priority, const char* format, ...)
		__attribute__ ((__format__ (__printf__, 2, 3)));

void safs_silent_errlog(int priority, const char* format, ...)
		__attribute__ ((__format__ (__printf__, 2, 3)));
} // extern "C"
