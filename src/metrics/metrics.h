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
