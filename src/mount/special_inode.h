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

#pragma once

#include "common/platform.h"

#include "common/special_inode_defs.h"
#include "mount/sauna_client.h"
#include "mount/sauna_client_context.h"
#include "mount/mastercomm.h"
#include "mount/masterproxy.h"
#include "mount/oplog.h"
#include "mount/tweaks.h"

namespace InodeMasterInfo {
	extern const Attributes attr;
	extern const SaunaClient::Inode inode_;
}

namespace InodeStats {
	typedef struct _sinfo {
		char *buff;
		uint32_t leng;
		uint8_t reset;
		pthread_mutex_t lock;
	} sinfo;

	extern const Attributes attr;
	extern const SaunaClient::Inode inode_;
}

namespace InodeOplog {
	extern const Attributes attr;
	extern const SaunaClient::Inode inode_;
}

namespace InodeOphistory {
	extern const Attributes attr;
	extern const SaunaClient::Inode inode_;
}

namespace InodeTweaks {
	extern const Attributes attr;
	extern const SaunaClient::Inode inode_;
}

namespace InodeFileByInode {
	extern const Attributes attr;
	extern const SaunaClient::Inode inode_;
}

std::vector<uint8_t> special_read(SaunaClient::Inode ino, const SaunaClient::Context &ctx,
	                          size_t size, off_t off, SaunaClient::FileInfo *fi, int debug_mode);

SaunaClient::BytesWritten special_write(SaunaClient::Inode ino, const SaunaClient::Context &ctx,
	                                 const char *buf, size_t size, off_t off, SaunaClient::FileInfo *fi);

SaunaClient::EntryParam special_lookup(SaunaClient::Inode ino, const SaunaClient::Context &ctx,
	                                SaunaClient::Inode parent, const char *name, char attrstr[256]);

SaunaClient::AttrReply special_getattr(SaunaClient::Inode ino, const SaunaClient::Context &ctx,
	                                char (&attrstr)[256]);

SaunaClient::AttrReply special_setattr(SaunaClient::Inode ino, const SaunaClient::Context &ctx, struct stat *stbuf,
	                                int to_set, char modestr[11], char attrstr[256]);

void special_open(SaunaClient::Inode ino, const SaunaClient::Context &ctx, SaunaClient::FileInfo *fi);

void special_release(SaunaClient::Inode ino, SaunaClient::FileInfo *fi);
