#pragma once
#ifdef HAVE_PROMETHEUS
#include <prometheus/counter.h>
#include <prometheus/exposer.h>
#include <prometheus/registry.h>
#endif

namespace metrics {

class MetricCounter {
public:
#ifdef HAVE_PROMETHEUS
	explicit MetricCounter(prometheus::Counter *counter = nullptr) : counter_(counter) {}

	inline void increment(double n = 1) const {
		if (counter_ != nullptr) {
			counter_->Increment(n);
		}
	}

private:
	prometheus::Counter *counter_{};
#else
	// Dummy class for packages without prometheus
	explicit MetricCounter() = default;

	inline void increment(double = 1) const {
	}
#endif
};

void metrics_init(const char* host);
void metrics_destroy();

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)

extern MetricCounter rx_client_packets;
extern MetricCounter tx_client_packets;

// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)
}
