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

// This is used to indicate for the metrics thread to stop, it's managed by a
// single function (metrics_destroy) and read by prometheus_loop
std::atomic<bool> destroy = false;

std::unique_ptr<std::thread> thread;

// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

void metrics_destroy() {
	if (thread != nullptr) {
		destroy = true;
		thread->join();
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

inline Family<Counter>& setup_family(const char* name, const char* help, const std::shared_ptr<Registry>& registry) {
	return BuildCounter()
	        .Name(name)
	        .Help(help)
	        .Register(*registry);
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

void prometheus_loop(const char* host) {
	try {
		// create an http server
		Exposer exposer{host};
		const auto registry = std::make_shared<Registry>();

		setup_client_packets(registry);

		exposer.RegisterCollectable(registry);
		safs::log_info("started prometheus server");

		// Sleep forever this thread
		while (!destroy) {};
	} catch (std::exception &e) {
		safs::log_err("could not setup prometheus server: {}", e.what());
	}
}

void metrics_init(const char* host) {
	thread = std::make_unique<std::thread>(std::thread(prometheus_loop, host));
}

}
#endif
