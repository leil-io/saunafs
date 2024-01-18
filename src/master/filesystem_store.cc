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

#include "master/filesystem_store.h"
#include "common/platform.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <future>
#include <utility>
#include <vector>

#include "common/cwrap.h"
#include "common/event_loop.h"
#include "common/metadata.h"
#include "common/rotate_files.h"
#include "common/saunafs_version.h"
#include "common/setup.h"

#include "common/memory_file.h"
#include "master/changelog.h"
#include "master/filesystem.h"
#include "master/filesystem_checksum.h"
#include "master/filesystem_freenode.h"
#include "master/filesystem_metadata.h"
#include "master/filesystem_node.h"
#include "master/filesystem_operations.h"
#include "master/filesystem_quota.h"
#include "master/filesystem_store_acl.h"
#include "master/filesystem_xattr.h"
#include "master/locks.h"
#include "master/matoclserv.h"
#include "master/matomlserv.h"
#include "master/metadata_dumper.h"
#include "master/restore.h"

constexpr uint8_t kMetadataVersionLegacy = 0x15;
constexpr uint8_t kMetadataVersionSaunaFS = 0x16;
constexpr uint8_t kMetadataVersionWithSections = 0x20;
constexpr uint8_t kMetadataVersionWithLockIds = 0x29;
static constexpr int8_t kOpSuccess = 0;
static constexpr int8_t kOpFailure = -1;
static std::mutex gOffsetBeginMutex;
static std::mutex gMetadataSectionHeaderMutex;
char const MetadataStructureReadErrorMsg[] =
    "error reading metadata (structure)";

void xattr_store(FILE *fd) {
	uint8_t hdrbuff[4 + 1 + 4];
	uint8_t *ptr;
	uint32_t i;
	xattr_data_entry *xa;

	for (i = 0; i < XATTR_DATA_HASH_SIZE; i++) {
		for (xa = gMetadata->xattr_data_hash[i]; xa; xa = xa->next) {
			ptr = hdrbuff;
			put32bit(&ptr, xa->inode);
			put8bit(&ptr, xa->anleng);
			put32bit(&ptr, xa->avleng);
			if (fwrite(hdrbuff, 1, 4 + 1 + 4, fd) != (size_t)(4 + 1 + 4)) {
				safs_pretty_syslog(LOG_NOTICE, "fwrite error");
				return;
			}
			if (fwrite(xa->attrname, 1, xa->anleng, fd) !=
			    (size_t)(xa->anleng)) {
				safs_pretty_syslog(LOG_NOTICE, "fwrite error");
				return;
			}
			if (xa->avleng > 0) {
				if (fwrite(xa->attrvalue, 1, xa->avleng, fd) !=
				    (size_t)(xa->avleng)) {
					safs_pretty_syslog(LOG_NOTICE, "fwrite error");
					return;
				}
			}
		}
	}
	memset(hdrbuff, 0, 4 + 1 + 4);
	if (fwrite(hdrbuff, 1, 4 + 1 + 4, fd) != (size_t)(4 + 1 + 4)) {
		safs_pretty_syslog(LOG_NOTICE, "fwrite error");
		return;
	}
}

bool xattr_load(MetadataSectionLoaderOptions options) {
	const uint8_t *ptr;
	uint32_t inode;
	uint8_t anleng;
	uint32_t avleng;
	xattr_data_entry *xa;
	xattr_inode_entry *ih;
	uint32_t hash, ihash;

	while (true) {
		try {
			ptr = options.metadataFile.seek(options.offset);
		} catch (const std::exception &e) {
			safs_pretty_syslog(LOG_ERR, "loading xattr: can't read xattr");
			return false;
		}
		inode = get32bit(&ptr);
		anleng = get8bit(&ptr);
		avleng = get32bit(&ptr);
		options.offset = options.metadataFile.offset(ptr);
		if (inode == 0) {
			return true;
		}
		if (anleng == 0) {
			safs_pretty_syslog(LOG_ERR, "loading xattr: empty name");
			if (options.ignoreflag) {
				continue;
			} else {
				return false;
			}
		}
		if (avleng > SFS_XATTR_SIZE_MAX) {
			safs_pretty_syslog(LOG_ERR, "loading xattr: value oversized");
			if (options.ignoreflag) {
				continue;
			} else {
				return false;
			}
		}

		ihash = xattr_inode_hash_fn(inode);
		for (ih = gMetadata->xattr_inode_hash[ihash]; ih && ih->inode != inode;
		     ih = ih->next)
			;

		if (ih && ih->anleng + anleng + 1 > SFS_XATTR_LIST_MAX) {
			safs_pretty_syslog(LOG_ERR, "loading xattr: name list too long");
			if (options.ignoreflag) {
				continue;
			} else {
				return false;
			}
		}

		xa = new xattr_data_entry;
		xa->inode = inode;
		xa->attrname = (uint8_t *)malloc(anleng);
		passert(xa->attrname);
		memcpy(xa->attrname, ptr, anleng);
		ptr+=anleng;
		xa->anleng = anleng;
		if (avleng > 0) {
			xa->attrvalue = (uint8_t *)malloc(avleng);
			passert(xa->attrvalue);
			memcpy(xa->attrvalue, ptr, avleng);
			ptr+=avleng;
		} else {
			xa->attrvalue = NULL;
		}
		options.offset = options.metadataFile.offset(ptr);
		xa->avleng = avleng;
		hash = xattr_data_hash_fn(inode, xa->anleng, xa->attrname);
		xa->next = gMetadata->xattr_data_hash[hash];
		if (xa->next) {
			xa->next->prev = &(xa->next);
		}
		xa->prev = gMetadata->xattr_data_hash + hash;
		gMetadata->xattr_data_hash[hash] = xa;

		if (ih) {
			xa->nextinode = ih->data_head;
			if (xa->nextinode) {
				xa->nextinode->previnode = &(xa->nextinode);
			}
			xa->previnode = &(ih->data_head);
			ih->data_head = xa;
			ih->anleng += anleng + 1U;
			ih->avleng += avleng;
		} else {
			ih = (xattr_inode_entry *)malloc(sizeof(xattr_inode_entry));
			passert(ih);
			ih->inode = inode;
			xa->nextinode = NULL;
			xa->previnode = &(ih->data_head);
			ih->data_head = xa;
			ih->anleng = anleng + 1U;
			ih->avleng = avleng;
			ih->next = gMetadata->xattr_inode_hash[ihash];
			gMetadata->xattr_inode_hash[ihash] = ih;
		}
	}
}

template <class... Args>
static void fs_store_generic(FILE *fd, Args &&...args) {
	static std::vector<uint8_t> buffer;
	buffer.clear();
	const uint32_t size = serializedSize(std::forward<Args>(args)...);
	serialize(buffer, size, std::forward<Args>(args)...);
	if (fwrite(buffer.data(), 1, buffer.size(), fd) != buffer.size()) {
		safs_pretty_syslog(LOG_NOTICE, "fwrite error");
		return;
	}
}

template <class... Args>
static bool fs_load_generic(const MemoryMappedFile &metadataFile,
                            size_t& offsetBegin, Args &&...args) {
	constexpr uint8_t kSize = sizeof(std::uint32_t);
	uint32_t size;
	uint8_t *ptr;
	try {
		ptr = metadataFile.seek(offsetBegin);
	} catch (const std::exception &e) {
		safs_pretty_syslog(LOG_ERR, "loading node: %s", e.what());
		throw Exception("fread error (size)");
	}
	deserialize(ptr, kSize, size);
	offsetBegin += kSize;
	if (size == 0) {
		// marker
		return false;
	}
	try {
		ptr = metadataFile.seek(offsetBegin);
	} catch (const std::exception &e) {
		safs_pretty_syslog(LOG_ERR, "loading node: %s", e.what());
		throw Exception("fread error (size)");
	}
	deserialize(ptr, size, std::forward<Args>(args)...);
	offsetBegin += size;
	return true;
}

void fs_storeedge(FSNodeDirectory *parent, FSNode *child,
                  const std::string &name, FILE *fd) {
	uint8_t uedgebuff[4 + 4 + 2 + 65535];
	uint8_t *ptr;
	if (child == nullptr) {  // last edge
		memset(uedgebuff, 0, 4 + 4 + 2);
		if (fwrite(uedgebuff, 1, 4 + 4 + 2, fd) != (size_t)(4 + 4 + 2)) {
			safs_pretty_syslog(LOG_NOTICE, "fwrite error");
			return;
		}
		return;
	}
	ptr = uedgebuff;
	if (parent == nullptr) {
		put32bit(&ptr, 0);
	} else {
		put32bit(&ptr, parent->id);
	}
	put32bit(&ptr, child->id);
	put16bit(&ptr, name.length());
	memcpy(ptr, name.c_str(), name.length());
	if (fwrite(uedgebuff, 1, 4 + 4 + 2 + name.length(), fd) !=
	    (size_t)(4 + 4 + 2 + name.length())) {
		safs_pretty_syslog(LOG_NOTICE, "fwrite error");
		return;
	}
}

/**
 * @brief
 * @param pSrc A pointer to the data storing all edges.
 * @param sectionOffset A reference to point to the next edge attribute.
 * @param ignoreFlag
 * @param init
 * @return 0 on success, 1 if last edge mark is found, -1 if unknown edge type
 * 			or Error.
 */
int fs_parseEdge(const MemoryMappedFile &metadataFile, size_t &sectionOffset, int ignoreFlag,
                    bool init = false) {
	static uint32_t currentParentId;
	uint32_t parentId, childId;

	if (init) {
		currentParentId = 0;
		return 0;
	}
	const uint8_t* pSrc = metadataFile.seek(sectionOffset);
	parentId = get32bit(&pSrc);
	childId = get32bit(&pSrc);
	uint16_t edgeNameSize = get16bit(&pSrc);
	sectionOffset = metadataFile.offset(pSrc);

	if (!parentId && !childId) {  /// Last edge mark;
		return 1;
	}

	if (!edgeNameSize) {
		safs_pretty_syslog(
		    LOG_ERR, "loading edge: %" PRIu32 "->%" PRIu32 " error: empty name",
		    parentId, childId);
		return -1;
	}

	std::string name(pSrc, pSrc + edgeNameSize);
	sectionOffset += edgeNameSize;
	FSNode *child = fsnodes_id_to_node(childId);
	if (!child) {
		safs_pretty_syslog(
		    LOG_ERR,
		    "loading edge: %" PRIu32 ",%s->%" PRIu32 " error: child not found",
		    parentId, fsnodes_escape_name(name).c_str(), childId);
		if (ignoreFlag) {
			return 0;
		}
		return -1;
	}
	if (!parentId) {
		if (child->type == FSNode::kTrash) {
			gMetadata->trash.insert(
			    {TrashPathKey(child), hstorage::Handle(name)});
			gMetadata->trashspace += static_cast<FSNodeFile *>(child)->length;
			gMetadata->trashnodes++;
		} else if (child->type == FSNode::kReserved) {
			gMetadata->reserved.insert({child->id, hstorage::Handle(name)});
			gMetadata->reservedspace +=
			    static_cast<FSNodeFile *>(child)->length;
			gMetadata->reservednodes++;
		} else {
			safs_pretty_syslog(LOG_ERR,
			                   "loading edge: %" PRIu32 ",%s->%" PRIu32
			                   " error: bad child type (%c)\n",
			                   parentId, fsnodes_escape_name(name).c_str(),
			                   childId, child->type);
			return -1;
		}
	} else {
		FSNodeDirectory *parent = fsnodes_id_to_node<FSNodeDirectory>(parentId);
		if (!parent) {
			safs_pretty_syslog(LOG_ERR,
			                   "loading edge: %" PRIu32 ",%s->%" PRIu32
			                   " error: parent not found",
			                   parentId, fsnodes_escape_name(name).c_str(),
			                   childId);
			if (ignoreFlag) {
				parent =
				    fsnodes_id_to_node<FSNodeDirectory>(SPECIAL_INODE_ROOT);
				if (!parent || parent->type != FSNode::kDirectory) {
					safs_pretty_syslog(
					    LOG_ERR,
					    "loading edge: %" PRIu32 ",%s->%" PRIu32
					    " root dir not found !!!",
					    parentId, fsnodes_escape_name(name).c_str(), childId);
					return -1;
				}
				safs_pretty_syslog(LOG_ERR,
				                   "loading edge: %" PRIu32 ",%s->%" PRIu32
				                   " attaching node to root dir",
				                   parentId, fsnodes_escape_name(name).c_str(),
				                   childId);
				parentId = SPECIAL_INODE_ROOT;
			} else {
				safs_pretty_syslog(
				    LOG_ERR,
				    "use sfsmetarestore (option -i) to attach this "
				    "node to root dir\n");
				return -1;
			}
		}
		if (parent->type != FSNode::kDirectory) {
			safs_pretty_syslog(LOG_ERR,
			                   "loading edge: %" PRIu32 ",%s->%" PRIu32
			                   " error: bad parent type (%c)",
			                   parentId, fsnodes_escape_name(name).c_str(),
			                   childId, parent->type);
			if (ignoreFlag) {
				parent =
				    fsnodes_id_to_node<FSNodeDirectory>(SPECIAL_INODE_ROOT);
				if (!parent || parent->type != FSNode::kDirectory) {
					safs_pretty_syslog(
					    LOG_ERR,
					    "loading edge: %" PRIu32 ",%s->%" PRIu32
					    " root dir not found !!!",
					    parentId, fsnodes_escape_name(name).c_str(), childId);
					return -1;
				}
				safs_pretty_syslog(LOG_ERR,
				                   "loading edge: %" PRIu32 ",%s->%" PRIu32
				                   " attaching node to root dir",
				                   parentId, fsnodes_escape_name(name).c_str(),
				                   childId);
				parentId = SPECIAL_INODE_ROOT;
			} else {
				safs_pretty_syslog(
				    LOG_ERR,
				    "use sfsmetarestore (option -i) to attach this "
				    "node to root dir\n");
				return -1;
			}
		}
		if (currentParentId != parentId) {
			if (parent->entries.size() > 0) {
				safs_pretty_syslog(LOG_ERR,
				                   "loading edge: %" PRIu32 ",%s->%" PRIu32
				                   " error: parent node sequence error",
				                   parentId, fsnodes_escape_name(name).c_str(),
				                   childId);
				return -1;
			}
			currentParentId = parentId;
		}

		auto it = parent->entries.insert({hstorage::Handle(name), child}).first;
		parent->entries_hash ^= (*it).first.hash();

		child->parent.push_back(parent->id);
		if (child->type == FSNode::kDirectory) {
			parent->nlink++;
		}

		statsrecord sr;
		fsnodes_get_stats(child, &sr);
		fsnodes_add_stats(parent, &sr);
	}
	return 0;
}

void fs_storenode(FSNode *f, FILE *fd) {
	uint8_t unodebuff[1 + 4 + 1 + 2 + 4 + 4 + 4 + 4 + 4 + 4 + 8 + 4 + 2 +
	                  8 * 65536 + 4 * 65536 + 4];
	uint8_t *ptr, *chptr;
	uint32_t i, indx, ch, sessionids;
	std::string name;

	if (f == NULL) {  // last node
		fputc(0, fd);
		return;
	}
	ptr = unodebuff;
	put8bit(&ptr, f->type);
	put32bit(&ptr, f->id);
	put8bit(&ptr, f->goal);
	put16bit(&ptr, f->mode);
	put32bit(&ptr, f->uid);
	put32bit(&ptr, f->gid);
	put32bit(&ptr, f->atime);
	put32bit(&ptr, f->mtime);
	put32bit(&ptr, f->ctime);
	put32bit(&ptr, f->trashtime);

	FSNodeFile *node_file = static_cast<FSNodeFile *>(f);

	switch (f->type) {
	case FSNode::kDirectory:
	case FSNode::kSocket:
	case FSNode::kFifo:
		if (fwrite(unodebuff, 1, 1 + 4 + 1 + 2 + 4 + 4 + 4 + 4 + 4 + 4, fd) !=
		    (size_t)(1 + 4 + 1 + 2 + 4 + 4 + 4 + 4 + 4 + 4)) {
			safs_pretty_syslog(LOG_NOTICE, "fwrite error");
			return;
		}
		break;
	case FSNode::kBlockDev:
	case FSNode::kCharDev:
		put32bit(&ptr, static_cast<FSNodeDevice *>(f)->rdev);
		if (fwrite(unodebuff, 1, 1 + 4 + 1 + 2 + 4 + 4 + 4 + 4 + 4 + 4 + 4,
		           fd) != (size_t)(1 + 4 + 1 + 2 + 4 + 4 + 4 + 4 + 4 + 4 + 4)) {
			safs_pretty_syslog(LOG_NOTICE, "fwrite error");
			return;
		}
		break;
	case FSNode::kSymlink:
		name = (std::string) static_cast<FSNodeSymlink *>(f)->path;
		put32bit(&ptr, name.length());
		if (fwrite(unodebuff, 1, 1 + 4 + 1 + 2 + 4 + 4 + 4 + 4 + 4 + 4 + 4,
		           fd) != (size_t)(1 + 4 + 1 + 2 + 4 + 4 + 4 + 4 + 4 + 4 + 4)) {
			safs_pretty_syslog(LOG_NOTICE, "fwrite error");
			return;
		}
		if (fwrite(name.c_str(), 1, name.length(), fd) !=
		    (size_t)(static_cast<FSNodeSymlink *>(f)->path_length)) {
			safs_pretty_syslog(LOG_NOTICE, "fwrite error");
			return;
		}
		break;
	case FSNode::kFile:
	case FSNode::kTrash:
	case FSNode::kReserved:
		put64bit(&ptr, node_file->length);
		ch = node_file->chunkCount();
		put32bit(&ptr, ch);
		sessionids = std::min<int>(node_file->sessionid.size(), 65535);
		put16bit(&ptr, sessionids);

		if (fwrite(unodebuff, 1,
		           1 + 4 + 1 + 2 + 4 + 4 + 4 + 4 + 4 + 4 + 8 + 4 + 2, fd) !=
		    (size_t)(1 + 4 + 1 + 2 + 4 + 4 + 4 + 4 + 4 + 4 + 8 + 4 + 2)) {
			safs_pretty_syslog(LOG_NOTICE, "fwrite error");
			return;
		}

		indx = 0;
		while (ch > 65536) {
			chptr = ptr;
			for (i = 0; i < 65536; i++) {
				put64bit(&chptr, node_file->chunks[indx]);
				indx++;
			}
			if (fwrite(ptr, 1, 8 * 65536, fd) != (size_t)(8 * 65536)) {
				safs_pretty_syslog(LOG_NOTICE, "fwrite error");
				return;
			}
			ch -= 65536;
		}

		chptr = ptr;
		for (i = 0; i < ch; i++) {
			put64bit(&chptr, node_file->chunks[indx]);
			indx++;
		}

		sessionids = 0;
		for (const auto &sid : node_file->sessionid) {
			if (sessionids >= 65535) {
				break;
			}
			put32bit(&chptr, sid);
			sessionids++;
		}

		if (fwrite(ptr, 1, 8 * ch + 4 * sessionids, fd) !=
		    (size_t)(8 * ch + 4 * sessionids)) {
			safs_pretty_syslog(LOG_NOTICE, "fwrite error");
			return;
		}
	}
}

/**
 * @brief
 * @param pSrc A pointer to the data storing all nodes.
 * @param sectionOffset A reference to point to the next node attribute.
 * @return 0 on success, 1 if last node mark is found, -1 if unknown node type.
 */
int8_t fs_parseNode(const MemoryMappedFile &metadataFile, size_t &sectionOffset) {
	static constexpr uint32_t kChunkSize = (1 << 16);

	uint8_t type;
	FSNode *node;
	const uint8_t *pSrc = metadataFile.seek(sectionOffset);
	type = get8bit(&pSrc);
	if (!type) {  // last node
		sectionOffset = metadataFile.offset(pSrc);
		return 1;
	}

	node = FSNode::create(type);
	passert(node);
	node->id = get32bit(&pSrc);
	node->goal = get8bit(&pSrc);
	node->mode = get16bit(&pSrc);
	node->uid = get32bit(&pSrc);
	node->gid = get32bit(&pSrc);
	node->atime = get32bit(&pSrc);
	node->mtime = get32bit(&pSrc);
	node->ctime = get32bit(&pSrc);
	node->trashtime = get32bit(&pSrc);
	FSNodeFile *nodeFile = static_cast<FSNodeFile *>(node);

	uint32_t nodeNameLength;
	uint32_t chunkAmount;
	uint16_t sessionIds;
	uint32_t index;

	switch (type) {
	case FSNode::kDirectory:
		gMetadata->dirnodes++;
		break;
	case FSNode::kSocket:
	case FSNode::kFifo:  /// No extra info to Parse
		break;
	case FSNode::kBlockDev:
	case FSNode::kCharDev:
		static_cast<FSNodeDevice *>(node)->rdev = get32bit(&pSrc);
		break;
	case FSNode::kSymlink:
		nodeNameLength = get32bit(&pSrc);
		static_cast<FSNodeSymlink *>(node)->path_length = nodeNameLength;
		if (nodeNameLength > 0) {  /// TODO check if pointer is shifted by
			                       /// memcpy
			std::memcpy(&(static_cast<FSNodeSymlink *>(node)->path.data()),
			            pSrc, nodeNameLength);
		}
		break;
	case FSNode::kFile:
	case FSNode::kTrash:
	case FSNode::kReserved:
		nodeFile->length = get64bit(&pSrc);
		chunkAmount = get32bit(&pSrc);
		sessionIds = get16bit(&pSrc);
		nodeFile->chunks.resize(chunkAmount);
		index = 0;
		while (chunkAmount > kChunkSize) {
			for (uint32_t i = 0; i < kChunkSize; i++) {
				nodeFile->chunks[index++] = get64bit(&pSrc);
			}
			chunkAmount -= kChunkSize;
		}
		for (uint32_t i = 0; i < chunkAmount; i++) {
			nodeFile->chunks[index++] = get64bit(&pSrc);
		}
		while (sessionIds) {
			uint32_t sessionId = get32bit(&pSrc);
			nodeFile->sessionid.push_back(sessionId);
#ifndef METARESTORE
			matoclserv_add_open_file(sessionId, node->id);
#endif
			sessionIds--;
		}
		fsnodes_quota_update(node,
		                     {{QuotaResource::kSize, +fsnodes_get_size(node)}});
		gMetadata->filenodes++;
		break;
	default:
		safs_pretty_syslog(LOG_ERR, "loading node: unrecognized node type: %c",
		                   type);
		fsnodes_quota_update(node, {{QuotaResource::kInodes, +1}});
		sectionOffset = metadataFile.offset(pSrc);
		return -1;
	}
	uint32_t nodeIndex = NODEHASHPOS(node->id);
	node->next = gMetadata->nodehash[nodeIndex];
	gMetadata->nodehash[nodeIndex] = node;
	gMetadata->inode_pool.markAsAcquired(node->id);
	gMetadata->nodes++;
	fsnodes_quota_update(node, {{QuotaResource::kInodes, +1}});
	sectionOffset = metadataFile.offset(pSrc);
	return 0;
}

void fs_storenodes(FILE *fd) {
	uint32_t i;
	FSNode *p;
	for (i = 0; i < NODEHASHSIZE; i++) {
		for (p = gMetadata->nodehash[i]; p; p = p->next) {
			fs_storenode(p, fd);
		}
	}
	fs_storenode(NULL, fd);  // end marker
}

void fs_storeedgelist(FSNodeDirectory *parent, FILE *fd) {
	for (const auto &entry : parent->entries) {
		fs_storeedge(parent, entry.second, (std::string)entry.first, fd);
	}
}

void fs_storeedgelist(const TrashPathContainer &data, FILE *fd) {
	for (const auto &entry : data) {
		FSNode *child = fsnodes_id_to_node(entry.first.id);
		fs_storeedge(nullptr, child, (std::string)entry.second, fd);
	}
}

void fs_storeedgelist(const ReservedPathContainer &data, FILE *fd) {
	for (const auto &entry : data) {
		FSNode *child = fsnodes_id_to_node(entry.first);
		fs_storeedge(nullptr, child, (std::string)entry.second, fd);
	}
}

void fs_storeedges_rec(FSNodeDirectory *f, FILE *fd) {
	fs_storeedgelist(f, fd);
	for (const auto &entry : f->entries) {
		if (entry.second->type == FSNode::kDirectory) {
			fs_storeedges_rec(static_cast<FSNodeDirectory *>(entry.second), fd);
		}
	}
}

void fs_storeedges(FILE *fd) {
	fs_storeedges_rec(gMetadata->root, fd);
	fs_storeedgelist(gMetadata->trash, fd);
	fs_storeedgelist(gMetadata->reserved, fd);
	fs_storeedge(nullptr, nullptr, std::string(), fd);  // end marker
}

static void fs_storequotas(FILE *fd) {
	const std::vector<QuotaEntry> &entries =
	    gMetadata->quota_database.getEntries();
	fs_store_generic(fd, entries);
}

static void fs_storelocks(FILE *fd) {
	gMetadata->flock_locks.store(fd);
	gMetadata->posix_locks.store(fd);
}

int fs_lostnode(FSNode *p) {
	uint8_t artname[40];
	uint32_t i, l;
	i = 0;
	do {
		if (i == 0) {
			l = snprintf((char *)artname, 40, "lost_node_%" PRIu32, p->id);
		} else {
			l = snprintf((char *)artname, 40, "lost_node_%" PRIu32 ".%" PRIu32,
			             p->id, i);
		}
		HString name((const char *)artname, l);
		if (!fsnodes_nameisused(gMetadata->root, name)) {
			fsnodes_link(0, gMetadata->root, p, name);
			return 1;
		}
		i++;
	} while (i);
	return -1;
}

int fs_checknodes(int ignoreflag) {
	uint32_t i;
	FSNode *p;
	for (i = 0; i < NODEHASHSIZE; i++) {
		for (p = gMetadata->nodehash[i]; p; p = p->next) {
			if (p->parent.empty() && p != gMetadata->root &&
			    (p->type != FSNode::kTrash) && (p->type != FSNode::kReserved)) {
				safs_pretty_syslog(LOG_ERR, "found orphaned inode: %" PRIu32,
				                   p->id);
				if (ignoreflag) {
					if (fs_lostnode(p) < 0) {
						return -1;
					}
				} else {
					safs_pretty_syslog(LOG_ERR,
					                   "use sfsmetarestore (option -i) to "
					                   "attach this node to root dir\n");
					return -1;
				}
			}
		}
	}
	return 1;
}

bool fs_loadnodes(MetadataSectionLoaderOptions options) {
	int8_t s;
	do {
		s = fs_parseNode(options.metadataFile, options.offset);
		if (s < 0) {
			return false;
		}
	} while (s == 0);
	return true;
}

bool fs_loadedges(MetadataSectionLoaderOptions options) {
	int s;
	fs_parseEdge(options.metadataFile, options.offset, options.ignoreflag, true);
	do {
		s = fs_parseEdge(options.metadataFile, options.offset, options.ignoreflag);
		if (s < 0) {
			return false;
		}
	} while (s == 0);
	return true;
}

static bool fs_loadquotas(MetadataSectionLoaderOptions options) {
	try {
		std::vector<QuotaEntry> entries;
		fs_load_generic(options.metadataFile, options.offset, entries);
		for (const auto &entry : entries) {
			gMetadata->quota_database.set(
			    entry.entryKey.owner.ownerType, entry.entryKey.owner.ownerId,
			    entry.entryKey.rigor, entry.entryKey.resource, entry.limit);
		}
		gMetadata->quota_checksum = gMetadata->quota_database.checksum();
	} catch (Exception &ex) {
		safs_pretty_syslog(LOG_ERR, "loading quotas: %s", ex.what());
		if (!options.ignoreflag || ex.status() != SAUNAFS_STATUS_OK) {
			return false;
		}
	}
	return true;
}

static bool fs_loadlocks(MetadataSectionLoaderOptions options) {
	try {
		gMetadata->flock_locks.load(options.metadataFile, options.offset);
		gMetadata->posix_locks.load(options.metadataFile, options.offset);
	} catch (Exception &ex) {
		safs_pretty_syslog(LOG_ERR, "loading locks: %s", ex.what());
		if (!options.ignoreflag || ex.status() != SAUNAFS_STATUS_OK) {
			return false;
		}
	}
	return true;
}

void fs_storefree(FILE *fd) {
	uint8_t wbuff[8 * 1024], *ptr;

	uint32_t l = gMetadata->inode_pool.detainedCount();

	ptr = wbuff;
	put32bit(&ptr, l);
	if (fwrite(wbuff, 1, 4, fd) != (size_t)4) {
		safs_pretty_syslog(LOG_NOTICE, "fwrite error");
		return;
	}
	l = 0;
	ptr = wbuff;

	for (const auto &n : gMetadata->inode_pool) {
		if (l == 1024) {
			if (fwrite(wbuff, 1, 8 * 1024, fd) != (size_t)(8 * 1024)) {
				safs_pretty_syslog(LOG_NOTICE, "fwrite error");
				return;
			}
			l = 0;
			ptr = wbuff;
		}
		put32bit(&ptr, n.id);
		put32bit(&ptr, n.ts);
		l++;
	}
	if (l > 0) {
		if (fwrite(wbuff, 1, 8 * l, fd) != (size_t)(8 * l)) {
			safs_pretty_syslog(LOG_NOTICE, "fwrite error");
			return;
		}
	}
}

bool fs_loadfree(MetadataSectionLoaderOptions options) {
	const uint8_t *ptr;
	uint32_t freeNodesToLoad, freeNodesNumber;

	try {
		ptr = options.metadataFile.seek(options.offset);
	} catch (const std::exception &e) {
		safs_pretty_errlog(LOG_INFO, "loading free nodes: %s", e.what());
		return false;
	}

	freeNodesNumber = get32bit(&ptr);

	if (options.sectionLength && freeNodesNumber != (options.sectionLength - 4) / 8) {
		safs_pretty_errlog(LOG_INFO,
		                   "loading free nodes: section size doesn't match "
		                   "number of free nodes");
		freeNodesNumber = (options.sectionLength - 4) / 8;
	}

	freeNodesToLoad = 0;
	while (freeNodesNumber > 0) {
		if (freeNodesToLoad == 0) {
			freeNodesToLoad = (freeNodesNumber > 1024) ? 1024 : freeNodesNumber;
		}
		uint32_t id = get32bit(&ptr);
		uint32_t timestamp = get32bit(&ptr);
		gMetadata->inode_pool.detain(id, timestamp, true);
		freeNodesToLoad--;
		freeNodesNumber--;
	}
	options.offset = options.metadataFile.offset(ptr);
	return true;
}

static int process_section(const char *label, uint8_t (&hdr)[16], uint8_t *&ptr,
                           off_t &offbegin, off_t &offend, FILE *&fd) {
	offend = ftello(fd);
	memcpy(hdr, label, 8);
	ptr = hdr + 8;
	put64bit(&ptr, offend - offbegin - 16);
	fseeko(fd, offbegin, SEEK_SET);
	if (fwrite(hdr, 1, 16, fd) != (size_t)16) {
		safs_pretty_syslog(LOG_NOTICE, "fwrite error");
		return SAUNAFS_ERROR_IO;
	}
	offbegin = offend;
	fseeko(fd, offbegin + 16, SEEK_SET);
	return SAUNAFS_STATUS_OK;
}

void fs_store(FILE *fd, uint8_t fver) {
	uint8_t hdr[16];
	uint8_t *ptr;
	off_t offbegin, offend;

	ptr = hdr;
	put32bit(&ptr, gMetadata->maxnodeid);
	put64bit(&ptr, gMetadata->metaversion);
	put32bit(&ptr, gMetadata->nextsessionid);
	if (fwrite(hdr, 1, 16, fd) != (size_t)16) {
		safs_pretty_syslog(LOG_NOTICE, "fwrite error");
		return;
	}
	if (fver >= kMetadataVersionWithSections) {
		offbegin = ftello(fd);
		fseeko(fd, offbegin + 16, SEEK_SET);
	} else {
		offbegin = 0;  // makes some old compilers happy
	}
	fs_storenodes(fd);
	if (fver >= kMetadataVersionWithSections) {
		if (process_section("NODE 1.0", hdr, ptr, offbegin, offend, fd) !=
		    SAUNAFS_STATUS_OK) {
			return;
		}
	}
	fs_storeedges(fd);
	if (fver >= kMetadataVersionWithSections) {
		if (process_section("EDGE 1.0", hdr, ptr, offbegin, offend, fd) !=
		    SAUNAFS_STATUS_OK) {
			return;
		}
	}
	fs_storefree(fd);
	if (fver >= kMetadataVersionWithSections) {
		if (process_section("FREE 1.0", hdr, ptr, offbegin, offend, fd) !=
		    SAUNAFS_STATUS_OK) {
			return;
		}
		xattr_store(fd);
		if (process_section("XATR 1.0", hdr, ptr, offbegin, offend, fd) !=
		    SAUNAFS_STATUS_OK) {
			return;
		}
		fs_store_acls(fd);
		if (process_section("ACLS 1.2", hdr, ptr, offbegin, offend, fd) !=
		    SAUNAFS_STATUS_OK) {
			return;
		}
		fs_storequotas(fd);
		if (process_section("QUOT 1.1", hdr, ptr, offbegin, offend, fd) !=
		    SAUNAFS_STATUS_OK) {
			return;
		}
		fs_storelocks(fd);
		if (process_section("FLCK 1.0", hdr, ptr, offbegin, offend, fd) !=
		    SAUNAFS_STATUS_OK) {
			return;
		}
	}
	chunk_store(fd);
	if (fver >= kMetadataVersionWithSections) {
		if (process_section("CHNK 1.0", hdr, ptr, offbegin, offend, fd) !=
		    SAUNAFS_STATUS_OK) {
			return;
		}

		fseeko(fd, offend, SEEK_SET);
		memcpy(hdr, "[SFS EOF MARKER]", 16);
		if (fwrite(hdr, 1, 16, fd) != (size_t)16) {
			safs_pretty_syslog(LOG_NOTICE, "fwrite error");
			return;
		}
	}
}

void fs_store_fd(FILE *fd) {
#if SAUNAFS_VERSHEX >= SAUNAFS_VERSION(2, 9, 0)
	const char hdr[] = SFSSIGNATURE "M 2.9";
	const uint8_t metadataVersion = kMetadataVersionWithLockIds;
#elif SAUNAFS_VERSHEX >= SAUNAFS_VERSION(1, 6, 29)
	const char hdr[] = SFSSIGNATURE "M 2.0";
	const uint8_t metadataVersion = kMetadataVersionWithSections;
#else
	const char hdr[] = SFSSIGNATURE "M 1.6";
	const uint8_t metadataVersion = kMetadataVersionSaunaFS;
#endif

	if (fwrite(&hdr, 1, sizeof(hdr) - 1, fd) != sizeof(hdr) - 1) {
		safs_pretty_syslog(LOG_NOTICE, "fwrite error");
	} else {
		fs_store(fd, metadataVersion);
	}
}

static constexpr uint8_t kMetadataSectionHeaderSize = 16;
static constexpr uint8_t kMetadataSectionNameSize = 8;

struct FutureInfo {
	std::string sectionName;
	std::string sectionDescription;
	std::future<bool> future;
};

struct MetadataSection {
	MetadataSection(
	    std::string_view name_, std::string_view description_,
	    std::function<bool(MetadataSectionLoaderOptions)> load_,
	    bool asyncLoad_ = true,
	    bool isLegacy_ = false)
	    : name(name_),
	      description(description_),
	      load(std::move(load_)),
	      asyncLoad(asyncLoad_),
	      isLegacy(isLegacy_) {}

	bool isSection(const uint8_t *sectionPtr) const {
		return memcmp(sectionPtr, name.data(), name.size()) == kOpSuccess;
	}

	std::string_view name;
	std::string_view description;
	std::function<bool(MetadataSectionLoaderOptions)> load;
	bool asyncLoad;
	bool isLegacy;
};

static const std::vector<MetadataSection> kMetadataSections = {
    /// Synchronously loaded sections (in order)
    MetadataSection("NODE 1.0", "Nodes", fs_loadnodes, false),
    /// Asynchronously loaded sections:
    MetadataSection("EDGE 1.0", "Edges", fs_loadedges),
    MetadataSection("FREE 1.0", "Free Nodes", fs_loadfree),
    MetadataSection("XATR 1.0", "Extended Attributes", xattr_load),
    MetadataSection("ACLS 1.0", "Access Control Lists", fs_load_legacy_acls),
    MetadataSection("ACLS 1.1", "Access Control Lists", fs_load_posix_acls),
    MetadataSection("ACLS 1.2", "Access Control Lists", fs_load_acls),
    MetadataSection("QUOT 1.1", "Quotas", fs_loadquotas),
    MetadataSection("FLCK 1.0", "File Locks", fs_loadlocks),
    MetadataSection("CHNK 1.0", "Chunks", chunk_load),
    /// Legacy Sections (won't be loaded):
    MetadataSection("QUOT 1.0", "Quotas",
                    [](const MetadataSectionLoaderOptions &) { return true; }, true, true),
    MetadataSection("LOCK 1.0", "Locks",
                    [](const MetadataSectionLoaderOptions &) { return true; }, true, true),
};

bool fileMatchesMetadataSection(const uint8_t *sectionPtr, const MetadataSection &section) {
	return section.isSection(sectionPtr);
}

bool loadSection(const MetadataSection &section,
                 const MetadataSectionLoaderOptions options) {
	    try {
			if (section.load(options)) {
				safs_pretty_syslog(LOG_INFO, "Section loaded successfully (%s)",
								   section.name.data());
			    return true;
			} else {
				safs_pretty_syslog(LOG_ERR, "error reading section (%s)",
								   section.name.data());
			}
		} catch (const std::exception &e) {
		    safs_pretty_syslog(LOG_ERR, "Exception while processing section (%s)",
		                       section.name.data());
		    throw MetadataConsistencyException(e.what());
	    }
	    return false;
}

std::future<bool> loadSectionAsync(const MetadataSection &section,
                                   MetadataSectionLoaderOptions options) {
		return std::async(std::launch::async, loadSection, section, options);
}

void loadSectionAsync(const MetadataSection &section,
					  MetadataSectionLoaderOptions options,
					  std::vector<FutureInfo> &futures) {
	FutureInfo future;
	future.sectionName = section.name;
	future.sectionDescription = section.description;
	future.future = loadSectionAsync(section, options);
	futures.push_back(std::move(future));
}

bool isEndOfMetadata(const uint8_t *sectionPtr) {
	static constexpr std::string_view kMetadataTrailer("[" SFSSIGNATURE
	                                                   " EOF MARKER]");
	return memcmp(sectionPtr, kMetadataTrailer.data(),
	              ::kMetadataSectionHeaderSize) == kOpSuccess;
}


int fs_load(const MemoryMappedFile &metadataFile, int ignoreflag) {

	static constexpr uint8_t kMetadataHeaderOffset = 8;

	/// Skip File Signature
	const uint8_t *metadataHeaderPtr= metadataFile.seek(kMetadataHeaderOffset);

	gMetadata->maxnodeid = get32bit(&metadataHeaderPtr);
	gMetadata->metaversion = get64bit(&metadataHeaderPtr);
	gMetadata->nextsessionid = get32bit(&metadataHeaderPtr);

	size_t offsetBegin = metadataFile.offset(metadataHeaderPtr);

	/// First secuential pass to gather all section lengths
	std::unordered_map<std::string_view, std::pair<size_t, uint64_t>>
	    sectionMarkers;
	uint8_t *sectionPtr = metadataFile.seek(offsetBegin);
	while (!isEndOfMetadata(sectionPtr)) {
		const uint8_t *sectionLengthPtr = sectionPtr + kMetadataSectionNameSize;
		uint64_t sectionLength = get64bit(&sectionLengthPtr);
		const uint8_t *sectionDataPtr = sectionLengthPtr;
		for (const auto &section : kMetadataSections) {
			if (fileMatchesMetadataSection(sectionPtr, section)) {
				sectionMarkers[section.name] = {
				    metadataFile.offset(sectionDataPtr), sectionLength};
				break;
			}
		}
		sectionPtr = metadataFile.seek(
		    offsetBegin += sectionLength + kMetadataSectionHeaderSize);
	}

	std::vector<FutureInfo> futures;
	for (const auto &section : kMetadataSections) {
		if (sectionMarkers.find(section.name) == sectionMarkers.end()) {
			continue;
		}
		size_t sectionOffset = sectionMarkers[section.name].first;
		uint64_t sectionLength = sectionMarkers[section.name].second;
		if (section.isLegacy) {
			safs_pretty_syslog(LOG_WARNING, "legacy section found (%s)",
			                   section.name.data());
			continue;
		}
		auto options = MetadataSectionLoaderOptions{
		    metadataFile, sectionOffset,
		    ignoreflag, sectionLength, true};

		if (!section.asyncLoad) {
			loadSection(section, options);
		} else {
			MetadataSectionLoaderOptions asyncOptionsCopy = options;
			loadSectionAsync(section, asyncOptionsCopy, futures);
		}
	}
	/// Wait for all futures to finish
	for (auto &future : futures) {
		future.future.wait();
	}

	safs_pretty_syslog_attempt(
	    LOG_INFO, "checking filesystem consistency of the metadata file");
	fflush(stderr);
	gMetadata->root = fsnodes_id_to_node<FSNodeDirectory>(SPECIAL_INODE_ROOT);
	if (gMetadata->root == nullptr) {
		safs_pretty_syslog(LOG_ERR,
		                   "error reading metadata (root node not found)");
		return kOpFailure;
	}
	if (gMetadata->root->type != FSNode::kDirectory) {
		safs_pretty_syslog(
		    LOG_ERR, "error reading metadata (root node not a directory)");
		return kOpFailure;
	}
	if (fs_checknodes(ignoreflag) < 0) {
		return kOpFailure;
	}
	return kOpSuccess;
}

#ifndef METARESTORE

void fs_new(void) {
	uint32_t nodepos;
	gMetadata->maxnodeid = SPECIAL_INODE_ROOT;
	gMetadata->metaversion = 1;
	gMetadata->nextsessionid = 1;
	gMetadata->root =
	    static_cast<FSNodeDirectory *>(FSNode::create(FSNode::kDirectory));
	gMetadata->root->id = SPECIAL_INODE_ROOT;
	gMetadata->root->ctime = gMetadata->root->mtime = gMetadata->root->atime =
	    eventloop_time();
	gMetadata->root->goal = DEFAULT_GOAL;
	gMetadata->root->trashtime = DEFAULT_TRASHTIME;
	gMetadata->root->mode = 0777;
	gMetadata->root->uid = 0;
	gMetadata->root->gid = 0;
	nodepos = NODEHASHPOS(gMetadata->root->id);
	gMetadata->root->next = gMetadata->nodehash[nodepos];
	gMetadata->nodehash[nodepos] = gMetadata->root;
	gMetadata->inode_pool.markAsAcquired(gMetadata->root->id);
	chunk_newfs();
	gMetadata->nodes = 1;
	gMetadata->dirnodes = 1;
	gMetadata->filenodes = 0;
	fs_checksum(ChecksumMode::kForceRecalculate);
	fsnodes_quota_update(gMetadata->root, {{QuotaResource::kInodes, +1}});
}

#endif

int fs_emergency_storeall(const std::string &fname) {
	cstream_t fd(fopen(fname.c_str(), "w"));
	if (fd == nullptr) {
		return -1;
	}

	fs_store_fd(fd.get());

	if (ferror(fd.get()) != 0) {
		return -1;
	}
	safs_pretty_syslog(
	    LOG_WARNING,
	    "metadata were stored to emergency file: %s - please copy this file to "
	    "your default location as '%s'",
	    fname.c_str(), kMetadataFilename);
	return 0;
}

int fs_emergency_saves() {
#if defined(SAUNAFS_HAVE_PWD_H) && defined(SAUNAFS_HAVE_GETPWUID)
	struct passwd *p;
#endif
	if (fs_emergency_storeall(kMetadataEmergencyFilename) == 0) {
		return 0;
	}
#if defined(SAUNAFS_HAVE_PWD_H) && defined(SAUNAFS_HAVE_GETPWUID)
	p = getpwuid(getuid());
	if (p) {
		std::string fname = p->pw_dir;
		fname.append("/").append(kMetadataEmergencyFilename);
		if (fs_emergency_storeall(fname) == 0) {
			return 0;
		}
	}
#endif
	std::string metadata_emergency_filename = kMetadataEmergencyFilename;
	if (fs_emergency_storeall("/" + metadata_emergency_filename) == 0) {
		return 0;
	}
	if (fs_emergency_storeall("/tmp/" + metadata_emergency_filename) == 0) {
		return 0;
	}
	if (fs_emergency_storeall("/var/" + metadata_emergency_filename) == 0) {
		return 0;
	}
	if (fs_emergency_storeall("/usr/" + metadata_emergency_filename) == 0) {
		return 0;
	}
	if (fs_emergency_storeall("/usr/share/" + metadata_emergency_filename) ==
	    0) {
		return 0;
	}
	if (fs_emergency_storeall("/usr/local/" + metadata_emergency_filename) ==
	    0) {
		return 0;
	}
	if (fs_emergency_storeall("/usr/local/var/" +
	                          metadata_emergency_filename) == 0) {
		return 0;
	}
	if (fs_emergency_storeall("/usr/local/share/" +
	                          metadata_emergency_filename) == 0) {
		return 0;
	}
	return -1;
}

#ifndef METARESTORE

/*
 * Load and apply changelogs.
 */
void fs_load_changelogs() {
	metadataserver::Personality personality = metadataserver::getPersonality();
	metadataserver::setPersonality(metadataserver::Personality::kShadow);
	/*
	 * We need to load 3 changelog files in extreme case.
	 * If we are being run as Shadow we need to download two
	 * changelog files:
	 * 1 - current changelog => "changelog.sfs.1"
	 * 2 - previous changelog in case Shadow connects during metadata dump,
	 *     that is "changelog.sfs.2"
	 * Beside this we received changelog lines that we stored in
	 * yet another changelog file => "changelog.sfs"
	 *
	 * If we are master we only really care for:
	 * "changelog.sfs.1" and "changelog.sfs" files.
	 */
	static const std::string changelogs[]{
	    std::string(kChangelogFilename) + ".2",
	    std::string(kChangelogFilename) + ".1", kChangelogFilename};
	restore_setverblevel(gVerbosity);
	bool oldExists = false;
	try {
		for (const std::string &s : changelogs) {
			std::string fullFileName =
			    fs::getCurrentWorkingDirectoryNoThrow() + "/" + s;
			if (fs::exists(s)) {
				oldExists = true;
				uint64_t first = changelogGetFirstLogVersion(s);
				uint64_t last = changelogGetLastLogVersion(s);
				if (last >= first) {
					if (last >= fs_getversion()) {
						fs_load_changelog(s);
					}
				} else {
					throw InitializeException(
					    "changelog " + fullFileName +
					    " inconsistent, "
					    "use sfsmetarestore to recover the filesystem; "
					    "current fs version: " +
					    std::to_string(fs_getversion()) +
					    ", first change in the file: " + std::to_string(first));
				}
			} else if (oldExists && s != kChangelogFilename) {
				safs_pretty_syslog(LOG_WARNING, "changelog `%s' missing",
				                   fullFileName.c_str());
			}
		}
	} catch (const FilesystemException &ex) {
		throw FilesystemException("error loading changelogs: " + ex.message());
	}
	fs_storeall(MetadataDumper::DumpType::kForegroundDump);
	metadataserver::setPersonality(personality);
}

/*
 * Load and apply given changelog file.
 */
void fs_load_changelog(const std::string &path) {
	std::string fullFileName =
	    fs::getCurrentWorkingDirectoryNoThrow() + "/" + path;
	std::ifstream changelog(path);
	std::string line;
	size_t end = 0;
	sassert(gMetadata->metaversion > 0);

	uint64_t first = 0;
	uint64_t id = 0;
	uint64_t skippedEntries = 0;
	uint64_t appliedEntries = 0;
	while (std::getline(changelog, line).good()) {
		id = std::stoull(line, &end);
		if (id < fs_getversion()) {
			++skippedEntries;
			continue;
		} else if (!first) {
			first = id;
		}
		++appliedEntries;
		uint8_t status = restore(path.c_str(), id, line.c_str() + end,
		                         RestoreRigor::kIgnoreParseErrors);
		if (status != SAUNAFS_STATUS_OK) {
			throw MetadataConsistencyException(
			    "can't apply changelog " + fullFileName, status);
		}
	}
	if (appliedEntries > 0) {
		safs_pretty_syslog_attempt(LOG_NOTICE,
		                           "%s: %" PRIu64 " changes applied (%" PRIu64
		                           " to %" PRIu64 "), %" PRIu64 " skipped",
		                           fullFileName.c_str(), appliedEntries, first,
		                           id, skippedEntries);
	} else if (skippedEntries > 0) {
		safs_pretty_syslog_attempt(LOG_NOTICE,
		                           "%s: skipped all %" PRIu64 " entries",
		                           fullFileName.c_str(), skippedEntries);
	} else {
		safs_pretty_syslog_attempt(LOG_NOTICE, "%s: file empty (ignored)",
		                           fullFileName.c_str());
	}
}

#endif

bool isNewMetadataFile([[maybe_unused]]const uint8_t *headerPtr) {
	static constexpr std::string_view kMetadataHeaderNew(SFSSIGNATURE "M NEW");
	[[maybe_unused]]static constexpr uint8_t kMetadataHeaderSize = 8;
#ifndef METARESTORE
	if (metadataserver::isMaster()) {  // special case - create new file system
		if (memcmp(headerPtr, kMetadataHeaderNew.data(), kMetadataHeaderSize) ==
		    kOpSuccess) {
			fs_new();
			safs_pretty_syslog(LOG_NOTICE, "empty filesystem created");
			// after creating new filesystem always create "back" file for using
			// in metarestore
			fs_storeall(MetadataDumper::kForegroundDump);
			return true;
		}
	}
#endif /* #ifndef METARESTORE */
	return false;
}

bool checkMetadataSignature(const MemoryMappedFile &metadataFile) {
	static constexpr std::string_view kMetadataHeaderV2_9(SFSSIGNATURE "M 2.9");
	static constexpr uint8_t kMetadataHeaderSize = 8;
	size_t kMetadataHeaderOffset{0};
	uint8_t *headerPtr;
	try {
		headerPtr = metadataFile.seek(kMetadataHeaderOffset);
		safs_pretty_syslog(LOG_INFO, "opened metadata file %s",
		                   metadataFile.filename().c_str());
	} catch (const std::exception &e) {
		throw e;
	}
	if (isNewMetadataFile(headerPtr)) {
		return false;
	}
	if (memcmp(headerPtr, kMetadataHeaderV2_9.data(), kMetadataHeaderSize) !=
	    kOpSuccess) {
		throw MetadataConsistencyException("wrong metadata header version");
	}
	return true;
}

void fs_loadall(const std::string &fname, int ignoreflag) {
	MemoryMappedFile metadataFile(fname);
	if (!checkMetadataSignature(metadataFile)) {
		return;
	}
	if (fs_load(metadataFile, ignoreflag) != kOpSuccess) {
		throw MetadataConsistencyException(MetadataStructureReadErrorMsg);
	}
	safs_pretty_syslog_attempt(LOG_INFO, "connecting files and chunks");
	fs_add_files_to_chunks();
	unlink(kMetadataTmpFilename);
	safs_pretty_syslog_attempt(LOG_INFO,
	                           "calculating checksum of the metadata");
	fs_checksum(ChecksumMode::kForceRecalculate);

#ifndef METARESTORE
	safs_pretty_syslog(
	    LOG_INFO,
	    "metadata file %s read (%" PRIu32 " inodes including %" PRIu32
	    " directory inodes and %" PRIu32 " file inodes, %" PRIu32 " chunks)",
	    fname.c_str(), gMetadata->nodes, gMetadata->dirnodes,
	    gMetadata->filenodes, chunk_count());
#else
	safs_pretty_syslog(LOG_INFO, "metadata file %s read", fname.c_str());
#endif
}

#ifndef METARESTORE

// Broadcasts information about status of the freshly finished
// metadata save process to interested modules.
void fs_broadcast_metadata_saved(uint8_t status) {
	matomlserv_broadcast_metadata_saved(status);
	matoclserv_broadcast_metadata_saved(status);
}

/*!
 * Commits successful metadata dump by renaming files.
 *
 * \return true iff up to date metadata.sfs file was created
 */
bool fs_commit_metadata_dump() {
	rotateFiles(kMetadataFilename, gStoredPreviousBackMetaCopies);
	try {
		fs::rename(kMetadataTmpFilename, kMetadataFilename);
		safs_silent_syslog(LOG_DEBUG, "master.fs.stored");
		return true;
	} catch (Exception &ex) {
		safs_pretty_syslog(LOG_ERR, "renaming %s to %s failed: %s",
		                   kMetadataTmpFilename, kMetadataFilename, ex.what());
	}

	// The previous step didn't return, so let's try to save us in other way
	std::string alternativeName =
	    kMetadataFilename + std::to_string(eventloop_time());
	try {
		fs::rename(kMetadataTmpFilename, alternativeName);
		safs_pretty_syslog(LOG_ERR, "emergency metadata file created as %s",
		                   alternativeName.c_str());
		return false;
	} catch (Exception &ex) {
		safs_pretty_syslog(LOG_ERR, "renaming %s to %s failed: %s",
		                   kMetadataTmpFilename, alternativeName.c_str(),
		                   ex.what());
	}

	// Nothing can be done...
	safs_pretty_syslog_attempt(
	    LOG_ERR, "trying to create emergency metadata file in foreground");
	fs_emergency_saves();
	return false;
}

// returns false in case of an error
uint8_t fs_storeall(MetadataDumper::DumpType dumpType) {
	if (gMetadata == nullptr) {
		// Periodic dump in shadow master or a request from saunafs-admin
		safs_pretty_syslog(LOG_INFO,
		                   "Can't save metadata because no metadata is loaded");
		return SAUNAFS_ERROR_NOTPOSSIBLE;
	}
	if (metadataDumper.inProgress()) {
		safs_pretty_syslog(LOG_ERR,
		                   "previous metadata save process hasn't finished yet "
		                   "- do not start another one");
		return SAUNAFS_ERROR_TEMP_NOTPOSSIBLE;
	}

	fs_erase_message_from_lockfile();  // We are going to do some changes in the
	                                   // data dir right now
	changelog_rotate();
	matomlserv_broadcast_logrotate();
	// child == true says that we forked
	// bg may be changed to dump in foreground in case of a fork error
	bool child =
	    metadataDumper.start(dumpType, fs_checksum(ChecksumMode::kGetCurrent));
	uint8_t status = SAUNAFS_STATUS_OK;

	if (dumpType == MetadataDumper::kForegroundDump) {
		cstream_t fd(fopen(kMetadataTmpFilename, "w"));
		if (fd == nullptr) {
			safs_pretty_syslog(LOG_ERR, "can't open metadata file");
			// try to save in alternative location - just in case
			fs_emergency_saves();
			if (child) {
				exit(1);
			}
			fs_broadcast_metadata_saved(SAUNAFS_ERROR_IO);
			return SAUNAFS_ERROR_IO;
		}

		fs_store_fd(fd.get());

		if (ferror(fd.get()) != 0) {
			safs_pretty_syslog(LOG_ERR, "can't write metadata");
			fd.reset();
			unlink(kMetadataTmpFilename);
			// try to save in alternative location - just in case
			fs_emergency_saves();
			if (child) {
				exit(1);
			}
			fs_broadcast_metadata_saved(SAUNAFS_ERROR_IO);
			return SAUNAFS_ERROR_IO;
		} else {
			if (fflush(fd.get()) == EOF) {
				safs_pretty_errlog(LOG_ERR, "metadata fflush failed");
			} else if (fsync(fileno(fd.get())) == -1) {
				safs_pretty_errlog(LOG_ERR, "metadata fsync failed");
			}
			fd.reset();
			if (!child) {
				// rename backups if no child was created, otherwise this is
				// handled by pollServe
				status = fs_commit_metadata_dump() ? SAUNAFS_STATUS_OK
				                                   : SAUNAFS_ERROR_IO;
			}
		}
		if (child) {
			printf("OK\n");  // give sfsmetarestore another chance
			exit(0);
		}
		fs_broadcast_metadata_saved(status);
	}
	sassert(!child);
	return status;
}

#endif
