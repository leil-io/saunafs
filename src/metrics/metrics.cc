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
#include <prometheus/detail/builder.h>
#include <prometheus/family.h>
#include <pthread.h>
#include <unistd.h>
#include <exception>
#include <memory>
#endif

#include "metrics.h"
#include "slogger/slogger.h"

constexpr auto THREAD_SLEEP_TIME_MS = 100;


namespace metrics {


// Setting up new metrics:
// 1. Add a MetricCounter type for the metric you want in this file and the
// header file
// 2. Create a setup function, indicating the metric family and counters. See
// setup_client_packets for an example.
// 3. Call that function in prometheus_loop

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)

// These variables are all thread safe for now (They are wrapper classes that
// simply check if metrics are enabled and call a function which is thread safe.
// Metrics are enabled/disabled at startup, so that checked variable doesn't
// change at all during the lifetime of the program.

// Received client packets counter
MetricCounter rx_client_packets;
// Sent client packets counter
MetricCounter tx_client_packets;

std::unique_ptr<std::jthread> metrics_main_thread;

// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

void metrics_destroy() {
	if (metrics_main_thread != nullptr) {
		metrics_main_thread->request_stop();
	}
}

#ifndef HAVE_PROMETHEUS
void metrics_init(const char* /* unused */) {
	safs::log_err(
	    "could not setup prometheus server: Prometheus isn't compiled with "
	    "this program");
}
}
#else

using namespace prometheus;

inline Family<Counter> &setup_family(
    const char *name, const char *help,
    const std::shared_ptr<Registry> &registry) {
	return BuildCounter().Name(name).Help(help).Register(*registry);
}

void setup_client_packets(const std::shared_ptr<Registry>& registry) {
	const char* name = "observed_packets_total_client";
	const char* help = "Number of observed packets from and for client";

	auto &packet_counter = setup_family(name, help, registry);

	rx_client_packets = MetricCounter(
	    &packet_counter.Add({{"protocol", "tcp"}, {"direction", "rx"}}));
	tx_client_packets = MetricCounter(
	    &packet_counter.Add({{"protocol", "tcp"}, {"direction", "tx"}}));

}

void prometheus_loop(const std::stop_token& stop, const char* host) {
	try {
		// create an http server
		Exposer exposer{host};
		const auto registry = std::make_shared<Registry>();

		setup_client_packets(registry);

		exposer.RegisterCollectable(registry);
		safs::log_info("started prometheus server");

		while (!stop.stop_requested()) {
			std::this_thread::sleep_for(std::chrono::milliseconds(THREAD_SLEEP_TIME_MS));
		};
	} catch (std::exception &e) {
		safs::log_err("could not setup prometheus server: {}", e.what());
	}
}

void metrics_init(const char* host) {
	metrics_main_thread = std::make_unique<std::jthread>(std::jthread(prometheus_loop, host));
}

}
#endif
