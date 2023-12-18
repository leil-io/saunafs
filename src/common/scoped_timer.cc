// src/common/scoped_timer.cc

#include <sys/syslog.h>

#include "scoped_timer.h"
#include "slogger.h"

namespace util {

void ScopedTimer::setTimeUnit(TimeUnit timeUnit) { timeUnit_ = timeUnit; }

void ScopedTimer::init(const std::string &message, TimeUnit timeUnit) {
	start_ = std::chrono::high_resolution_clock::now();
	message_ = message;
	timeUnit_ = timeUnit;
}

ScopedTimer::ScopedTimer(const std::string &message, TimeUnit timeUnit) {
	init(message, timeUnit);
}

ScopedTimer::ScopedTimer() { init("elapsed time", TimeUnit::TU_SEC); }

ScopedTimer::ScopedTimer(const std::string &message) {
	init(message, TimeUnit::TU_SEC);
}

ScopedTimer::ScopedTimer(TimeUnit timeUnit) { init("elapsed time", timeUnit); }

ScopedTimer::~ScopedTimer() {
	safs_pretty_syslog(LOG_INFO, "%s", sayElapsed().c_str());
}

std::string ScopedTimer::sayElapsed() {
	auto end = std::chrono::high_resolution_clock::now();
	auto elapsed = end - start_;
	int64_t elapsed_time;
	std::string unit;
	switch (timeUnit_) {
	case TimeUnit::TU_NANO:
		elapsed_time =
		    std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed)
		        .count();
		unit = "ns";
		break;
	case TimeUnit::TU_MICRO:
		elapsed_time =
		    std::chrono::duration_cast<std::chrono::microseconds>(elapsed)
		        .count();
		unit = "us";
		break;
	case TimeUnit::TU_MILLI:
		elapsed_time =
		    std::chrono::duration_cast<std::chrono::milliseconds>(elapsed)
		        .count();
		unit = "ms";
		break;
	case TimeUnit::TU_SEC:
	default:
		elapsed_time =
		    std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
		unit = "s";
		break;
	}
	return message_ + ": " + std::to_string(elapsed_time) + unit;
}

}  // namespace util