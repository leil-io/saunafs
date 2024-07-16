#pragma once
#include "metrics/metrics.h"

#ifdef HAVE_PROMETHEUS
#include <prometheus/counter.h>
#include <prometheus/registry.h>

namespace metrics::master {

struct Master {
public:
	Master() = default;
	Master(std::shared_ptr<prometheus::Registry>& registry);

	// Metric(s)
	prometheus::Family<prometheus::Counter> *packet_client_counter{nullptr};
	prometheus::Family<prometheus::Counter> *byte_client_counter{nullptr};
	prometheus::Family<prometheus::Counter> *filesystem_counter{nullptr};
	prometheus::Family<prometheus::Counter> *chunk_counter{nullptr};

	// Master Counters
	std::array<Counter, Counters::KEY_END + 1> master_counters;
};
}
#endif
