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

#pragma once

#include "common/platform.h"

#include <map>

#include "common/goal.h"
#include "master/fs_context.h"
#include "master/locks.h"
#include "master/setgoal_task.h"
#include "protocol/lock_info.h"

#define DEFAULT_GOAL 1
#define DEFAULT_TRASHTIME 86400

namespace FsStats {
enum {
	Statfs = 0,
	Getattr,
	Setattr,
	Lookup,
	Mkdir,
	Rmdir,
	Symlink,
	Readlink,
	Mknod,
	Unlink,
	Rename,
	Link,
	Readdir,
	Open,
	Read,
	Write,
	Size
};
}

extern std::array<uint32_t, FsStats::Size> gFsStatsArray;

void fs_retrieve_stats(std::array<uint32_t, FsStats::Size> &output_stats);

const std::map<int, Goal> &fs_get_goal_definitions();
const Goal &fs_get_goal_definition(uint8_t goalId);

void fs_broadcast_metadata_saved(uint8_t status);

// Adds an entry to a changelog, updates filesystem.cc internal structures, prepends a
// proper timestamp to changelog entry and broadcasts it to metaloggers and shadow masters
void fs_changelog(uint32_t ts, const char *format, ...) __attribute__((__format__(__printf__, 2, 3)));
void fs_add_files_to_chunks();

uint64_t fs_getversion();
uint8_t fs_repair(uint32_t rootinode, uint8_t sesflags, uint32_t inode, uint32_t uid, uint32_t gid,
			uint8_t correct_only, uint32_t *notchanged, uint32_t *erased, uint32_t *repaired);

/*! \brief Perform a flock operation on filesystem
 * Possible operations:
 * - unlock
 * - shared/exclusive blocking/nonblocking lock
 * - handle interrupt
 */
int fs_flock_op(const FsContext &context, uint32_t inode, uint64_t owner, uint32_t sessionid,
		uint32_t reqid, uint32_t msgid, uint16_t op, bool nonblocking,
		std::vector<FileLocks::Owner> &applied);

/*! \brief Perform a posix lock operation on filesystem
 * Possible operations:
 * - unlock
 * - shared/exclusive blocking/nonblocking lock
 * - handle interrupt
 */
int fs_posixlock_op(const FsContext &context, uint32_t inode, uint64_t start,
		uint64_t end, uint64_t owner, uint32_t sessionid, uint32_t reqid, uint32_t msgid,
		uint16_t op, bool nonblocking, std::vector<FileLocks::Owner> &applied);

/*! \brief Perform a POSIX lock probe on filesystem
 * \param info - wrapper around 'struct flock', filled with data appropriate with getlk standard
 */
int fs_posixlock_probe(const FsContext &context, uint32_t inode, uint64_t start, uint64_t end,
		uint64_t owner, uint32_t sessionid, uint32_t reqid, uint32_t msgid, uint16_t op,
		safs_locks::FlockWrapper &info);

/*! \brief Release (unlock + unqueue) all locks from a given session
 */
int fs_locks_clear_session(const FsContext &context, uint8_t type, uint32_t inode,
		uint32_t sessionid, std::vector<FileLocks::Owner> &applied);

/*! \brief Perform a lock management operation on inode */
int fs_locks_list_all(const FsContext &context, uint8_t type, bool pending,
		uint64_t start, uint64_t max, std::vector<safs_locks::Info> &locks);
int fs_locks_list_inode(const FsContext &context, uint8_t type, bool pending,
		uint32_t inode, uint64_t start, uint64_t max, std::vector<safs_locks::Info> &locks);
int fs_locks_unlock_inode(const FsContext &context, uint8_t type, uint32_t inode,
		std::vector<FileLocks::Owner> &applied);
int fs_locks_remove_pending(const FsContext &context, uint8_t type, uint64_t ownerid,
		uint32_t sessionid, uint32_t inode, uint64_t reqid);
uint8_t fs_full_path_by_inode(const FsContext &context, uint32_t inode,
                              std::string &fullPath);
