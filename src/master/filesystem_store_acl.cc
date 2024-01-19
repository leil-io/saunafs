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


#include <cstdio>
#include <vector>

#include "common/cwrap.h"
#include "common/main.h"
#include "common/setup.h"
#include "common/saunafs_version.h"
#include "common/metadata.h"
#include "common/rotate_files.h"
#include "common/setup.h"

#include "master/changelog.h"
#include "master/filesystem.h"
#include "master/filesystem_xattr.h"
#include "master/filesystem_metadata.h"
#include "master/filesystem_node.h"
#include "master/filesystem_freenode.h"
#include "master/filesystem_operations.h"
#include "master/filesystem_checksum.h"
#include "master/locks.h"
#include "master/matomlserv.h"
#include "master/metadata_dumper.h"

#include "master/filesystem_store_acl.h"

static void fs_store_marker(FILE *fd) {
	static std::vector<uint8_t> buffer;
	buffer.clear();
	uint32_t marker = 0;
	serialize(buffer, marker);
	if (fwrite(buffer.data(), 1, buffer.size(), fd) != buffer.size()) {
		safs_pretty_syslog(LOG_NOTICE, "fwrite error");
		return;
	}
}

static void fs_store_acl(uint32_t id, const RichACL &acl, FILE *fd) {
	static std::vector<uint8_t> buffer;
	buffer.clear();
	uint32_t size = serializedSize(id, acl);
	serialize(buffer, size, id, acl);
	if (fwrite(buffer.data(), 1, buffer.size(), fd) != buffer.size()) {
		safs_pretty_syslog(LOG_NOTICE, "fwrite error");
		return;
	}
}

void fs_store_acls(FILE *fd) {
	for (uint32_t i = 0; i < NODEHASHSIZE; ++i) {
		for (FSNode *p = gMetadata->nodehash[i]; p; p = p->next) {
			const RichACL *node_acl = gMetadata->acl_storage.get(p->id);
			if (node_acl) {
				fs_store_acl(p->id, *node_acl, fd);
			}
		}
	}
	fs_store_marker(fd);
}

static int fs_load_posix_acl(const MemoryMappedFile&metadataFile,
                             size_t& offsetBegin,
                             int ignoreFlag,
                             bool default_acl) {
	try {
		// Read size of the entry
		uint32_t size = 0;
        uint32_t sizeofsize = sizeof(size);
		uint8_t *ptr;
		try{
			ptr = metadataFile.seek(offsetBegin);
		} catch (const std::exception &e) {
			safs_pretty_syslog(LOG_ERR, "loading acl: %s", e.what());
			throw e;
		}
		deserialize(ptr, sizeofsize, size);
		offsetBegin += sizeofsize;

		if (size == 0) {
			// this is end marker
			return 1;
		} else if (size > 10000000) {
			throw Exception("strange size of entry: " + std::to_string(size),
			                SAUNAFS_ERROR_ERANGE);
		}

		// Read the entry
        try {
			ptr = metadataFile.seek(offsetBegin);
        } catch (const std::exception &e) {
			safs_pretty_syslog(LOG_ERR, "loading acl: %s", e.what());
			throw e;
		}

		// Deserialize inode & ACL
		uint32_t inode;
		AccessControlList posix_acl;
		deserialize(ptr, size, inode, posix_acl);
		offsetBegin += size;
		FSNode *p = fsnodes_id_to_node(inode);
		if (!p) {
			throw Exception("unknown inode: " + std::to_string(inode));
		}

		if (default_acl) {
			RichACL new_acl;
			const RichACL *node_acl = gMetadata->acl_storage.get(p->id);
			if (p->type != FSNode::kDirectory) {
				throw Exception("Trying to set default acl for non-directory inode: " + std::to_string(inode));
			}
			if (node_acl) {
				new_acl = *node_acl;
			}
			new_acl.appendDefaultPosixACL(posix_acl);
			gMetadata->acl_storage.set(p->id, std::move(new_acl));
		} else {
			RichACL new_acl;
			const RichACL *node_acl = gMetadata->acl_storage.get(p->id);
			if (node_acl) {
				new_acl = *node_acl;
			}
			new_acl.appendPosixACL(posix_acl, p->type == FSNode::kDirectory);
			p->mode = (p->mode & ~0777) | (new_acl.getMode() & 0777);
			gMetadata->acl_storage.set(p->id, std::move(new_acl));
		}
		return 0;
	} catch (Exception &ex) {
		safs_pretty_syslog(LOG_ERR, "loading acl: %s", ex.what());
		if (!ignoreFlag || ex.status() != SAUNAFS_STATUS_OK) {
			return -1;
		} else {
			return 0;
		}
	}
}

static int fs_load_legacy_acl(const MemoryMappedFile &metadataFile,
                              size_t& offsetBegin,
                              int ignoreFlag) {
	try {
		// Read size of the entry
		uint32_t size = 0;
        uint32_t sizeofsize = sizeof(size);
		uint8_t *ptr;
        try {
			ptr = metadataFile.seek(offsetBegin);
        } catch (const std::exception &e) {
			safs_pretty_syslog(LOG_ERR, "loading acl: %s", e.what());
			throw e;
		}
		deserialize(ptr, sizeofsize, size);
		offsetBegin += sizeofsize;
		if (size == 0) {
			// this is end marker
			return 1;
		} else if (size > 10000000) {
			throw Exception("strange size of entry: " + std::to_string(size),
			                SAUNAFS_ERROR_ERANGE);
		}

		// Read the entry
        try {
			ptr = metadataFile.seek(offsetBegin);
        } catch (const std::exception &e) {
			safs_pretty_syslog(LOG_ERR, "loading acl: %s", e.what());
			throw e;
		}

		// Deserialize inode & ACL
		uint32_t inode;
		std::unique_ptr<legacy::ExtendedAcl> extended_acl;
		std::unique_ptr<legacy::AccessControlList> default_acl;
		deserialize(ptr, size, inode, extended_acl, default_acl);
		offsetBegin += size;
		FSNode *p = fsnodes_id_to_node(inode);
		if (!p) {
			throw Exception("unknown inode: " + std::to_string(inode));
		}

		RichACL new_acl;
		if (extended_acl) {
			AccessControlList posix_acl = (AccessControlList)*extended_acl;
			new_acl.appendPosixACL(posix_acl, p->type == FSNode::kDirectory);
			p->mode = (p->mode & ~0777) | (new_acl.getMode() & 0777);
		}
		if (default_acl && p->type == FSNode::kDirectory) {
			AccessControlList posix_acl = (AccessControlList)*default_acl;
			new_acl.appendDefaultPosixACL(posix_acl);
		}
		if (new_acl.size() > 0) {
			gMetadata->acl_storage.set(p->id, std::move(new_acl));
		}
		return 0;
	} catch (Exception &ex) {
		safs_pretty_syslog(LOG_ERR, "loading acl: %s", ex.what());
		if (!ignoreFlag || ex.status() != SAUNAFS_STATUS_OK) {
			return -1;
		} else {
			return 0;
		}
	}
}

bool fs_load_legacy_acls(MetadataLoader::Options options) {
	int s;

	do {
		s = fs_load_legacy_acl(options.metadataFile, options.offset, options.ignoreFlag);
		if (s < 0) {
			return false;
		}
	} while (s == 0);
	return true;
}

bool fs_load_posix_acls(MetadataLoader::Options options) {
	int s;
	do {
		s = fs_load_posix_acl(options.metadataFile, options.offset, options.ignoreFlag, false);
		if (s < 0) {
			return false;
		}
	} while (s == 0);

	do {
		s = fs_load_posix_acl(options.metadataFile, options.offset, options.ignoreFlag, true);
		if (s < 0) {
			return false;
		}
	} while (s == 0);

	return true;
}

int fs_load_acl(const MemoryMappedFile &metadataFile,
                       size_t& offsetBegin,
                       int ignoreFlag) {
	try {
		// Read size of the entry
		uint32_t size = 0;
        uint32_t sizeofsize = sizeof(size);
		uint8_t *ptr;
        try {
			ptr = metadataFile.seek(offsetBegin);
        } catch (const std::exception &e) {
			safs_pretty_syslog(LOG_ERR, "loading acl: %s", e.what());
			throw e;
		}
		deserialize(ptr, sizeofsize, size);
		offsetBegin += sizeofsize;
		if (size == 0) {
			// this is end marker
			return 1;
		} else if (size > 10000000) {
			throw Exception("strange size of entry: " + std::to_string(size),
				SAUNAFS_ERROR_ERANGE);
		}

		// Read the entry
        try {
			ptr = metadataFile.seek(offsetBegin);
        } catch (const std::exception &e) {
			safs_pretty_syslog(LOG_ERR, "loading acl: %s", e.what());
			throw e;
		}

		// Deserialize inode & ACL
		uint32_t inode;
		RichACL acl;
		deserialize(ptr, size, inode);
		offsetBegin += size;
		FSNode *p = fsnodes_id_to_node(inode);
		if (!p) {
			throw Exception("unknown inode: " + std::to_string(inode));
		}
		gMetadata->acl_storage.set(p->id, std::move(acl));
		return 0;
	} catch (Exception &ex) {
		safs_pretty_syslog(LOG_ERR, "loading acl: %s", ex.what());
		if (!ignoreFlag || ex.status() != SAUNAFS_STATUS_OK) {
			return -1;
		} else {
			return 0;
		}
	}
}

bool fs_load_acls(MetadataLoader::Options options) {
	int s;

	do {
		s = fs_load_acl(options.metadataFile, options.offset, options.ignoreFlag);
		if (s < 0) {
			return false;
		}
	} while (s == 0);

	return true;
}
