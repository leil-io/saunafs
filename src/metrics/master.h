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
