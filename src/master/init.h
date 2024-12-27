/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA
   Copyright 2013-2014 EditShare
   Copyright 2013-2015 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ


   SaunaFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   SaunaFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with SaunaFS  If not, see <http://www.gnu.org/licenses/>.
 */

#include "common/platform.h"

#include <sys/syslog.h>
#include <vector>

#include "config/cfg.h"
#include "common/event_loop.h"
#include "common/random.h"
#include "common/run_tab.h"
#include "master/chartsdata.h"
#include "master/datacachemgr.h"
#include "master/exports.h"
#include "master/filesystem.h"
#include "master/hstorage_init.h"
#include "master/masterconn.h"
#include "master/matoclserv.h"
#include "master/matocsserv.h"
#include "master/matomlserv.h"
#include "master/personality.h"
#include "master/topology.h"
#include "metrics/metrics.h"

inline int prometheus_init() {
	if (cfg_getuint8("ENABLE_PROMETHEUS", 0) != 1) {
		safs::log_info(
		    "Prometheus disabled, no Prometheus metrics will be "
		    "gathered");
		return 0;
	}
	metrics::init(cfg_getstr("PROMETHEUS_HOST", "0.0.0.0:9499"));
	eventloop_destructregister(metrics::destroy);
	return 0;
}

/// Functions to call before normal startup
inline const std::vector<RunTab> earlyRunTabs = {
    RunTab{metadataserver::personality_validate, "validate personality"}};

/// Functions to call during normal startup
inline const std::vector<RunTab> runTabs = {
    RunTab{prometheus_init, "prometheus module"},
    // has to be first
    RunTab{hstorage_init, "name storage"},
    // has to be second
    RunTab{metadataserver::personality_init, "personality"},
    RunTab{rnd_init, "random generator"},
    // has to be before 'fs_init' and 'matoclserv_networkinit'
    RunTab{dcm_init, "data cache manager"},
    // has to be before 'fs_init'
    RunTab{matoclserv_sessionsinit, "load stored sessions"},
    RunTab{exports_init, "exports manager"},
    RunTab{topology_init, "net topology module"},
    // the lambda is used to select the correct fs_init overload
    RunTab{[]() { return fs_init(); }, "file system manager"},
    RunTab{chartsdata_init, "charts module"},
    RunTab{masterconn_init, "communication with master server"},
    RunTab{matomlserv_init, "communication with metalogger"},
    RunTab{matocsserv_init, "communication with chunkserver"},
    RunTab{matoclserv_networkinit, "communication with clients"}};

/// Functions to call delayed after the initialization is correct
inline const std::vector<RunTab> lateRunTabs = {};
