// src/common/scoped_timer.h

#pragma once

#include <chrono>
#include <string>

namespace util {

/**
 * ScopedTimer is a class that measures the time between its creation and
 * destruction.
 * It is a RAII wrapper around std::chrono::high_resolution_clock
 * It logs the elapsed time to syslog
 */
class ScopedTimer {
public:

	enum class TimeUnit {
		TU_SEC = 0,
		TU_MILLI = 1,
		TU_MICRO = 2,
		TU_NANO = 3
	};

	/**
	 * Constructor
	 */
	ScopedTimer();

	/**
	 * Constructor
	 * @param message The message to log, it is prepended with the elapsed time
	 * @param time_unit The time unit to use for the elapsed time
	 */

	void setTimeUnit(TimeUnit time_unit);

	explicit ScopedTimer(const std::string &);

	explicit ScopedTimer(TimeUnit);

	explicit ScopedTimer(const std::string &, TimeUnit);

	/**
	 * Destructor
	 * Logs the elapsed time to syslog
	 */
	virtual ~ScopedTimer();

	std::string sayElapsed();

private:
	std::string message_{"elapsed time"};
	std::chrono::time_point<std::chrono::high_resolution_clock> start_;
	TimeUnit timeUnit_{TimeUnit::TU_SEC};

	void init(const std::string &, TimeUnit);
};

}  // namespace util
