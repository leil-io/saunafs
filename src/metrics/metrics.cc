/*

   Copyright 2024 Leil Storage OÜ

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

#ifdef HAVE_PROMETHEUS
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#include <prometheus/counter.h>
#pragma GCC diagnostic pop
#include <prometheus/detail/builder.h>
#include <prometheus/family.h>
#include <prometheus/registry.h>
#include <pthread.h>
#include <unistd.h>
#include <array>
#include <exception>
#include <memory>
#endif

#include "metrics.h"
#include "slogger/slogger.h"

constexpr auto THREAD_SLEEP_TIME_MS = 100;

namespace metrics {

std::unique_ptr<std::jthread>
    metrics_main_thread;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

void destroy() {
	if (metrics_main_thread != nullptr) {
		metrics_main_thread->request_stop();
	}
}

#ifndef HAVE_PROMETHEUS
void init(const char* /* unused */) {
	safs::log_err(
	    "could not setup prometheus server: Prometheus isn't compiled with "
	    "this program");
}
}
#else

prometheus::Family<prometheus::Counter> &setup_family(
    const char *name, const char *help,
    std::shared_ptr<prometheus::Registry> &registry) {
	return prometheus::BuildCounter().Name(name).Help(help).Register(*registry);
}

class Counters {
public:
	Counters() {
		registry = std::make_shared<prometheus::Registry>();
		// NOLINTBEGIN(cppcoreguidelines-prefer-member-initializer)
		packet_counter = &setup_family(
		    "observed_packets_total_client",
		    "Number of observed packets from and for client", registry);
		// NOLINTEND(cppcoreguidelines-prefer-member-initializer)

		// A very hacky way to allow compile time checking if metrics have been
		// set. Any enum value not used will throw a compile time error.
		Counter::Key start = Counter::KEY_START;
		switch (start) {
		case Counter::KEY_START:
		case Counter::CLIENT_RX_PACKETS:
			counters[Counter::CLIENT_RX_PACKETS] = Counter(
			    {{"protocol", "tcp"}, {"direction", "rx"}}, packet_counter);
			[[fallthrough]];
		case Counter::CLIENT_TX_PACKETS:
			counters[Counter::CLIENT_TX_PACKETS] = Counter(
			    {{"protocol", "tcp"}, {"direction", "tx"}}, packet_counter);
		case Counter::KEY_END:
			break;
		}
	}

	std::shared_ptr<prometheus::Registry> get_registry() {
		return registry;
	}

	inline Counter& get(Counter::Key type) {
		// Completely safe, as all values are defined
		return counters[type]; // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
	}

private:
	// Registry, used by packet_counter and counters
	std::shared_ptr<prometheus::Registry> registry;

	// Metric(s)
	prometheus::Family<prometheus::Counter> *packet_counter;

	// All counters
	std::array<Counter, Counter::Key::KEY_END + 1> counters;
};
Counters counters;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

void Counter::increment(Key key, double n) {
	auto counter = counters.get(key);
	if (counter.counter_ != nullptr) { counter.counter_->Increment(n); }
}

void prometheus_loop(const std::stop_token& stop, const char* host) {
	try {
		// create an http server
		prometheus::Exposer exposer{host};

		exposer.RegisterCollectable(counters.get_registry());
		safs::log_info("started prometheus server");

		while (!stop.stop_requested()) {
			std::this_thread::sleep_for(std::chrono::milliseconds(THREAD_SLEEP_TIME_MS));
		};
	} catch (std::exception &e) {
		safs::log_err("could not setup prometheus server: {}", e.what());
	}
}

void init(const char* host) {
	metrics_main_thread = std::make_unique<std::jthread>(std::jthread(prometheus_loop, host));
}

}
#endif
