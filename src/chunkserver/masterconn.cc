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

#include "chunkserver/masterconn.h"

#include <netinet/in.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <syslog.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>

#include "chunkserver/bgjobs.h"
#include "chunkserver/hddspacemgr.h"
#include "chunkserver/master_connection.h"
#include "chunkserver/network_main_thread.h"
#include "common/event_loop.h"
#include "common/massert.h"
#include "common/network_address.h"
#include "common/random.h"
#include "common/sockets.h"
#include "config/cfg.h"
#include "devtools/request_log.h"
#include "protocol/SFSCommunication.h"
#include "protocol/cstoma.h"
#include "slogger/slogger.h"

//  From config
static std::string gMasterHost;
static std::string gMasterPort;
static bool gEnableLoadFactor;

static const uint64_t kSendStatusDelay = 5;

// Shared worker pools, with a distinct completion listener for each connection.
static std::shared_ptr<MasterJobPool> gJobPool;
static std::shared_ptr<MasterJobPool> gReplicationJobPool;

/// One metadata server connection and the poll bookkeeping of its job listener.
struct ConnectionSlot {
	std::unique_ptr<MasterConn> connection;
	/// Closed but kept until both pools have no job left for its listener.
	bool retiring = false;
	uint8_t lastLoadFactor = 0;
	int32_t jobPollPosition = -1;
	int32_t replicationPollPosition = -1;
};

/// Slot index is the job-pool listener id; slot 0 is the configured connection and never retires.
static std::array<ConnectionSlot, kMaxMetadataConnections> gConnections;
/// Peers the latest discovery snapshot named; the reconcile step makes the slots match it.
static std::vector<MetadataClusterMember> gDesiredMembers;
/// Completion pipes of the two pools, indexed by listener id like the slots.
static std::vector<int> gJobFDs;
static std::vector<int> gReplicationJobFDs;
/// Retry failed admission on the reconnect tick, while existing connections keep being served.
static bool gDiscoveryAdmissionDeferred = false;

static MasterConn *configuredConnection() { return gConnections.front().connection.get(); }

constexpr uint32_t kDefaultNumberOfWorkers = 10;
constexpr uint32_t kMinNumberOfWorkers = 2;
static uint32_t gNumberOfWorkers = kDefaultNumberOfWorkers;

constexpr uint32_t kDefaultReplicationNumberOfWorkers = 5;
constexpr uint32_t kMinReplicationNumberOfWorkers = 1;
static uint32_t gReplicationNumberOfWorkers = kDefaultReplicationNumberOfWorkers;

static void *gReconnectHook;

//  Stats
static uint32_t stats_maxjobscnt = 0;

void masterconn_stats(uint64_t *bin, uint64_t *bout, uint32_t *maxjobscnt) {
	*bin = 0;
	*bout = 0;
	for (auto &slot : gConnections) {
		if (!slot.connection) { continue; }
		*bin += slot.connection->bytesIn();
		*bout += slot.connection->bytesOut();
		slot.connection->resetStats();
	}

	// Get the stats non dependent on specific connections
	*maxjobscnt = stats_maxjobscnt;
	stats_maxjobscnt = 0;
}

void masterconn_check_hdd_reports() {
	const bool hasRegisteredConnection =
	    std::any_of(gConnections.begin(), gConnections.end(), [](const auto &slot) {
		    return slot.connection && !slot.retiring && slot.connection->isRegistered();
	    });
	if (!hasRegisteredConnection) { return; }

	// Gather shared counters once, then send them to every registered peer.
	if (hddGetAndResetSpaceChanged()) {
		uint64_t usedSpace, totalSpace, deletedUsedSpace, deletedTotalSpace;
		uint32_t chunkCount, deletedChunkCount;
		hddGetTotalSpace(&usedSpace, &totalSpace, &chunkCount, &deletedUsedSpace,
		                 &deletedTotalSpace, &deletedChunkCount);
		for (auto &slot : gConnections) {
			if (slot.connection && !slot.retiring && slot.connection->isRegistered()) {
				slot.connection->createAttachedNoVersionPacket(
				    CSTOMA_SPACE, usedSpace, totalSpace, chunkCount, deletedUsedSpace,
				    deletedTotalSpace, deletedChunkCount);
			}
		}
	}

	// Only the configured connection consumes physical loss and damage reports.
	MasterConn *eptr = configuredConnection();
	uint32_t errorcounter;
	if (eptr->isRegistered()) {
		errorcounter = hddGetAndResetErrorCounter();
		while (errorcounter) {
			eptr->createAttachedNoVersionPacket(CSTOMA_ERROR_OCCURRED);
			errorcounter--;
		}

		const auto chunkBulkSize = gChunkBulkSize.load(std::memory_order_relaxed);

		std::vector<ChunkWithType> chunks_with_type;
		hddGetDamagedChunks(chunks_with_type, chunkBulkSize);
		if (!chunks_with_type.empty()) {
			eptr->createAttachedPacket(cstoma::chunkDamaged::build(chunks_with_type));
		}

		hddGetLostChunks(chunks_with_type, chunkBulkSize);
		if (!chunks_with_type.empty()) {
			eptr->createAttachedPacket(cstoma::chunkLost::build(chunks_with_type));
		}

		std::vector<ChunkWithVersionAndType> chunks_with_version;
		hddGetNewChunks(chunks_with_version, chunkBulkSize);
		if (eptr->sendsInventory() && !chunks_with_version.empty()) {
			eptr->createAttachedPacket(cstoma::chunkNew::build(chunks_with_version));
		}
	}
}

void masterconn_unwantedjobfinished(uint8_t status, void *packet) {
	(void)status;
	MasterConn::deletePacket(packet);
}

JobPool::JobCallback masterconn_jobDeleteAfterErrorFinished(ChunkWithType chunkWithType) {
	return [chunkWithType](uint8_t status, void *packet) {
		(void)packet;
		// packet should be nullptr

		if (status == SAUNAFS_STATUS_OK) {
			// Queued whatever the connection is doing. The released code only polled completions
			// while connected, so this report always reached the metadata server, late; these
			// pools drain while disconnected, so skipping it here would lose it instead.
			hddReportLostChunk(chunkWithType.id, chunkWithType.type);
		}
	};
}

std::function<void(uint8_t, void *)> masterconn_unwantedLockJobFinished(ChunkWithType chunkWithType,
                                                                        uint32_t listenerId) {
	return [chunkWithType, listenerId](uint8_t status, void *packet) {
		MasterConn::deletePacket(packet);

		if (status == SAUNAFS_STATUS_OK) { return; }

		// If there was an error while writing, which is passed to the callback as status, we want
		// to remove the chunk itself and avoid registering it again with the master server, as it
		// might contain broken data. To do that, we add a delete job to the job pool, which will be
		// processed and will remove the chunk from the chunk server.
		job_delete(*gJobPool, masterconn_jobDeleteAfterErrorFinished(chunkWithType), nullptr,
		           chunkWithType.id, 0, chunkWithType.type, listenerId);
	};
}

MasterJobPool *masterconn_get_job_pool() { return gJobPool.get(); }

bool masterconn_canexit() {
	// A chunkserver with nothing established may exit whatever is still queued, since none of it
	// can be delivered; this is the single-connection rule applied to the whole set.
	const bool anyConnected =
	    std::any_of(gConnections.begin(), gConnections.end(), [](const auto &slot) {
		    return slot.connection && slot.connection->mode() == ConnectionMode::CONNECTED;
	    });
	if (!anyConnected) { return true; }

	if (!gJobPool->isEmpty() || !gReplicationJobPool->isEmpty()) { return false; }
	return std::all_of(gConnections.begin(), gConnections.end(), [](const auto &slot) {
		return !slot.connection || slot.connection->mode() != ConnectionMode::CONNECTED ||
		       slot.connection->isOutputQueueEmpty();
	});
}

void masterconn_term(void) {
	for (auto &slot : gConnections) {
		if (!slot.connection) { continue; }
		slot.connection->setMode(ConnectionMode::KILL);
		slot.connection->releaseResources();
		slot.connection.reset();
	}

	//  Now reset the last reference to the job pools.
	gReplicationJobPool.reset();
	gJobPool.reset();
}

void masterconn_desc(std::vector<pollfd> &pdesc) {
	LOG_AVG_TILL_END_OF_SCOPE0("master_desc");
	for (uint32_t index = 0; index < gConnections.size(); ++index) {
		auto &slot = gConnections[index];
		slot.jobPollPosition = -1;
		slot.replicationPollPosition = -1;
		if (!slot.connection) { continue; }

		// Keep draining completions even after the socket has closed.
		pdesc.emplace_back(gJobFDs[index], POLLIN, 0);
		slot.jobPollPosition = static_cast<int32_t>(pdesc.size() - 1);
		pdesc.emplace_back(gReplicationJobFDs[index], POLLIN, 0);
		slot.replicationPollPosition = static_cast<int32_t>(pdesc.size() - 1);
		slot.connection->providePollDescriptors(pdesc, doTerminate());
	}
}

void masterconn_send_status() {
	if (gEnableLoadFactor) {
		uint8_t load_factor = hddGetLoadFactor();
		for (auto &slot : gConnections) {
			if (!slot.connection || slot.retiring) { continue; }

			// The configured connection keeps the released rule, any connected socket; a
			// discovered one has nothing to say before it is registered.
			const bool ready = slot.connection->isConfigured()
			                       ? slot.connection->mode() == ConnectionMode::CONNECTED
			                       : slot.connection->isRegistered();
			if (ready && slot.lastLoadFactor != load_factor) {
				slot.connection->createAttachedPacket(cstoma::status::build(load_factor));
				slot.lastLoadFactor = load_factor;
			}
		}
	}
}

void masterconn_close_connection(MasterJobPool &jobPool, MasterJobPool &replicationJobPool,
                                 MasterConn &connection, uint32_t listenerId) {
	jobPool.disableAndChangeCallbackAll(masterconn_unwantedjobfinished, listenerId);
	jobPool.changeLockJobsCallback(masterconn_unwantedLockJobFinished, listenerId);
	replicationJobPool.disableAndChangeCallbackAll(masterconn_unwantedjobfinished, listenerId);
	connection.closeSocketQuietly();
	if (!connection.sendsInventory()) { connection.requeueUnsentReports(); }
	connection.resetPackets();
	connection.setMode(ConnectionMode::FREE);
}

static void closeConnection(uint32_t index) {
	masterconn_close_connection(*gJobPool, *gReplicationJobPool, *gConnections[index].connection,
	                            index);
}

bool masterconn_prepare_listeners(MasterJobPool &jobPool, MasterJobPool &replicationJobPool,
                                  uint32_t listenerId, int &jobDescriptor,
                                  int &replicationDescriptor) {
	try {
		const int jobs = jobPool.allocateListener(listenerId);
		const int replications = replicationJobPool.allocateListener(listenerId);
		if (jobs < 0 || replications < 0) { return false; }
		jobDescriptor = jobs;
		replicationDescriptor = replications;
		return true;
	} catch (const std::exception &exception) {
		safs::log_warn("Deferring peer admission: {}", exception.what());
		return false;
	}
}

/// Brings the discovered connections in line with the latest snapshot: retire peers no longer
/// named, then admit new ones into free slots.
static void reconcileConnections() {
	if (auto snapshot = configuredConnection()->takeClusterSnapshot()) {
		gDesiredMembers = std::move(snapshot->members);
	}
	// A metadata server that took the inventory runs alone; forget any earlier discovery.
	if (configuredConnection()->isRegistered() && configuredConnection()->sendsInventory()) {
		gDesiredMembers.clear();
	}

	// Retire removed or changed peers before admitting replacements into unused listener slots.
	for (uint32_t index = 1; index < gConnections.size(); ++index) {
		auto &slot = gConnections[index];
		if (!slot.connection) { continue; }
		const auto wanted = std::find_if(
		    gDesiredMembers.begin(), gDesiredMembers.end(), [&slot](const auto &member) {
			    return member.serverId == slot.connection->serverId() &&
			           NetworkAddress(member.ip, member.port) == slot.connection->address();
		    });
		if (!slot.retiring && wanted == gDesiredMembers.end()) {
			slot.retiring = true;
			slot.connection->setMode(ConnectionMode::KILL);
			closeConnection(index);
		}
		if (slot.retiring && gJobPool->isListenerIdle(index) &&
		    gReplicationJobPool->isListenerIdle(index)) {
			slot = ConnectionSlot{};
		}
	}

	// Admit named peers this process does not talk to yet, one per free slot.
	if (gDiscoveryAdmissionDeferred) { return; }
	for (const auto &member : gDesiredMembers) {
		const auto existing =
		    std::find_if(gConnections.begin(), gConnections.end(), [&member](const auto &slot) {
			    return slot.connection && slot.connection->serverId() == member.serverId;
		    });
		if (existing != gConnections.end()) { continue; }
		const auto available = std::find_if(gConnections.begin() + 1, gConnections.end(),
		                                    [](const auto &slot) { return !slot.connection; });
		if (available == gConnections.end()) { break; }
		const auto index = static_cast<uint32_t>(available - gConnections.begin());
		if (!masterconn_prepare_listeners(*gJobPool, *gReplicationJobPool, index, gJobFDs[index],
		                                  gReplicationJobFDs[index])) {
			gDiscoveryAdmissionDeferred = true;
			break;
		}
		available->connection = std::make_unique<MasterConn>(
		    ipToString(member.ip), std::to_string(member.port), configuredConnection()->clusterId(),
		    gJobPool, gReplicationJobPool, index, member.serverId);
		available->connection->setMasterAddress(member.ip, member.port);
		available->connection->initConnect();
	}
}

void masterconn_serve(const std::vector<pollfd> &pdesc) {
	LOG_AVG_TILL_END_OF_SCOPE0("master_serve");
	for (uint32_t index = 0; index < gConnections.size(); ++index) {
		auto &slot = gConnections[index];
		if (!slot.connection) { continue; }
		auto &connection = *slot.connection;
		connection.handlePollErrors(pdesc);
		if (connection.mode() == ConnectionMode::KILL) { closeConnection(index); }

		if (slot.jobPollPosition >= 0 && (pdesc[slot.jobPollPosition].revents & POLLIN)) {
			gJobPool->processCompletedJobs(index);
		}
		if (slot.replicationPollPosition >= 0 &&
		    (pdesc[slot.replicationPollPosition].revents & POLLIN)) {
			gReplicationJobPool->processCompletedJobs(index);
		}
		connection.servePoll(pdesc);
		if (connection.mode() == ConnectionMode::KILL) { closeConnection(index); }
	}

	stats_maxjobscnt =
	    std::max(stats_maxjobscnt, gJobPool->getJobCount() + gReplicationJobPool->getJobCount());
	// Snapshot handlers only stage data; no connection is added while poll entries are in use.
	if (!doTerminate()) { reconcileConnections(); }
}

void masterconn_reconnect(void) {
	gDiscoveryAdmissionDeferred = false;
	for (auto &slot : gConnections) {
		if (slot.connection && !slot.retiring && slot.connection->mode() == ConnectionMode::FREE) {
			slot.connection->initConnect();
		}
	}
}

static uint32_t get_cfg_timeout() {
	return 1000 * cfg_get_minmaxvalue<double>("MASTER_TIMEOUT", 60, 0.01, 1000 * 1000);
}

/// Read the label from configuration file and return true if it's changed to a valid one
bool masterconn_load_label() {
	std::string oldLabel = gLabel;
	gLabel = cfg_getstring("LABEL", MediaLabelManager::kWildcard);
	if (!MediaLabelManager::isLabelValid(gLabel)) {
		safs::log_warn("invalid label '{}'", gLabel);
		return false;
	}
	return gLabel != oldLabel;
}

void masterconn_reload(void) {
	//  Read the common configuration from file.
	gBindHostStr = cfg_getstring("BIND_HOST", "*");
	gEnableLoadFactor = static_cast<bool>(cfg_getuint32("ENABLE_LOAD_FACTOR", 0));

	uint32_t bindIp = 0;
	if (tcpresolve(gBindHostStr.c_str(), nullptr, &bindIp, nullptr, 1) < 0) { bindIp = 0; }
	gTimeout_ms = get_cfg_timeout();
	const bool labelChanged = masterconn_load_label();

	// For each connection, reload the configuration and reconnect if needed.
	for (auto &slot : gConnections) {
		if (!slot.connection || slot.retiring) { continue; }
		auto &connection = *slot.connection;
		if (connection.isMasterAddressValid() && connection.mode() != ConnectionMode::FREE) {
			if (connection.bindHostAddress().ip != bindIp) {
				connection.setBindHostAddress(bindIp, connection.bindHostAddress().port);
				connection.setMode(ConnectionMode::KILL);
			}
			connection.reloadConfig();
		} else {
			connection.setMasterAddressValid(false);
		}
		// A discovered connection has nothing to say before it is registered; the configured one
		// keeps the released behaviour of sending on any connected socket.
		if (!connection.isConfigured() && !connection.isRegistered()) { continue; }
		if (labelChanged) { connection.sendRegisterLabel(); }
		connection.sendConfig();
	}

	uint32_t reconnectionDelay = cfg_getuint32("MASTER_RECONNECTION_DELAY", 5);
	eventloop_timechange(gReconnectHook, TIMEMODE_RUN_LATE, reconnectionDelay, 0);
}

int masterconn_init(void) {
	// Read the configuration
	uint32_t reconnectionDelay = cfg_getuint32("MASTER_RECONNECTION_DELAY", 5);
	gMasterHost = cfg_getstring("MASTER_HOST", "sfsmaster");
	gMasterPort = cfg_getstring("MASTER_PORT", "9420");
	std::string clusterId = cfg_getstring("CLUSTER_ID", "default");
	gBindHostStr = cfg_getstring("BIND_HOST", "*");
	gTimeout_ms = get_cfg_timeout();
	gEnableLoadFactor = static_cast<bool>(cfg_getuint32("ENABLE_LOAD_FACTOR", 0));

	if (!masterconn_load_label()) { return -1; }

	// The configured seed supplies the first discovery snapshot after registration.
	gConnections.front().connection = std::make_unique<MasterConn>(
	    gMasterHost, gMasterPort, clusterId, gJobPool, gReplicationJobPool);
	MasterConn *eptr = configuredConnection();
	passert(eptr);

	// Init the connections
	if (eptr->initConnect() < 0) { return -1; }

	// Register the callbacks in the event loop
	eventloop_eachloopregister(masterconn_check_hdd_reports);
	eventloop_timeregister(TIMEMODE_RUN_LATE, kSendStatusDelay,
	                       rnd_ranged<uint32_t>(kSendStatusDelay), masterconn_send_status);
	gReconnectHook =
	    eventloop_timeregister(TIMEMODE_RUN_LATE, reconnectionDelay,
	                           rnd_ranged<uint32_t>(reconnectionDelay), masterconn_reconnect);

	eventloop_destructregister(masterconn_term);
	eventloop_pollregister(masterconn_desc, masterconn_serve);
	eventloop_reloadregister(masterconn_reload);

	return 0;
}

int masterconn_init_threads(void) {
	gNumberOfWorkers = cfg_get_minvalue<uint32_t>("MASTER_NR_OF_WORKERS", kDefaultNumberOfWorkers,
	                                              kMinNumberOfWorkers);

	try {
		gJobFDs.assign(kMaxMetadataConnections, -1);
		std::vector<int> initFDs;
		gJobPool = std::make_shared<MasterJobPool>("ma", gNumberOfWorkers, kMaxBackgroundJobsCount,
		                                           1, initFDs);
		if (!initFDs.empty()) { gJobFDs[0] = initFDs[0]; }
	} catch (const std::exception &e) {
		safs::log_err("masterconn_init_threads: Failed to create JobPool instance: {}", e.what());
		return -1;
	}

	if (gJobPool == nullptr) {
		safs::log_err("masterconn_init_threads: jobPool is null. Unable to create worker threads.");
		return -1;
	}

	safs::log_info("master connection: {} background workers created", gNumberOfWorkers);

	gReplicationNumberOfWorkers = cfg_get_minvalue<uint32_t>("MASTER_REPLICATION_NR_OF_WORKERS",
	                                                         kDefaultReplicationNumberOfWorkers,
	                                                         kMinReplicationNumberOfWorkers);

	try {
		gReplicationJobFDs.assign(kMaxMetadataConnections, -1);
		std::vector<int> initReplFDs;
		gReplicationJobPool = std::make_shared<MasterJobPool>(
		    "ma_repl", gReplicationNumberOfWorkers, kMaxBackgroundJobsCount, 1, initReplFDs);
		if (!initReplFDs.empty()) { gReplicationJobFDs[0] = initReplFDs[0]; }
	} catch (const std::exception &e) {
		safs::log_err("masterconn_init_threads: Failed to create ReplicationJobPool instance: {}",
		              e.what());
		return -1;
	}

	if (gReplicationJobPool == nullptr) {
		safs::log_err(
		    "masterconn_init_threads: replicationJobPool is null. Unable to create "
		    "replication worker threads.");
		return -1;
	}

	safs::log_info("master connection: {} replication background workers created",
	               gReplicationNumberOfWorkers);

	return 0;
}
