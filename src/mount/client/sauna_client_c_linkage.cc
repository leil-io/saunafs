/*

   Copyright 2017 Skytechnology sp. z o.o.
   Copyright 2023 Leil Storage OÜ

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

#include "client/sauna_client_c_linkage.h"

typedef SaunaClient::EntryParam EntryParam;
typedef SaunaClient::Inode Inode;
typedef SaunaClient::Context Context;
typedef SaunaClient::AttrReply AttrReply;
typedef SaunaClient::FileInfo FileInfo;
typedef SaunaClient::BytesWritten BytesWritten;
typedef SaunaClient::DirEntry DirEntry;
typedef SaunaClient::RequestException RequestException;

int saunafs_fs_init(SaunaClient::FsInitParams &params) {
	try {
		SaunaClient::fs_init(params);
		return SAUNAFS_STATUS_OK;
	} catch (const RequestException &e) {
		return e.saunafs_error_code;
	} catch (...) {
		return SAUNAFS_ERROR_IO;
	}
}

int saunafs_lookup(Context &ctx, Inode parent, const char *name, EntryParam &param) {
	try {
		param = SaunaClient::lookup(ctx, parent, name);
		return SAUNAFS_STATUS_OK;
	} catch (const RequestException &e) {
		return e.saunafs_error_code;
	} catch (...) {
		return SAUNAFS_ERROR_IO;
	}
}

int saunafs_mknod(Context &ctx, Inode parent, const char *name, mode_t mode, dev_t rdev,
		EntryParam &param) {
	try {
		param = SaunaClient::mknod(ctx, parent, name, mode, rdev);
		return SAUNAFS_STATUS_OK;
	} catch (const RequestException &e) {
		return e.saunafs_error_code;
	} catch (...) {
		return SAUNAFS_ERROR_IO;
	}
}

int saunafs_link(Context ctx, Inode inode, Inode parent, const char *name,
		EntryParam &param) {
	try {
		param = SaunaClient::link(ctx, inode, parent, name);
		return SAUNAFS_STATUS_OK;
	} catch (const RequestException &e) {
		return e.saunafs_error_code;
	} catch (...) {
		return SAUNAFS_ERROR_IO;
	}
}

int saunafs_symlink(Context ctx, const char *link, Inode parent, const char *name,
		EntryParam &param) {
	try {
		param = SaunaClient::symlink(ctx, link, parent, name);
		return SAUNAFS_STATUS_OK;
	} catch (const RequestException &e) {
		return e.saunafs_error_code;
	} catch (...) {
		return SAUNAFS_ERROR_IO;
	}
}

int saunafs_unlink(Context &ctx, Inode parent, const char *name) {
	try {
		SaunaClient::unlink(ctx, parent, name);
		return SAUNAFS_STATUS_OK;
	} catch (const RequestException &e) {
		return e.saunafs_error_code;
	} catch (...) {
		return SAUNAFS_ERROR_IO;
	}
}

int saunafs_undel(Context &ctx, Inode ino) {
	try {
		SaunaClient::undel(ctx, ino);
		return SAUNAFS_STATUS_OK;
	} catch (const RequestException &e) {
		return e.saunafs_error_code;
	} catch (...) {
		return SAUNAFS_ERROR_IO;
	}
}

int saunafs_open(Context &ctx, Inode ino, FileInfo *fi) {
	try {
		SaunaClient::open(ctx, ino, fi);
		return SAUNAFS_STATUS_OK;
	} catch (const RequestException &e) {
		return e.saunafs_error_code;
	} catch (...) {
		return SAUNAFS_ERROR_IO;
	}
}

int saunafs_getattr(Context &ctx, Inode ino, AttrReply &reply) {
	try {
		reply = SaunaClient::getattr(ctx, ino);
		return SAUNAFS_STATUS_OK;
	} catch (const RequestException &e) {
		return e.saunafs_error_code;
	} catch (...) {
		return SAUNAFS_ERROR_IO;
	}
}

std::pair<int, SaunaClient::JobId> saunafs_makesnapshot(Context &ctx, Inode ino, Inode dst_parent,
	                                       const std::string &dst_name, bool can_overwrite) {
	try {
		SaunaClient::JobId job_id = SaunaClient::makesnapshot(ctx, ino, dst_parent,
		                                                        dst_name, can_overwrite);
		return {SAUNAFS_STATUS_OK, job_id};
	} catch (RequestException &e) {
		return {e.saunafs_error_code, 0};
	} catch (...) {
		return {SAUNAFS_ERROR_IO, 0};
	}
}

int saunafs_getgoal(Context &ctx, Inode ino, std::string &goal) {
	try {
		goal = SaunaClient::getgoal(ctx, ino);
		return SAUNAFS_STATUS_OK;
	} catch (const RequestException &e) {
		return e.saunafs_error_code;
	} catch (...) {
		return SAUNAFS_ERROR_IO;
	}
}

int saunafs_setattr(Context &ctx, Inode ino, struct stat *stbuf, int to_set,
	             AttrReply &reply) {
	try {
		reply = SaunaClient::setattr(ctx, ino, stbuf, to_set);
		return SAUNAFS_STATUS_OK;
	} catch (const RequestException &e) {
		return e.saunafs_error_code;
	} catch (...) {
		return SAUNAFS_ERROR_IO;
	}
}

int saunafs_setgoal(Context &ctx, Inode ino, const std::string &goal_name, uint8_t smode) {
	try {
		SaunaClient::setgoal(ctx, ino, goal_name, smode);
		return SAUNAFS_STATUS_OK;
	} catch (RequestException &e) {
		return e.saunafs_error_code;
	} catch (...) {
		return SAUNAFS_ERROR_IO;
	}
}

std::pair<int, ReadCache::Result> saunafs_read(Context &ctx, Inode ino, size_t size,
	                                        off_t off, FileInfo *fi) {
	try {
		return std::pair<int, ReadCache::Result>(
		        SAUNAFS_STATUS_OK, SaunaClient::read(ctx, ino, size, off, fi));
	} catch (const RequestException &e) {
		return std::pair<int, ReadCache::Result>(e.saunafs_error_code,
		                                         ReadCache::Result());
	} catch (...) {
		return std::pair<int, ReadCache::Result>(SAUNAFS_ERROR_IO,
		                                         ReadCache::Result());
	}
}

std::pair<int, std::vector<uint8_t>> saunafs_read_special_inode(Context &ctx, Inode ino,
		size_t size, off_t off, FileInfo *fi) {
	try {
		return std::pair<int, std::vector<uint8_t>>(
		        SAUNAFS_STATUS_OK,
		        SaunaClient::read_special_inode(ctx, ino, size, off, fi));
	} catch (const RequestException &e) {
		return std::pair<int, std::vector<uint8_t>>(e.saunafs_error_code,
		                                            std::vector<uint8_t>());
	} catch (...) {
		return std::pair<int, std::vector<uint8_t>>(SAUNAFS_ERROR_IO,
		                                            std::vector<uint8_t>());
	}
}

std::pair<int, ssize_t> saunafs_write(Context &ctx, Inode ino, const char *buf, size_t size,
		off_t off, FileInfo *fi) {
	try {
		auto write_ret = SaunaClient::write(ctx, ino, buf, size, off, fi);
		return {SAUNAFS_STATUS_OK, write_ret};
	} catch (const RequestException &e) {
		return {e.saunafs_error_code, 0};
	} catch (...) {
		return {SAUNAFS_ERROR_IO, 0};
	}
}

int saunafs_release(Inode ino, FileInfo *fi) {
	try {
		SaunaClient::release(ino, fi);
		return SAUNAFS_STATUS_OK;
	} catch (const RequestException &e) {
		return e.saunafs_error_code;
	} catch (...) {
		return SAUNAFS_ERROR_IO;
	}
}

int saunafs_flush(Context &ctx, Inode ino, FileInfo *fi) {
	try {
		SaunaClient::flush(ctx, ino, fi);
		return SAUNAFS_STATUS_OK;
	} catch (const RequestException &e) {
		return e.saunafs_error_code;
	} catch (...) {
		return SAUNAFS_ERROR_IO;
	}
}

int saunafs_fsync(Context &ctx, Inode ino, int datasync, FileInfo* fi) {
	try {
		SaunaClient::fsync(ctx, ino, datasync, fi);
		return SAUNAFS_STATUS_OK;
	} catch (const RequestException &e) {
		return e.saunafs_error_code;
	} catch (...) {
		return SAUNAFS_ERROR_IO;
	}
}

int saunafs_statfs(uint64_t *totalspace, uint64_t *availspace, uint64_t *trashspace,
	             uint64_t *reservedspace, uint32_t *inodes) {
	try {
		SaunaClient::statfs(totalspace, availspace, trashspace, reservedspace, inodes);
		return SAUNAFS_STATUS_OK;
	} catch (...) {
		return SAUNAFS_ERROR_IO;
	}
}

void saunafs_fs_term() {
	try {
		SaunaClient::fs_term();
	} catch (...) {
		// ignore
	}
}

bool saunafs_isSpecialInode(Inode ino) {
	return SaunaClient::isSpecialInode(ino);
}

std::pair<int, std::vector<DirEntry>> saunafs_readdir(Context &ctx, uint64_t opendirSessionID,
		Inode ino, off_t off, size_t max_entries) {
	try {
		auto fsDirEntries = SaunaClient::readdir(ctx, opendirSessionID, ino, off, max_entries);
		uint64_t nextEntryIno = (fsDirEntries.empty()) ? 0 : fsDirEntries.back().attr.st_ino;
		SaunaClient::update_readdir_session(opendirSessionID, nextEntryIno);
		return {SAUNAFS_STATUS_OK, fsDirEntries};
	} catch (const RequestException &e) {
		return {e.saunafs_error_code, std::vector<DirEntry>()};
	} catch (...) {
		return {SAUNAFS_ERROR_IO, std::vector<DirEntry>()};
	}
}

int saunafs_readlink(Context &ctx, Inode ino, std::string &link) {
	try {
		link = SaunaClient::readlink(ctx, ino);
		return SAUNAFS_STATUS_OK;
	} catch (const RequestException &e) {
		return e.saunafs_error_code;
	} catch (...) {
		return SAUNAFS_ERROR_IO;
	}
}

std::pair<int, std::vector<NamedInodeEntry>> saunafs_readreserved(Context &ctx,
		SaunaClient::NamedInodeOffset off, SaunaClient::NamedInodeOffset max_entries) {
	try {
		auto ret = SaunaClient::readreserved(ctx, off, max_entries);
		return {SAUNAFS_STATUS_OK, ret};
	} catch (const RequestException &e) {
		return {e.saunafs_error_code, std::vector<NamedInodeEntry>()};
	} catch (...) {
		return {SAUNAFS_ERROR_IO, std::vector<NamedInodeEntry>()};
	}
}

std::pair<int, std::vector<NamedInodeEntry>> saunafs_readtrash(Context &ctx,
		SaunaClient::NamedInodeOffset off, SaunaClient::NamedInodeOffset max_entries) {
	try {
		auto ret = SaunaClient::readtrash(ctx, off, max_entries);
		return {SAUNAFS_STATUS_OK, ret};
	} catch (const RequestException &e) {
		return {e.saunafs_error_code, std::vector<NamedInodeEntry>()};
	} catch (...) {
		return {SAUNAFS_ERROR_IO, std::vector<NamedInodeEntry>()};
	}
}

int saunafs_opendir(Context &ctx, Inode ino) {
	try {
		SaunaClient::opendir(ctx, ino);
		return SAUNAFS_STATUS_OK;
	} catch (const RequestException &e) {
		return e.saunafs_error_code;
	} catch (...) {
		return SAUNAFS_ERROR_IO;
	}
}

int saunafs_releasedir(Inode ino, uint64_t opendirSessionID) {
	try {
		SaunaClient::releasedir(ino);
		SaunaClient::drop_readdir_session(opendirSessionID);
		return SAUNAFS_STATUS_OK;
	} catch (const RequestException &e) {
		return e.saunafs_error_code;
	} catch (...) {
		return SAUNAFS_ERROR_IO;
	}
}

int saunafs_mkdir(Context &ctx, Inode parent, const char *name, mode_t mode,
		EntryParam &entry_param) {
	try {
		entry_param = SaunaClient::mkdir(ctx, parent, name, mode);
		return SAUNAFS_STATUS_OK;
	} catch (const RequestException &e) {
		return e.saunafs_error_code;
	} catch (...) {
		return SAUNAFS_ERROR_IO;
	}
}

int saunafs_rmdir(Context &ctx, Inode parent, const char *name) {
	try {
		SaunaClient::rmdir(ctx, parent, name);
		return SAUNAFS_STATUS_OK;
	} catch (const RequestException &e) {
		return e.saunafs_error_code;
	} catch (...) {
		return SAUNAFS_ERROR_IO;
	}
}

int saunafs_rename(Context &ctx, Inode parent, const char *name, Inode new_parent,
	            const char *new_name) {
	try {
		SaunaClient::rename(ctx, parent, name, new_parent, new_name);
		return SAUNAFS_STATUS_OK;
	} catch (const RequestException &e) {
		return e.saunafs_error_code;
	} catch (...) {
		return SAUNAFS_ERROR_IO;
	}
}

int saunafs_update_groups(Context &ctx) {
	try {
		SaunaClient::updateGroups(ctx);
	} catch (const RequestException &e) {
		return e.saunafs_error_code;
	} catch(...) {
		return SAUNAFS_ERROR_IO;
	}
	return SAUNAFS_STATUS_OK;
}

int saunafs_setxattr(Context ctx, Inode ino, const char *name, const char *value,
		size_t size, int flags) {
	try {
		SaunaClient::setxattr(ctx, ino, name, value, size, flags, 0);
		return SAUNAFS_STATUS_OK;
	} catch (const RequestException &e) {
		return e.saunafs_error_code;
	} catch (...) {
		return SAUNAFS_ERROR_IO;
	}
}

std::pair<int,std::vector<ChunkWithAddressAndLabel>> saunafs_getchunksinfo(Context &ctx,
	                          Inode ino, uint32_t chunk_index, uint32_t chunk_count) {
	try {
		auto chunks = SaunaClient::getchunksinfo(ctx, ino, chunk_index, chunk_count);
		return {SAUNAFS_STATUS_OK, chunks};
	} catch (const RequestException &e) {
		return {e.saunafs_error_code, std::vector<ChunkWithAddressAndLabel>()};
	} catch (...) {
		return {SAUNAFS_ERROR_IO, std::vector<ChunkWithAddressAndLabel>()};
	}
}

std::pair<int, std::vector<ChunkserverListEntry>> saunafs_getchunkservers() {
	try {
		auto chunkservers = SaunaClient::getchunkservers();
		return {SAUNAFS_STATUS_OK, chunkservers};
	} catch (const RequestException &e) {
		return {e.saunafs_error_code, std::vector<ChunkserverListEntry>()};
	} catch (...) {
		return {SAUNAFS_ERROR_IO, std::vector<ChunkserverListEntry>()};
	}
}


int saunafs_getlk(Context &ctx, Inode ino,
	           SaunaClient::FileInfo *fi, safs_locks::FlockWrapper &lock) {
	try {
		SaunaClient::getlk(ctx, ino, fi, lock);
		return SAUNAFS_STATUS_OK;
	} catch (const RequestException &e) {
		return e.saunafs_error_code;
	} catch (...) {
		return SAUNAFS_ERROR_IO;
	}
}

std::pair<int, uint32_t> saunafs_setlk_send(Context &ctx, Inode ino,
	                            SaunaClient::FileInfo *fi, safs_locks::FlockWrapper &lock) {
	try {
		uint32_t reqid = SaunaClient::setlk_send(ctx, ino, fi, lock);
		return {SAUNAFS_STATUS_OK, reqid};
	} catch (const RequestException &e) {
		return {e.saunafs_error_code, 0};
	} catch (...) {
		return {SAUNAFS_ERROR_IO, 0};
	}
}

int saunafs_setlk_recv() {
	try {
		SaunaClient::setlk_recv();
		return SAUNAFS_STATUS_OK;
	} catch (const RequestException &e) {
		return e.saunafs_error_code;
	} catch (...) {
		return SAUNAFS_ERROR_IO;
	}
}

int saunafs_setlk_interrupt(const safs_locks::InterruptData &data) {
	try {
		SaunaClient::setlk_interrupt(data);
		return SAUNAFS_STATUS_OK;
	} catch (const RequestException &e) {
		return e.saunafs_error_code;
	} catch (...) {
		return SAUNAFS_ERROR_IO;
	}
}

int saunafs_getxattr(Context ctx, Inode ino, const char *name,
	              size_t size, SaunaClient::XattrReply &xattr_reply) {
	try {
		xattr_reply = SaunaClient::getxattr(ctx, ino, name, size, 0);
		return SAUNAFS_STATUS_OK;
	} catch (const RequestException &e) {
		return e.saunafs_error_code;
	} catch (...) {
		return SAUNAFS_ERROR_IO;
	}
}
int saunafs_listxattr(Context ctx, Inode ino, size_t size,
	               SaunaClient::XattrReply &xattr_reply) {
	try {
		xattr_reply = SaunaClient::listxattr(ctx, ino, size);
		return SAUNAFS_STATUS_OK;
	} catch (const RequestException &e) {
		return e.saunafs_error_code;
	} catch (...) {
		return SAUNAFS_ERROR_IO;
	}
}

int saunafs_removexattr(Context ctx, Inode ino, const char *name) {
	try {
		SaunaClient::removexattr(ctx, ino, name);
		return SAUNAFS_STATUS_OK;
	} catch (const RequestException &e) {
		return e.saunafs_error_code;
	} catch (...) {
		return SAUNAFS_ERROR_IO;
	}
}
