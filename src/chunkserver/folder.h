#pragma once

#include "common/platform.h"

#include "folder_chunks.h"
#include "common/disk_info.h"

#define STATS_HISTORY (24*60)
#define LAST_ERROR_SIZE 30

constexpr uint32_t kSecondsInOneMinute = 60;

class Chunk;

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

		/// The ID of the chunk which caused the error.
		uint64_t chunkid;

		/// The timestamp of the error.
		uint32_t timestamp;

		/// The error number (a.k.a., errno) of the error.
		int errornumber;
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

	char *path;

	ScanState scanState = ScanState::kNeeded;  ///< The status of scanning this disk.

	bool needRefresh = true;  ///< Tells if the disk usage related fields need to be recalculated
	uint32_t lastRefresh;     ///< Timestamp in seconds storing the last time this folder was refreshed

	unsigned int todel:2;
	bool isDamaged = true;
	unsigned int toremove:2;
	uint8_t scanprogress;

	MigrateState migrateState;

	uint64_t leavefree;
	uint64_t avail;
	uint64_t total;

	HddAtomicStatistics currentStat;                 ///< Current stats (updated with every operation)
	std::array<HddStatistics, STATS_HISTORY> stats;  ///< History of stats for the last STATSHISTORY minutes
	uint32_t statsPos;                               ///< Used to rotate the stats in the stats array

	std::array<IoError, LAST_ERROR_SIZE> lastErrorTab;  ///< History with last LAST_ERROR_SIZE errors
	uint32_t lastErrorIndex;                            ///< Index of the last error

	/// Folder's lock file.
	///
	/// Used to prevent the same data folder from being used by different
	/// chunkserver processes.
	std::unique_ptr<const LockFile> lock = nullptr;

	double carry;

	std::thread scanthread;
	std::thread migratethread;

	/// A collection of chunks located in this folder.
	///
	/// The collection is ordered into a near-random sequence in which
	/// the chunks will be tested for checksum correctness.
	///
	/// The collection is guarded by `hashlock`, which should be locked
	/// when the collection is accessed for reading or modifying.
	FolderChunks chunks;
};
