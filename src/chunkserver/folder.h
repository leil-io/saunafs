#pragma once

#include "common/platform.h"

#include "folder_chunks.h"
#include "common/disk_info.h"

#define STATS_HISTORY (24*60)
#define LAST_ERROR_SIZE 30

constexpr uint32_t kSecondsInOneMinute = 60;

/// Number of bytes which should be addded to each disk's used space
inline uint64_t gLeaveFree;

/// Default value for HDD_LEAVE_SPACE_DEFAULT
constexpr char gLeaveSpaceDefaultDefaultStrValue[] = "4GiB";

class Chunk;

/// Represents a data folder in the Chunkserver context.
///
/// Each Folder maps to a single line in the hdd.cfg file.
/// In production, the recommended configuration is one Folder per physical drive.
/// For tests, it is fine to have different Folders pointing to the same device.
/// Entries starting with '*' are considered marked for removal, and the system
/// will copy their Chunks to other Chunkservers or Folders.
/// E.g.:
/// /mnt/hdd_02
/// /mnt/ssd_04
/// */mnt/ssd_08
struct Folder {
	enum class ScanState {
		kNeeded = 0u,           ///< Scanning is scheduled (the scanning thread is not running yet).
		kInProgress = 1u,       ///< Scan in progress (the scanning thread is running).
		kTerminate = 2u,        ///< Requested the scanning thread to stop scanning ASAP.
		kThreadFinished = 3u,   ///< The scanning thread has finished its work and can be joined.
		kSendNeeded = 4u,       ///< Resend the content of this folder to Master
		kWorking = 5u           ///< Scan is complete and the folder can be used.
	};

	enum class MigrateState {
		kDone = 0u,            ///< The migration process completely finished
		kInProgress = 1u,      ///< Migration thread is running
		kTerminate = 2u,       ///< Requested the migrate thread to stop ASAP
		kThreadFinished = 3u   ///< The migration thread already finished and can be joined
	};

	/// An I/O error which happened when accessing chunks in this folder.
	struct IoError {
		/// A constructor.
		IoError() : chunkid(0), timestamp(0), errornumber(0) {}

		uint64_t chunkid;    ///< The ID of the chunk which caused the error.
		uint32_t timestamp;  ///< The timestamp of the error.
		int errornumber;     ///< The error number (a.k.a., errno) of the error.
	};

	/// Lock class to avoid different chunkservers using the same folder
	class LockFile {
	public:
		/// Constructor of a LockFile object.
		///
		/// \param fd    Lock-file's file descriptor.
		/// \param dev   Lock-file's device number (probably from stat).
		/// \param inode Lock-file's Inode number.
		LockFile(int fd, dev_t dev, ino_t inode)
		    : fd_(fd), device_(dev), inode_(inode) {
			sassert(fd != -1);
		}

		// Disable copying and moving lock file objects.
		LockFile(const LockFile&) = delete;
		LockFile(LockFile&&) = delete;
		LockFile& operator=(const LockFile&) = delete;
		LockFile& operator=(LockFile&&) = delete;

		/// Releases the lock file if needed.
		~LockFile() {
			if (fd_ >= 0)
				close(fd_);
		}

		/// True if this lock file is in the device `dev`.
		bool isInTheSameDevice(dev_t dev) const {
			return device_ == dev;
		}

		/// True if this lock file is the same.
		bool isTheSameFile(dev_t dev, ino_t inode) const {
			return device_ == dev && inode_ == inode;
		}

	private:
		int fd_ = -1;   ///< Lock-file's file descriptor.
		dev_t device_;  ///< Lock-file's device number.
		ino_t inode_;   ///< Lock-file's Inode number.
	};

	/// Constructs a `Folder` with previous information from hdd.cfg and assigns default values.
	Folder(std::string _path, bool _isMarkedForRemoval)
	    : path(std::move(_path)),
	      isMarkedForRemoval(_isMarkedForRemoval),
	      leaveFreeSpace(gLeaveFree),
	      carry(random() / static_cast<double>(RAND_MAX)) {
	}

	/// Tells if this folder is 'logically' marked for deletion.
	///
	/// Folders are considered marked for deletion, from the master's
	/// perspective, if they are explicitly marked for removal in the hdd.cfg
	/// file or if it is on a read-only file system.
	inline bool isMarkedForDeletion() const {
		return isMarkedForRemoval || isReadOnly;
	}

	/// Tells if this folder is suitable for storing new chunks, according it general state
	inline bool isSelectableForNewChunk() const {
		return !(isDamaged || isMarkedForDeletion() || totalSpace == 0
		         || availableSpace == 0
		         || scanState != Folder::ScanState::kWorking);
	}

	std::string path;  ///< Location of this data folder (e.g., /mnt/hdd07).

	ScanState scanState = ScanState::kNeeded;  ///< The status of scanning this disk.
	uint8_t scanProgress = 0;                  ///< Scan progress percentage

	bool needRefresh = true;   ///< Tells if the disk usage related fields need to be recalculated
	uint32_t lastRefresh = 0;  ///< Timestamp in seconds storing the last time this folder was refreshed

	bool wasRemovedFromConfig = false;  ///< Tells if this folder is missing in the config file after reloading
	bool isMarkedForRemoval = false;    ///< Marked with * in the hdd.cfg file
	bool isReadOnly = false;            ///< A read-only file system was detected

	/// Tells if this folder contains important errors
	///
	/// Most common errors are related to:
	/// * Being unable to create a needed lock file
	/// * Read/write errors greater than the specified limit
	bool isDamaged = true;

	MigrateState migrateState = MigrateState::kDone;  ///< Controls the migration process

	uint64_t leaveFreeSpace = 0;    ///< Reserved space in bytes on this disk (from configuration)
	uint64_t availableSpace = 0;    ///< Total space - used space - leaveFreeSpace (bytes)
	uint64_t totalSpace = 0;        ///< Total usable space in bytes in this device

	HddAtomicStatistics currentStat;                 ///< Current stats (updated with every operation)
	std::array<HddStatistics, STATS_HISTORY> stats;  ///< History of stats for the last STATSHISTORY minutes
	uint32_t statsPos = 0;                           ///< Used to rotate the stats in the stats array

	std::array<IoError, LAST_ERROR_SIZE> lastErrorTab;  ///< History with last LAST_ERROR_SIZE errors
	uint32_t lastErrorIndex = 0;                        ///< Index of the last error

	/// Folder's lock file.
	///
	/// Used to prevent the same data folder from being used by different
	/// chunkserver processes.
	std::unique_ptr<const LockFile> lock = nullptr;

	double carry;               ///< Assists in the process of selecting the Folder for a new Chunk

	std::thread scanThread;     ///< Holds the thread for scanning this folder, so far hdd_folder_scan on hddspacemgr
	std::thread migrateThread;  ///< Holds the thread for migrating the chunks' filenames from old layouts

	/// A collection of chunks located in this folder.
	///
	/// The collection is ordered into a near-random sequence in which
	/// the chunks will be tested for checksum correctness.
	///
	/// The collection is guarded by `hashlock`, which should be locked
	/// when the collection is accessed for reading or modifying.
	FolderChunks chunks;
};
