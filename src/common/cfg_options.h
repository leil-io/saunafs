#pragma once

#include <string>
#include <unordered_map>
#include "common/platform.h"

// Empty admin password
#define ADMIN_PASSWORD {"ADMIN_PASSWORD", ""},

// Auto recovery mode off
#define AUTO_RECOVERY {"AUTO_RECOVERY", "0"},

// Avoid assigning chunks to servers with the same IP
#define AVOID_SAME_IP_CHUNKSERVERS {"AVOID_SAME_IP_CHUNKSERVERS", "0"},

// Number of backlog connections
#define BACK_LOGS {"BACK_LOGS", "50"},

// Keep previous metadata backups
#define BACK_META_KEEP_PREVIOUS {"BACK_META_KEEP_PREVIOUS", "3"},

// BDB cache size for name storage
#define BDB_NAME_STORAGE_CACHE_SIZE {"BDB_NAME_STORAGE_CACHE_SIZE", "10"},

// Bind host for all interfaces
#define BIND_HOST {"BIND_HOST", "*"},

// Hard deletion limit for chunks
#define CHUNKS_HARD_DEL_LIMIT {"CHUNKS_HARD_DEL_LIMIT", "25"},

// Read replication limit for chunks
#define CHUNKS_READ_REP_LIMIT {"CHUNKS_READ_REP_LIMIT", "10"},

// Rebalancing chunks between labels off
#define CHUNKS_REBALANCING_BETWEEN_LABELS {"CHUNKS_REBALANCING_BETWEEN_LABELS", "0"},

// Soft deletion limit for chunks
#define CHUNKS_SOFT_DEL_LIMIT {"CHUNKS_SOFT_DEL_LIMIT", "10"},

// Write replication limit for chunks
#define CHUNKS_WRITE_REP_LIMIT {"CHUNKS_WRITE_REP_LIMIT", "2"},

// Listen host for CS (Chunk Server)
#define CSSERV_LISTEN_HOST {"CSSERV_LISTEN_HOST", "*"},

// Listen port for CS (Chunk Server)
#define CSSERV_LISTEN_PORT {"CSSERV_LISTEN_PORT", "9422"},

// Filename for custom goals, empty by default
#define CUSTOM_GOALS_FILENAME {"CUSTOM_GOALS_FILENAME", ""},

// Path for data storage
#define CFG_DATA_PATH {"DATA_PATH", DATA_PATH},

// Disable chunk deletion
#define DISABLE_CHUNKS_DEL {"DISABLE_CHUNKS_DEL", "0"},

// Disable metadata checksum verification
#define DISABLE_METADATA_CHECKSUM_VERIFICATION {"DISABLE_METADATA_CHECKSUM_VERIFICATION", "0"},

// Load factor balancing disabled
#define ENABLE_LOAD_FACTOR {"ENABLE_LOAD_FACTOR", "0"},

// Maximum capacity for endangered chunks
#define ENDANGERED_CHUNKS_MAX_CAPACITY {"ENDANGERED_CHUNKS_MAX_CAPACITY", "1048576"},

// Exports configuration file path
#define EXPORTS_FILENAME {"EXPORTS_FILENAME", ETC_PATH "/sfsexports.cfg"},

// File creation mask
#define FILE_UMASK {"FILE_UMASK", "027"},

// Global IO limits file, empty by default
#define GLOBALIOLIMITS_FILENAME {"GLOBALIOLIMITS_FILENAME", ""},

// Advise no cache for HDD access off
#define HDD_ADVISE_NO_CACHE {"HDD_ADVISE_NO_CACHE", "0"},

// Check CRC when reading from HDD on
#define HDD_CHECK_CRC_WHEN_READING {"HDD_CHECK_CRC_WHEN_READING", "1"},

// HDD hole punching off
#define HDD_PUNCH_HOLES {"HDD_PUNCH_HOLES", "0"},

// Wildcard label for media
#define LABEL {"LABEL", "_"},

// Lock memory option off
#define LOCK_MEMORY {"LOCK_MEMORY", "0"},

// Flush logs on critical events
#define LOG_FLUSH_ON {"LOG_FLUSH_ON", "CRITICAL"},

// Auto file repair off
#define MAGIC_AUTO_FILE_REPAIR {"MAGIC_AUTO_FILE_REPAIR", "0"},

// Prefer background dump off
#define MAGIC_PREFER_BACKGROUND_DUMP {"MAGIC_PREFER_BACKGROUND_DUMP", "0"},

// Default master host
#define MASTER_HOST {"MASTER_HOST", "sfsmaster"},

// Master port for shadow to connect
#define MASTER_PORT_SHADOW {"MASTER_PORT", "9419"},

// Master port for metalogger to connect
#define MASTER_PORT_METALOGGER {"MASTER_PORT", "9419"},

// Master port for chunkserver to connect
#define MASTER_PORT_CHUNKSERVER {"MASTER_PORT", "9420"},

// Master reconnection delay for metalogger/shadow
#define MASTER_RECONNECTION_DELAY {"MASTER_RECONNECTION_DELAY", "1"},

// Master reconnection delay for chunkserver
#define MASTER_RECONNECTION_DELAY_CHUNKSERVER {"MASTER_RECONNECTION_DELAY", "5"},

// Master connection timeout
#define MASTER_TIMEOUT {"MASTER_TIMEOUT", "60"},

// Listen host for MatoCL
#define MATOCL_LISTEN_HOST {"MATOCL_LISTEN_HOST", "*"},

// Listen port for MatoCL
#define MATOCL_LISTEN_PORT {"MATOCL_LISTEN_PORT", "9421"},

// Listen host for MatoCS
#define MATOCS_LISTEN_HOST {"MATOCS_LISTEN_HOST", "*"},

// Listen port for MatoCS
#define MATOCS_LISTEN_PORT {"MATOCS_LISTEN_PORT", "9420"},

// Alternative name of sfshdd.cfg
#define HDD_CONF_FILENAME {"HDD_CONF_FILENAME", ETC_PATH "/sfshdd.cfg"},

// Listen host for MatoCU
#define MATOCU_LISTEN_HOST {"MATOCU_LISTEN_HOST", "*"},

// Listen port for MatoCU
#define MATOCU_LISTEN_PORT {"MATOCU_LISTEN_PORT", "9421"},

// Listen host for MatoML
#define MATOML_LISTEN_HOST {"MATOML_LISTEN_HOST", "*"},

// Listen port for MatoML
#define MATOML_LISTEN_PORT {"MATOML_LISTEN_PORT", "9419"},

// Log preserve time for MatoML
#define MATOML_LOG_PRESERVE_SECONDS {"MATOML_LOG_PRESERVE_SECONDS", "600"},

// Listen host for MatoTS
#define MATOTS_LISTEN_HOST {"MATOTS_LISTEN_HOST", "*"},

// Listen port for MatoTS
#define MATOTS_LISTEN_PORT {"MATOTS_LISTEN_PORT", "9424"},

// Metadata checksum calculation interval
#define METADATA_CHECKSUM_INTERVAL {"METADATA_CHECKSUM_INTERVAL", "50"},

// Speed of metadata checksum recalculation
#define METADATA_CHECKSUM_RECALCULATION_SPEED {"METADATA_CHECKSUM_RECALCULATION_SPEED", "100"},

// Minimum period between metadata save requests
#define METADATA_SAVE_REQUEST_MIN_PERIOD {"METADATA_SAVE_REQUEST_MIN_PERIOD", "1800"},

// Frequency of metadata download
#define META_DOWNLOAD_FREQ {"META_DOWNLOAD_FREQ", "24"},

// Nice level for process priority
#define NICE_LEVEL {"NICE_LEVEL", "-19"},

// Disable atime updates
#define NO_ATIME {"NO_ATIME", "0"},

// Delay for disconnecting operations
#define OPERATIONS_DELAY_DISCONNECT {"OPERATIONS_DELAY_DISCONNECT", "3600"},

// Initialization delay for operations
#define OPERATIONS_DELAY_INIT {"OPERATIONS_DELAY_INIT", "300"},

// Force fsync after each write
#define PERFORM_FSYNC {"PERFORM_FSYNC", "1"},

// Personality for master role
#define PERSONALITY {"PERSONALITY", "master"},

// Prefer local chunkserver for reads
#define PREFER_LOCAL_CHUNKSERVER {"PREFER_LOCAL_CHUNKSERVER", "1"},

// Redundancy level off
#define REDUNDANCY_LEVEL {"REDUNDANCY_LEVEL", "0"},

// Reject old clients off
#define REJECT_OLD_CLIENTS {"REJECT_OLD_CLIENTS", "0"},

// Replication bandwidth limit off
#define REPLICATION_BANDWIDTH_LIMIT_KBPS {"REPLICATION_BANDWIDTH_LIMIT_KBPS", "0"},

// Delay for disconnecting replications
#define REPLICATIONS_DELAY_DISCONNECT {"REPLICATIONS_DELAY_DISCONNECT", "3600"},

// Initialization delay for replications
#define REPLICATIONS_DELAY_INIT {"REPLICATIONS_DELAY_INIT", "300"},

// Session sustain time
#define SESSION_SUSTAIN_TIME {"SESSION_SUSTAIN_TIME", "86400"},

// Path for SFS MetaRestore
#define SFSMETARESTORE_PATH {"SFSMETARESTORE_PATH", SBIN_PATH "/sfsmetarestore"},

// Initial batch size for snapshots
#define SNAPSHOT_INITIAL_BATCH_SIZE {"SNAPSHOT_INITIAL_BATCH_SIZE", "1000"},

// Limit for initial batch size of snapshots
#define SNAPSHOT_INITIAL_BATCH_SIZE_LIMIT {"SNAPSHOT_INITIAL_BATCH_SIZE_LIMIT", "10000"},

// Syslog identifier for master
#define SYSLOG_IDENT_MASTER {"SYSLOG_IDENT", "sfsmaster"},

// Syslog identifier for chunkserver
#define SYSLOG_IDENT_CHUNKSERVER {"SYSLOG_IDENT", "sfschunkserver"},

// Syslog identifier for metalogger
#define SYSLOG_IDENT_METALOGGER {"SYSLOG_IDENT", "sfsmetalogger"},

// Topology configuration file path
#define TOPOLOGY_FILENAME {"TOPOLOGY_FILENAME", ETC_PATH "/sfstopology.cfg"},

// Use BDB for name storage off
#define USE_BDB_FOR_NAME_STORAGE {"USE_BDB_FOR_NAME_STORAGE", "0"},

// Use BDB name storage off
#define USE_BDB_NAME_STORAGE {"USE_BDB_NAME_STORAGE", "0"},

// Default working group
#define WORKING_GROUP {"WORKING_GROUP", DEFAULT_GROUP},

// Default working user
#define WORKING_USER {"WORKING_USER", DEFAULT_USER},

// HDD test frequency
#define HDD_TEST_FREQ {"HDD_TEST_FREQ", "10.0"},

// Endangered chunks priority, default
#define ENDANGERED_CHUNKS_PRIORITY {"ENDANGERED_CHUNKS_PRIORITY", "0.0"},

// Acceptable difference threshold
#define ACCEPTABLE_DIFFERENCE {"ACCEPTABLE_DIFFERENCE", "0.1"},

// Number of network workers
#define NR_OF_NETWORK_WORKERS {"NR_OF_NETWORK_WORKERS", "1"},

// Number of HDD workers per network worker
#define NR_OF_HDD_WORKERS_PER_NETWORK_WORKER {"NR_OF_HDD_WORKERS_PER_NETWORK_WORKER", "2"},

// Background jobs count per network worker
#define BGJOBSCNT_PER_NETWORK_WORKER {"BGJOBSCNT_PER_NETWORK_WORKER", "1000"},

// Global IO limits accumulation time in milliseconds
#define GLOBALIOLIMITS_ACCUMULATE_MS {"GLOBALIOLIMITS_ACCUMULATE_MS", "250"},

// Period for renegotiating global IO limits in seconds
#define GLOBALIOLIMITS_RENEGOTIATION_PERIOD_SECONDS {"GLOBALIOLIMITS_RENEGOTIATION_PERIOD_SECONDS", "0.1"},

// Read-ahead size in KB, disabled
#define READ_AHEAD_KB {"READ_AHEAD_KB", "0"},

// Maximum size for reading behind in KB, disabled
#define MAX_READ_BEHIND_KB {"MAX_READ_BEHIND_KB", "0"},

// Master connection timeout, repeated
#define MASTER_TIMEOUT_REPEAT {"MASTER_TIMEOUT", "60"},

// Default total timeout for replication in milliseconds
#define REPLICATION_TOTAL_TIMEOUT_MS {"REPLICATION_TOTAL_TIMEOUT_MS", "60000"},

// Default wave timeout for replication in milliseconds
#define REPLICATION_WAVE_TIMEOUT_MS {"REPLICATION_WAVE_TIMEOUT_MS", "500"},

// Default connection timeout for replication in milliseconds
#define REPLICATION_CONNECTION_TIMEOUT_MS {"REPLICATION_CONNECTION_TIMEOUT_MS", "1000"},

// Chunks loop period in milliseconds
#define CHUNKS_LOOP_PERIOD {"CHUNKS_LOOP_PERIOD", "1000"},

// Maximum CPU usage for chunks loop
#define CHUNKS_LOOP_MAX_CPU {"CHUNKS_LOOP_MAX_CPU", "60"},

// Time for chunks loop in milliseconds
#define CHUNKS_LOOP_TIME {"CHUNKS_LOOP_TIME", "300"},

// Minimum time for chunks loop in milliseconds
#define CHUNKS_LOOP_MIN_TIME {"CHUNKS_LOOP_MIN_TIME", "300"},

// Maximum chunks per second processing rate
#define CHUNKS_LOOP_MAX_CPS {"CHUNKS_LOOP_MAX_CPS", "100000"},

// Chunks loop period repeated
#define CHUNKS_LOOP_PERIOD_REPEAT {"CHUNKS_LOOP_PERIOD", "1000"},

// Minimum time for file test loop in seconds
#define FILE_TEST_LOOP_MIN_TIME {"FILE_TEST_LOOP_MIN_TIME", "3600"},

// Load factor penalty off
#define LOAD_FACTOR_PENALTY {"LOAD_FACTOR_PENALTY", "0.0"},

const static std::unordered_map<std::string, std::string> gDefaultOptionsMaster = {
	PERSONALITY
	CFG_DATA_PATH
	WORKING_USER
	WORKING_GROUP
	SYSLOG_IDENT_MASTER
	LOCK_MEMORY
	NICE_LEVEL
	EXPORTS_FILENAME
	TOPOLOGY_FILENAME
	CUSTOM_GOALS_FILENAME
	PREFER_LOCAL_CHUNKSERVER
	BACK_LOGS
	BACK_META_KEEP_PREVIOUS
	AUTO_RECOVERY
	REPLICATIONS_DELAY_INIT
	REPLICATIONS_DELAY_DISCONNECT
	OPERATIONS_DELAY_INIT
	OPERATIONS_DELAY_DISCONNECT
	MATOML_LISTEN_HOST
	MATOML_LISTEN_PORT
	MATOML_LOG_PRESERVE_SECONDS
	MATOCS_LISTEN_HOST
	MATOCS_LISTEN_PORT
	MATOCL_LISTEN_HOST
	MATOCL_LISTEN_PORT
	MATOTS_LISTEN_HOST
	MATOTS_LISTEN_PORT
	CHUNKS_LOOP_MAX_CPS
	CHUNKS_LOOP_MIN_TIME
	CHUNKS_LOOP_PERIOD
	CHUNKS_LOOP_MAX_CPU
	CHUNKS_SOFT_DEL_LIMIT
	CHUNKS_HARD_DEL_LIMIT
	CHUNKS_WRITE_REP_LIMIT
	CHUNKS_READ_REP_LIMIT
	ENDANGERED_CHUNKS_PRIORITY
	ENDANGERED_CHUNKS_MAX_CAPACITY
	ACCEPTABLE_DIFFERENCE
	CHUNKS_REBALANCING_BETWEEN_LABELS
	REJECT_OLD_CLIENTS
	GLOBALIOLIMITS_FILENAME
	GLOBALIOLIMITS_RENEGOTIATION_PERIOD_SECONDS
	GLOBALIOLIMITS_ACCUMULATE_MS
	METADATA_CHECKSUM_INTERVAL
	METADATA_CHECKSUM_RECALCULATION_SPEED
	DISABLE_METADATA_CHECKSUM_VERIFICATION
	NO_ATIME
	METADATA_SAVE_REQUEST_MIN_PERIOD
	SESSION_SUSTAIN_TIME
	USE_BDB_FOR_NAME_STORAGE
	BDB_NAME_STORAGE_CACHE_SIZE
	AVOID_SAME_IP_CHUNKSERVERS
	REDUNDANCY_LEVEL
	SNAPSHOT_INITIAL_BATCH_SIZE
	SNAPSHOT_INITIAL_BATCH_SIZE_LIMIT
	FILE_TEST_LOOP_MIN_TIME
};

const static std::unordered_map<std::string, std::string> gDefaultOptionsShadow = {
	MASTER_HOST
	MASTER_PORT_SHADOW
	MASTER_RECONNECTION_DELAY
	MASTER_TIMEOUT
	LOAD_FACTOR_PENALTY
};


const static std::unordered_map<std::string, std::string> gDefaultOptionsCS = {
	CFG_DATA_PATH
	LABEL
	WORKING_USER
	WORKING_GROUP
	SYSLOG_IDENT_CHUNKSERVER
	LOCK_MEMORY
	NICE_LEVEL
	MASTER_HOST
	MASTER_PORT_CHUNKSERVER
	MASTER_RECONNECTION_DELAY
	MASTER_TIMEOUT
	BIND_HOST
	CSSERV_LISTEN_HOST
	CSSERV_LISTEN_PORT
	HDD_CONF_FILENAME
	HDD_TEST_FREQ
	HDD_CHECK_CRC_WHEN_READING
	HDD_ADVISE_NO_CACHE
	HDD_PUNCH_HOLES
	ENABLE_LOAD_FACTOR
	REPLICATION_BANDWIDTH_LIMIT_KBPS
	NR_OF_NETWORK_WORKERS
	NR_OF_HDD_WORKERS_PER_NETWORK_WORKER
	MAX_READ_BEHIND_KB
	PERFORM_FSYNC
	REPLICATION_TOTAL_TIMEOUT_MS
	REPLICATION_CONNECTION_TIMEOUT_MS
	REPLICATION_WAVE_TIMEOUT_MS
};

const static std::unordered_map<std::string, std::string> gDefaultOptionsMeta = {
	CFG_DATA_PATH
	WORKING_USER
	WORKING_GROUP
	SYSLOG_IDENT_METALOGGER
	LOCK_MEMORY
	NICE_LEVEL
	BACK_LOGS
	BACK_META_KEEP_PREVIOUS
	META_DOWNLOAD_FREQ
	MASTER_HOST
	MASTER_PORT_METALOGGER
	MASTER_RECONNECTION_DELAY
};
