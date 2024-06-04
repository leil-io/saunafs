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

#include "common/config/config.h"
#include "common/platform.h"

#include <cstdint>
#include <vector>

#include "chunkserver/chartsdata.h"
#include "chunkserver/hddspacemgr.h"
#include "chunkserver/masterconn.h"
#include "chunkserver/network_main_thread.h"
#include "common/random.h"
#include "common/run_tab.h"

using ConfigString = std::pair<std::string, std::string>;
using ConfigInt = std::pair<std::string, int>;
using ConfigInt32 = std::pair<std::string, int32_t>;
using ConfigUint32 = std::pair<std::string, uint32_t>;
using ConfigUint64 = std::pair<std::string, uint32_t>;
using ConfigBool = std::pair<std::string, bool>;

template<typename... Options>
inline void setConfigOptions(Options&&... options) {
    auto config = Config::instance();
    (config->addOption(std::forward<Options>(options)), ...);
}

inline void setDefaultConfig() {
	// clang-format off
    setConfigOptions(
        ConfigString("LABEL", MediaLabelManager::kWildcard),
        ConfigString{"WORKING_USER", "saunafs"},
        ConfigString{"WORKING_GROUP", "saunafs"},
        ConfigString{"SYSLOG_IDENT", "sfschunkserver"},
        ConfigInt{"LOCK_MEMORY", 0},
        ConfigInt32{"NICE_LEVEL", -19},
        ConfigString{"DATA_PATH", DATA_PATH},  // Replace with your actual data path
        ConfigUint32{"MASTER_RECONNECTION_DELAY", 5},
        ConfigString{"BIND_HOST", "*"},
        ConfigString{"MASTER_HOST", "sfsmaster"},
        ConfigUint32{"MASTER_PORT", 9420},
        ConfigUint32{"MASTER_TIMEOUT", 60},
        ConfigString{"CSSERV_LISTEN_HOST", "*"},
        ConfigUint32{"CSSERV_LISTEN_PORT", 9422},
        ConfigUint32{"NR_OF_NETWORK_WORKERS", 1},
        ConfigUint32{"NR_OF_HDD_WORKERS_PER_NETWORK_WORKER", 20},
        ConfigUint32{"BGJOBSCNT_PER_NETWORK_WORKER", 1000},
        ConfigUint32{"READ_AHEAD_KB", 0},
        ConfigUint32{"MAX_READ_BEHIND_KB", 0},
        ConfigString{"HDD_CONF_FILENAME", "/usr/local/etc/saunafs/sfshdd.cfg"},
        ConfigUint64{"HDD_LEAVE_SPACE_DEFAULT", 4 * 1024 * 1024}, // Convert GiB to KiB
        ConfigUint32{"HDD_TEST_FREQ", 10},
        ConfigBool{"HDD_CHECK_CRC_WHEN_READING", true},
        ConfigBool{"HDD_CHECK_CRC_WHEN_WRITING", true},
        ConfigBool{"HDD_ADVISE_NO_CACHE", false},
        ConfigBool{"HDD_PUNCH_HOLES", true},
        ConfigBool{"ENABLE_LOAD_FACTOR", false},
        ConfigUint32{"REPLICATION_BANDWIDTH_LIMIT_KBPS", 102400},
        ConfigUint32{"REPLICATION_TOTAL_TIMEOUT_MS", 60000},
        ConfigUint32{"REPLICATION_CONNECTION_TIMEOUT_MS", 1000},
        ConfigUint32{"REPLICATION_WAVE_TIMEOUT_MS", 500},
        ConfigBool{"PERFORM_FSYNC", true},
        // Deprecated configurations
        ConfigInt32{"BACK_LOGS", 50},  // Deprecated, to be removed
        ConfigUint32{"CSSERV_TIMEOUT", 5}  // Deprecated, to be removed
		);
	// clang-format on
}

/// Functions to call before normal startup
inline const std::vector<RunTab> earlyRunTabs = {};

/// Functions to call during normal startup
inline const std::vector<RunTab> runTabs = {
    RunTab{rnd_init, "random generator"},
    RunTab{initDiskManager, "disk manager"},  // Always before "plugin manager"
    RunTab{loadPlugins, "plugin manager"}, RunTab{hddInit, "hdd space manager"},
    // Has to be before "masterconn"
    RunTab{mainNetworkThreadInit, "main server module"},
    RunTab{masterconn_init, "master connection module"},
    RunTab{chartsdata_init, "charts module"}};

/// Functions to call delayed after the initialization is correct
inline const std::vector<RunTab> lateRunTabs = {
    RunTab{masterconn_init_threads, "master connection module - threads"},
    RunTab{hddLateInit, "hdd space manager - threads"},
    RunTab{mainNetworkThreadInitThreads, "main server module - threads"}};
