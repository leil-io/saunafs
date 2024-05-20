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
#include <prometheus/family.h>
#endif

namespace metrics {

class Counter {
public:
	enum Key {
		KEY_START = 0,
		CLIENT_RX_PACKETS,
		CLIENT_TX_PACKETS,
		KEY_END,
	};
#ifdef HAVE_PROMETHEUS
	Counter() : counter_(nullptr) {};
	Counter(const prometheus::Labels &labels, prometheus::Family<prometheus::Counter> *family) : counter_(&family->Add(labels)) {};

	static void increment(Key key, double n = 1);

private:
	prometheus::Counter* counter_;
#else
	// Dummy methods for packages without prometheus
	explicit Counter() = default;

	static void increment(Key /*unused*/, double  /*unused*/= 1) {
	}

	static inline Counter get(Counter::Key  /*type*/) {
		return Counter();
	}
#endif
};

void init(const char* host);
void destroy();

} // metrics
