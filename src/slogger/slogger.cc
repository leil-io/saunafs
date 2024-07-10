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

#include "common/platform.h"
#include "slogger/slogger.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>
#include <boost/algorithm/string/case_conv.hpp>
#include <cassert>
#include <string>
#include <expected>

#include "common/cfg.h"
#include "errors/sfserr.h"

static safs::log_level::LogLevel log_level_from_syslog(int priority) {
	static const std::array<safs::log_level::LogLevel, 8> kSyslogToLevel = {{
		safs::log_level::critical, // emerg
		safs::log_level::critical, // alert
		safs::log_level::critical, // critical
		safs::log_level::err,      // error
		safs::log_level::warn,     // warning
		safs::log_level::info,     // notice
		safs::log_level::info,     // info
		safs::log_level::debug,    // debug
	}};
	return kSyslogToLevel[std::min<int>(priority, kSyslogToLevel.size())];
}

std::expected<safs::log_level::LogLevel, std::string>
safs::log_level_from_string(const std::string &level) {
	const auto lower_level = boost::algorithm::to_lower_copy(level);
	if (lower_level == "trace") {
		return safs::log_level::trace;
	}
	if (lower_level == "debug") {
		return safs::log_level::debug;
	}
	if (lower_level == "info") {
		return safs::log_level::info;
	}
	if (lower_level == "warn" || lower_level == "warning") {
		return safs::log_level::warn;
	}
	if (lower_level == "err" || lower_level == "error") {
		return safs::log_level::err;
	}
	if (lower_level == "crit" || lower_level == "critical") {
		return safs::log_level::critical;
	}
	if (lower_level == "off") {
		return safs::log_level::off;
	}
	return std::unexpected<std::string>("Invalid log level: " + level);
}

std::string safs::log_level_to_string(safs::log_level::LogLevel level) {
	using safs::log_level::LogLevel;
	switch (level) {
	case LogLevel::trace:
		return "trace";
	case LogLevel::debug:
		return "debug";
	case LogLevel::info:
		return "info";
	case LogLevel::warn:
		return "warn";
	case LogLevel::err:
		return "err";
	case LogLevel::critical:
		return "crit";
	case LogLevel::off:
		break;
	}
	return "off";
};

bool safs_add_log_file(const char *path, int priority, int max_file_size, int max_file_count) {
	return safs::add_log_file(path, log_level_from_syslog(priority), max_file_size, max_file_count);
}

void safs_set_log_flush_on(int priority) {
	return safs::set_log_flush_on(log_level_from_syslog(priority));
}

void safs_drop_all_logs() {
	return safs::drop_all_logs();
}

bool safs_add_log_stderr(int priority) {
	return safs::add_log_stderr(log_level_from_syslog(priority));
}

bool safs_add_log_syslog() {
	return safs::add_log_syslog();
}

bool safs::add_log_file(const char *path, log_level::LogLevel level, int max_file_size, int max_file_count) {
	try {
		LoggerPtr logger = spdlog::rotating_logger_mt(path, path, max_file_size, max_file_count);
		logger->set_level((spdlog::level::level_enum)level);
		// Format: DATE TIME [LEVEL] [PID:TID] : MESSAGE
		logger->set_pattern("%D %H:%M:%S.%e [%l] [%P:%t] : %v");
#ifdef _WIN32
		logger->flush_on(spdlog::level::err);
#endif
		return true;
	} catch (const spdlog::spdlog_ex &e) {
		safs_pretty_syslog(LOG_ERR, "Adding %s log file failed: %s", path, e.what());
	}
	return false;
}

void safs::set_log_flush_on(log_level::LogLevel level) {
	spdlog::apply_all([level](LoggerPtr l) {l->flush_on((spdlog::level::level_enum)level);});
}

void safs::drop_all_logs() {
	spdlog::drop_all();
}

bool safs::add_log_syslog() {
#ifndef _WIN32
	try {
		spdlog::syslog_logger_mt("syslog");
		return true;
	} catch (const spdlog::spdlog_ex &e) {
		safs_pretty_syslog(LOG_ERR, "Adding syslog log failed: %s", e.what());
	}
#endif
	return false;
}

bool safs::add_log_stderr(log_level::LogLevel level) {
	try {
		LoggerPtr logger = spdlog::stderr_color_mt("stderr");
		logger->set_level((spdlog::level::level_enum)level);
		// Format: DATE TIME [LEVEL] [PID:TID] : MESSAGE
		logger->set_pattern("%D %H:%M:%S.%e [%l] [%P:%t] : %v");
		return true;
	} catch (const spdlog::spdlog_ex &e) {
		safs_pretty_syslog(LOG_ERR, "Adding stderr log failed: %s", e.what());
	}
	return false;
}

static void safs_vsyslog(int priority, const char* format, va_list ap) {
	char buf[1024];
	va_list ap2;
	va_copy(ap2, ap);
	int written = vsnprintf(buf, 1023, format, ap2);
	if (written < 0) {
		va_end(ap2);
		return;
	}
	buf[std::min<int>(written, sizeof(buf))] = '\0';
	va_end(ap2);

	spdlog::apply_all([priority, buf](LoggerPtr l) {
		l->log((spdlog::level::level_enum)log_level_from_syslog(priority), buf);
	});
}

void safs_pretty_syslog(int priority, const char* format, ...) {
	va_list ap;
	va_start(ap, format);
	safs_vsyslog(priority, format, ap);
	va_end(ap);
}

void safs_pretty_syslog_attempt(int priority, const char* format, ...) {
	va_list ap;
	va_start(ap, format);
	safs_vsyslog(priority, format, ap);
	va_end(ap);
}

void safs_pretty_errlog(int priority, const char* format, ...) {
	int err = errno;
	char buffer[1024];
	va_list ap;
	va_start(ap, format);
	int len = vsnprintf(buffer, 1023, format, ap);
	buffer[len] = 0;
	va_end(ap);
	safs_pretty_syslog(priority, "%s: %s", buffer, strerr(err));
	errno = err;
}

void safs_silent_syslog(int priority, const char* format, ...) {
	va_list ap;
	va_start(ap, format);
	safs_vsyslog(priority, format, ap);
	va_end(ap);
}

void safs_silent_errlog(int priority, const char* format, ...) {
	int err = errno;
	char buffer[1024];
	va_list ap;
	va_start(ap, format);
	int len = vsnprintf(buffer, 1023, format, ap);
	buffer[len] = 0;
	va_end(ap);
	safs_silent_syslog(priority, "%s: %s", buffer, strerr(err));
	errno = err;
}
