/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA
   Copyright 2013-2016 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ

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

#include "common/platform.h"

#include "mount/client_common.h"
#include "mount/special_inode.h"

using namespace SaunaClient;

namespace InodeMasterInfo {
static BytesWritten write(const Context &ctx, const char */*buf*/, size_t size,
	                            off_t off, FileInfo */*fi*/) {
	oplog_printf(ctx, "write (%lu,%" PRIu64 ",%" PRIu64 "): %s",
	            (unsigned long int)inode_,
	            (uint64_t)size,
	            (uint64_t)off,
	            saunafs_error_string(SAUNAFS_ERROR_EACCES));
	throw RequestException(SAUNAFS_ERROR_EACCES);
}
} // InodeMasterInfo

namespace InodeStats {
static BytesWritten write(const Context &ctx, const char */*buf*/, size_t size,
	                       off_t off, FileInfo *fi) {
	sinfo *statsinfo = reinterpret_cast<sinfo*>(fi->fh);
	if (statsinfo != NULL) {
		PthreadMutexWrapper lock((statsinfo->lock));         // make helgrind happy
		statsinfo->reset = 1;
	}
	oplog_printf(ctx, "write (%lu,%" PRIu64 ",%" PRIu64 "): OK (%lu)",
	            (unsigned long int)inode_,
	            (uint64_t)size,
	            (uint64_t)off,
	            (unsigned long int)size);
	return size;
}
} // InodeStats

namespace InodeOplog {
static BytesWritten write(const Context &ctx, const char */*buf*/, size_t size,
	                       off_t off, FileInfo */*fi*/) {
	oplog_printf(ctx, "write (%lu,%" PRIu64 ",%" PRIu64 "): %s",
	            (unsigned long int)inode_,
	            (uint64_t)size,
	            (uint64_t)off,
	            saunafs_error_string(SAUNAFS_ERROR_EACCES));
	throw RequestException(SAUNAFS_ERROR_EACCES);
}
} // InodeOplog

namespace InodeOphistory {
static BytesWritten write(const Context &ctx, const char */*buf*/, size_t size,
	                           off_t off, FileInfo */*fi*/) {
	oplog_printf(ctx, "write (%lu,%" PRIu64 ",%" PRIu64 "): %s",
	            (unsigned long int)inode_,
	            (uint64_t)size,
	            (uint64_t)off,
	            saunafs_error_string(SAUNAFS_ERROR_EACCES));
	throw RequestException(SAUNAFS_ERROR_EACCES);
}
} // InodeOphistory

namespace InodeTweaks {
static BytesWritten write(const Context &ctx, const char *buf, size_t size,
	                        off_t off, FileInfo *fi) {
	MagicFile *file = reinterpret_cast<MagicFile*>(fi->fh);
	std::unique_lock<std::mutex> lock(file->mutex);
	if (off + size > file->value.size()) {
		file->value.resize(off + size);
	}
	file->value.replace(off, size, buf, size);
	file->wasWritten = true;
	oplog_printf(ctx, "write (%lu,%" PRIu64 ",%" PRIu64 "): OK (%lu)",
	            (unsigned long int)inode_,
	            (uint64_t)size,
	            (uint64_t)off,
	            (unsigned long int)size);
	return size;
}
} // InodeTweaks

namespace InodePathByInode {
static BytesWritten write(const Context &ctx, const char * /*buf*/, size_t size,
	                           off_t off, FileInfo * /*fi*/) {
	std::unique_lock<std::mutex> lock(inodePathInfo.mtx);
	oplog_printf(ctx, "write (%lu,%" PRIu64 ",%" PRIu64 "): %s",
	            (unsigned long int)inode_,
	            (uint64_t)size,
	            (uint64_t)off,
	            saunafs_error_string(SAUNAFS_ERROR_EACCES));
	throw RequestException(SAUNAFS_ERROR_EACCES);
}
} // InodePathByInode

static const std::array<std::function<BytesWritten
	(const Context&, const char *, size_t, off_t, FileInfo*)>, 16> funcs = {{
	 &InodeStats::write,            //0x0U
	 &InodeOplog::write,            //0x1U
	 &InodeOphistory::write,        //0x2U
	 &InodeTweaks::write,           //0x3U
	 nullptr,                       //0x4U
	 nullptr,                       //0x5U
	 nullptr,                       //0x6U
	 nullptr,                       //0x7U
	 &InodePathByInode::write,      //0x8U
	 nullptr,                       //0x9U
	 nullptr,                       //0xAU
	 nullptr,                       //0xBU
	 nullptr,                       //0xCU
	 nullptr,                       //0xDU
	 nullptr,                       //0xEU
	 &InodeMasterInfo::write        //0xFU
}};

BytesWritten special_write(Inode ino, const Context &ctx, const char *buf,
	                           size_t size, off_t off, FileInfo *fi) {
	auto func = funcs[ino - SPECIAL_INODE_BASE];
	if (!func) {
		safs_pretty_syslog(LOG_WARNING,
			"Trying to call unimplemented 'write' function for special inode");
		throw RequestException(SAUNAFS_ERROR_EINVAL);
	}
	return func(ctx, buf, size, off, fi);
}
