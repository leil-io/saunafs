/*
   Copyright 2005-2017 Jakub Kruszona-Zawadzki, Gemius SA
   Copyright 2013-2014 EditShare
   Copyright 2013-2017 Skytechnology sp. z o.o.
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
#include "master/filesystem.h"

#include "config/cfg.h"
#include "common/event_loop.h"
#include "common/lockfile.h"
#include "common/main.h"
#include "common/metadata.h"
#include "common/scoped_timer.h"
#include "master/changelog.h"
#include "master/chunks.h"
#include "master/datacachemgr.h"
#include "master/filesystem_checksum_updater.h"
#include "master/filesystem_metadata.h"
#include "master/filesystem_operations.h"
#include "master/filesystem_periodic.h"
#include "master/filesystem_snapshot.h"
#include "master/filesystem_store.h"
#include "master/goal_config_loader.h"
#include "master/matoclserv.h"
#include "master/matomlserv.h"
#include "master/metadata_dumper.h"
#include "master/restore.h"

#include <fstream>

FilesystemMetadata* gMetadata = nullptr;

#ifndef METARESTORE

static bool gAutoRecovery = false;
bool gMagicAutoFileRepair = false;
bool gAtimeDisabled = false;
MetadataDumper metadataDumper(kMetadataFilename, kMetadataTmpFilename);

uint32_t gTestStartTime;

static bool gSaveMetadataAtExit = true;
static uint32_t gOperationsDelayInit;
static uint32_t gOperationsDelayDisconnect;

// Configuration of goals
std::map<int, Goal> gGoalDefinitions;

#endif // ifndef METARESTORE

// Number of changelog file versions
uint32_t gStoredPreviousBackMetaCopies;

// Checksum validation
bool gDisableChecksumVerification = false;

ChecksumBackgroundUpdater gChecksumBackgroundUpdater;

#ifdef METARESTORE

void fs_disable_checksum_verification(bool value) {
	gDisableChecksumVerification = value;
}

#endif

void fs_unlock() {
	gMetadataLockfile->unlock();
}

#ifndef METARESTORE

static void metadataPollDesc(std::vector<pollfd> &pdesc) {
	metadataDumper.pollDesc(pdesc);
}

static void metadataPollServe(const std::vector<pollfd> &pdesc) {
	bool metadataDumpInProgress = metadataDumper.inProgress();
	metadataDumper.pollServe(pdesc);
	if (metadataDumpInProgress && !metadataDumper.inProgress()) {
		if (metadataDumper.dumpSucceeded()) {
			if (fs_commit_metadata_dump()) {
				fs_broadcast_metadata_saved(SAUNAFS_STATUS_OK);
			} else {
				fs_broadcast_metadata_saved(SAUNAFS_ERROR_IO);
			}
		} else {
			fs_broadcast_metadata_saved(SAUNAFS_ERROR_IO);
			if (metadataDumper.useMetarestore()) {
				// master should recalculate its checksum
				safs_pretty_syslog(LOG_WARNING, "dumping metadata failed, recalculating checksum");
				fs_start_checksum_recalculation();
			}
			unlink(kMetadataTmpFilename);
		}
	}
}

void fs_periodic_storeall() {
	auto result = fs_storeall(MetadataDumper::kBackgroundDump); // ignore error

	safs_silent_syslog(LOG_DEBUG, "periodic metadata dump: %s",
	                   (SAUNAFS_STATUS_OK == result) ? "success" : "failure");
}

void fs_term(void) {
	if (metadataDumper.inProgress()) {
		metadataDumper.waitUntilFinished();
	}
	bool metadataStored = false;
	if (gMetadata != nullptr && gSaveMetadataAtExit) {
		for (;;) {
			metadataStored = (fs_storeall(MetadataDumper::kForegroundDump) == SAUNAFS_STATUS_OK);
			if (metadataStored) {
				break;
			}
			safs_pretty_syslog(LOG_ERR,"can't store metadata - try to make more space on your hdd or change privieleges - retrying after 10 seconds");
			sleep(10);
		}
	}
	if (metadataStored) {
		// Remove the lock to say that the server has gently stopped and saved its metadata.
		fs_unlock();
	} else if (gMetadata != nullptr && !gSaveMetadataAtExit) {
		// We will leave the lockfile present to indicate, that our metadata.sfs file should not be
		// loaded (it is not up to date -- some changelogs need to be applied). Write a message
		// which tells that the lockfile is not left because of a crash, but because we have been
		// asked to stop without saving metadata. Include information about version of metadata
		// which can be recovered using our changelogs.
		auto message = "quick_stop: " + std::to_string(gMetadata->metaversion) + "\n";
		gMetadataLockfile->writeMessage(message);
	} else {
		// We will leave the lockfile present to indicate, that our metadata.sfs file should not be
		// loaded (it is not up to date, because we didn't manage to download the most recent).
		// Write a message which tells that the lockfile is not left because of a crash, but because
		// we have been asked to stop before loading metadata. Don't overwrite 'quick_stop' though!
		if (!gMetadataLockfile->hasMessage()) {
			gMetadataLockfile->writeMessage("no_metadata: 0\n");
		}
	}
	delete gMetadata;
}

void fs_disable_metadata_dump_on_exit() {
	gSaveMetadataAtExit = false;
}

#else
void fs_storeall(const char *fname) {
	FILE *fd;
	fd = fopen(fname,"w");
	if (fd==NULL) {
		safs_pretty_syslog(LOG_ERR, "can't open metadata file");
		return;
	}
	fs_store_fd(fd);

	if (ferror(fd)!=0) {
		safs_pretty_syslog(LOG_ERR, "can't write metadata");
	} else if (fflush(fd) == EOF) {
		safs_pretty_syslog(LOG_ERR, "can't fflush metadata");
	} else if (fsync(fileno(fd)) == -1) {
		safs_pretty_syslog(LOG_ERR, "can't fsync metadata");
	}
	fclose(fd);
}

void fs_term(const char *fname, bool noLock) {
	if (!noLock) {
		gMetadataLockfile->eraseMessage();
	}
	fs_storeall(fname);
	if (!noLock) {
		fs_unlock();
	}
}
#endif

void fs_strinit(void) {
	gMetadata = new FilesystemMetadata;
}

/* executed in master mode */
#ifndef METARESTORE

/// Returns true iff we are allowed to swallow a stale lockfile and apply changelogs.
static bool fs_can_do_auto_recovery() {
	return gAutoRecovery || main_has_extra_argument("auto-recovery", CaseSensitivity::kIgnore);
}

void fs_erase_message_from_lockfile() {
	if (gMetadataLockfile != nullptr) {
		gMetadataLockfile->eraseMessage();
	}
}

int fs_loadall(void) {
	fs_strinit();
	chunk_strinit();
	changelogsMigrateFrom_1_6_29("changelog");
	if (fs::exists(kMetadataTmpFilename)) {
		throw MetadataFsConsistencyException(
		    "temporary metadata file (" + std::string(kMetadataTmpFilename) + ") exists,"
		    " metadata directory is in dirty state");
	}
	std::string metadataFile;
	bool metadataFileExists = fs::exists(kMetadataFilename);
	bool legacyMetadataFileExists = fs::exists(kMetadataLegacyFilename);
	if (metadataFileExists) {
		metadataFile = kMetadataFilename;
	}
	if(metadataFileExists && legacyMetadataFileExists) {
		metadataFile = kMetadataFilename;
		safs_pretty_syslog(LOG_WARNING, "There are two metadata files in the data path: %s and %s."
		                   " Please remove the legacy one (%s) to avoid damage to your storage.",
		                   kMetadataFilename, kMetadataLegacyFilename, kMetadataLegacyFilename);
	}
	if (!metadataFileExists && legacyMetadataFileExists) {
		metadataFile = kMetadataLegacyFilename;
		safs_pretty_syslog(LOG_WARNING, "Only Legacy metadata file %s found and will be loaded instead."
		                   " You should delete legacy metadata %s on next restart after new metadata %s is created ",
		                   metadataFile.c_str(), kMetadataLegacyFilename, kMetadataFilename);
	}
	if (metadataserver::isMaster() && !metadataFileExists && !legacyMetadataFileExists) {
		fs_unlock();
		std::string currentPath = fs::getCurrentWorkingDirectoryNoThrow();
		throw FilesystemException("can't open metadata file "+ currentPath + "/" + kMetadataFilename
					+ ": if this is a new installation create empty metadata by copying "
					+ currentPath + "/" + kMetadataFilename + ".empty to " + currentPath
					+ "/" + kMetadataFilename);
	}

	{
		auto scopedTimer = util::ScopedTimer("metadata load time");
		fs_loadall(metadataFile, 0);
	}

	bool autoRecovery = fs_can_do_auto_recovery();
	if (autoRecovery || (metadataserver::getPersonality() == metadataserver::Personality::kShadow)) {
		safs_pretty_syslog_attempt(LOG_INFO, "%s - applying changelogs from %s",
				(autoRecovery ? "AUTO_RECOVERY enabled" : "running in shadow mode"),
				fs::getCurrentWorkingDirectoryNoThrow().c_str());
		fs_load_changelogs();
		safs_pretty_syslog(LOG_INFO, "all needed changelogs applied successfully");
	}
	return 0;
}

void fs_cs_disconnected(void) {
	gTestStartTime = eventloop_time() + gOperationsDelayDisconnect;
}

/*
 * Initialize subsystems required by Master personality of metadataserver.
 */
void fs_become_master() {
	if (!gMetadata) {
		safs_pretty_syslog(LOG_ERR, "Attempted shadow->master transition without metadata - aborting");
		exit(1);
	}
	dcm_clear();
	gTestStartTime = eventloop_time() + gOperationsDelayInit;
	fs_periodic_master_init();
	return;
}

static void fs_read_goals_from_stream(std::istream& stream) {
	auto goals = goal_config::load(stream);
	std::swap(gGoalDefinitions, goals);
}

static void fs_read_goals_from_stream(std::istream&& stream) {
	fs_read_goals_from_stream(stream);
}

static void fs_read_goal_config_file() {
	std::string goalConfigFile =
			cfg_getstring("CUSTOM_GOALS_FILENAME", "");
	if (goalConfigFile.empty()) {
		// file is not specified
		const char *defaultGoalConfigFile = ETC_PATH "/sfsgoals.cfg";
		if (access(defaultGoalConfigFile, F_OK) == 0) {
			// the default file exists - use it
			goalConfigFile = defaultGoalConfigFile;
		} else {
			safs_pretty_syslog(LOG_WARNING,
					"goal configuration file %s not found - using default goals; if you don't "
					"want to define custom goals create an empty file %s to disable this warning",
					defaultGoalConfigFile, defaultGoalConfigFile);
			fs_read_goals_from_stream(std::stringstream()); // empty means defaults
			return;
		}
	}
	std::ifstream goalConfigStream(goalConfigFile);
	if (!goalConfigStream.good()) {
		throw ConfigurationException("failed to open goal definitions file " + goalConfigFile);
	}
	try {
		fs_read_goals_from_stream(goalConfigStream);
		safs_pretty_syslog(LOG_INFO,
				"initialized goal definitions from file %s",
				goalConfigFile.c_str());
	} catch (Exception& ex) {
		throw ConfigurationException(
				"malformed goal definitions in " + goalConfigFile + ": " + ex.message());
	}
}

static void fs_read_config_file() {
	gAutoRecovery = cfg_getint32("AUTO_RECOVERY", 0) == 1;
	gDisableChecksumVerification = cfg_getint32("DISABLE_METADATA_CHECKSUM_VERIFICATION", 0) != 0;
	gMagicAutoFileRepair = cfg_getint32("MAGIC_AUTO_FILE_REPAIR", 0) == 1;
	gAtimeDisabled = cfg_getint32("NO_ATIME", 0) == 1;
	gStoredPreviousBackMetaCopies = cfg_get_maxvalue(
			"BACK_META_KEEP_PREVIOUS",
			kDefaultStoredPreviousBackMetaCopies,
			kMaxStoredPreviousBackMetaCopies);

	ChecksumUpdater::setPeriod(cfg_getint32("METADATA_CHECKSUM_INTERVAL", 50));
	gChecksumBackgroundUpdater.setSpeedLimit(
			cfg_getint32("METADATA_CHECKSUM_RECALCULATION_SPEED", 100));
	metadataDumper.setMetarestorePath(
			cfg_get("SFSMETARESTORE_PATH", std::string(SBIN_PATH "/sfsmetarestore")));
	metadataDumper.setUseMetarestore(cfg_getint32("MAGIC_PREFER_BACKGROUND_DUMP", 0));

	// Set deprecated values first, then override them if newer version is found
	gOperationsDelayInit = cfg_getuint32("REPLICATIONS_DELAY_INIT", 300);
	gOperationsDelayDisconnect = cfg_getuint32("REPLICATIONS_DELAY_DISCONNECT", 3600);
	gOperationsDelayInit = cfg_getuint32("OPERATIONS_DELAY_INIT", gOperationsDelayInit);
	gOperationsDelayDisconnect = cfg_getuint32("OPERATIONS_DELAY_DISCONNECT", gOperationsDelayDisconnect);
	if (cfg_isdefined("REPLICATIONS_DELAY_INIT") || cfg_isdefined("REPLICATIONS_DELAY_DISCONNECT")) {
		safs_pretty_syslog(LOG_WARNING, "REPLICATIONS_DELAY_INIT and REPLICATION_DELAY_DISCONNECT"
		" entries are deprecated. Use OPERATIONS_DELAY_INIT and OPERATIONS_DELAY_DISCONNECT instead.");
	}
	gEmptyReservedFilesPeriod = cfg_getuint32("EMPTY_RESERVED_FILES_PERIOD_MSECONDS", 0);

	chunk_invalidate_goal_cache();
	fs_read_goal_config_file(); // may throw
	fs_read_snapshot_config_file();
	fs_read_periodic_config_file();
}

void fs_reload(void) {
	try {
		fs_read_config_file();
	} catch (Exception& ex) {
		safs_pretty_syslog(LOG_WARNING, "Error in configuration: %s", ex.what());
	}
}

void fs_unload() {
	safs_pretty_syslog(LOG_WARNING, "unloading filesystem at %" PRIu64, fs_getversion());
	restore_reset();
	matoclserv_session_unload();
	chunk_unload();
	dcm_clear();
	delete gMetadata;
	gMetadata = nullptr;
}

int fs_init(bool doLoad) {
	fs_read_config_file();
	if (!gMetadataLockfile) {
		gMetadataLockfile.reset(new Lockfile(kMetadataFilename + std::string(".lock")));
	}
	if (!gMetadataLockfile->isLocked()) {
		try {
			gMetadataLockfile->lock((fs_can_do_auto_recovery() || !metadataserver::isMaster()) ?
					Lockfile::StaleLock::kSwallow : Lockfile::StaleLock::kReject);
		} catch (const LockfileException& e) {
			if (e.reason() == LockfileException::Reason::kStaleLock) {
				throw LockfileException(
						std::string(e.what()) + ", consider running `sfsmetarestore -a' to fix problems with your datadir.",
						LockfileException::Reason::kStaleLock);
			}
			throw;
		}
	}
	changelog_init(kChangelogFilename, 0, 50);

	if (doLoad || (metadataserver::isMaster())) {
		fs_loadall();
	}
	eventloop_reloadregister(fs_reload);
	metadataserver::registerFunctionCalledOnPromotion(fs_become_master);
	auto metadataDumpPeriod = cfg_getint32("METADATA_DUMP_PERIOD_SECONDS", 3600);
	if (metadataDumpPeriod > 0) {  /// 0 means disabled periodic metadata dumps
		eventloop_timeregister(TIMEMODE_RUN_LATE, metadataDumpPeriod, 0,
		                       fs_periodic_storeall);
	}
	if (metadataserver::isMaster()) {
		fs_become_master();
	}
	eventloop_pollregister(metadataPollDesc, metadataPollServe);
	eventloop_destructregister(fs_term);
	return 0;
}

/*
 * Initialize filesystem subsystem if currently metadataserver have Master personality.
 */
int fs_init() {
	return fs_init(false);
}

#else
int fs_init(const char *fname,int ignoreflag, bool noLock) {
	if (!noLock) {
		gMetadataLockfile.reset(new Lockfile(fs::dirname(fname) + "/" + kMetadataFilename + ".lock"));
		gMetadataLockfile->lock(Lockfile::StaleLock::kSwallow);
	}
	fs_strinit();
	chunk_strinit();
	fs_loadall(fname,ignoreflag);
	return 0;
}
#endif
