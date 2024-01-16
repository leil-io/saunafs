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

#include <cstdio>

#include "common/cwrap.h"
#include "common/exceptions.h"
#include "common/memory_file.h"
#include "master/metadata_dumper.h"

SAUNAFS_CREATE_EXCEPTION_CLASS(MetadataException, Exception);
SAUNAFS_CREATE_EXCEPTION_CLASS(MetadataFsConsistencyException,
                               MetadataException);
SAUNAFS_CREATE_EXCEPTION_CLASS(MetadataConsistencyException, MetadataException);

struct MetadataSectionLoaderOptions {
	MetadataSectionLoaderOptions(const MemoryMappedFile &metadataFile_,
	                             size_t &offset_, int ignoreflag_,
	                             uint64_t sectionLength_, bool loadLockIds_)
	    : metadataFile(metadataFile_),
	      offset(offset_),
	      ignoreflag(ignoreflag_),
	      sectionLength(sectionLength_),
	      loadLockIds(loadLockIds_) {}

	const MemoryMappedFile &metadataFile;
	size_t& offset;
	int ignoreflag;
	uint64_t sectionLength;
	bool loadLockIds;
};

bool fs_commit_metadata_dump();

int fs_emergency_saves();

void fs_broadcast_metadata_saved(uint8_t status);
void fs_load_changelogs();
void fs_load_changelog(const std::string &path);
void fs_loadall(const std::string &fname, int ignoreflag);
void fs_store_fd(FILE *fd);
