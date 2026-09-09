/*
   Copyright 2026      Leil Storage OÜ

   This file is part of SaunaFS.

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

#include <algorithm>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/quota_database.h"
#include "common/richacl.h"
#include "common/serialization.h"
#include "master/acl_storage.h"
#include "master/chunks.h"
#include "master/filesystem_metadata.h"
#include "master/filesystem_operations.h"
#include "master/filesystem_operations_interface.h"
#include "master/filesystem_xattr.h"
#include "master/metadata_mutation_persistence_fdb.h"
#include "master/metadata_writer_fdb.h"
#include "slogger/slogger.h"

void MetadataMutationPersistenceFDB::attachWriter(MetadataWriterFDB &writer) { writer_ = &writer; }

void MetadataMutationPersistenceFDB::onNodeChanged(FSNode *node) {
	if (node == nullptr) {
		safs::log_err("{}: received null node, skipping metadata update", __func__);
		return;
	}
	if (writer_ != nullptr) {
		writer_->enqueue(std::make_unique<NodeUpdateEvent>(node));
	} else {
		dirtyNodes_.insert(node->id);
	}
}

void MetadataMutationPersistenceFDB::onNodeRemoved(inode_t nodeId) {
	if (writer_ != nullptr) {
		writer_->enqueue(std::make_unique<NodeRemoveEvent>(nodeId));
	} else {
		dirtyNodes_.insert(nodeId);
	}
}

void MetadataMutationPersistenceFDB::onEdgeChanged(inode_t parentId, inode_t childId,
                                                   const HString &name) {
	if (writer_ != nullptr) {
		writer_->enqueue(std::make_unique<EdgeUpdateEvent>(parentId, name, childId));
	} else {
		dirtyEdges_.emplace(parentId, name);
	}
}

void MetadataMutationPersistenceFDB::onEdgeRemoved(inode_t parentId, const HString &name) {
	if (writer_ != nullptr) {
		writer_->enqueue(std::make_unique<EdgeRemoveEvent>(parentId, name));
	} else {
		dirtyEdges_.emplace(parentId, name);
	}
}

void MetadataMutationPersistenceFDB::onXAttrInodeRemoved(inode_t inode) {
	if (writer_ != nullptr) {
		writer_->enqueue(std::make_unique<XAttrInodeRemoveEvent>(inode));
	} else {
		dirtyXattrInodes_.insert(inode);
	}
}

void MetadataMutationPersistenceFDB::onXAttrChanged(inode_t inode, std::span<const uint8_t> name,
                                                    std::span<const uint8_t> value) {
	if (writer_ != nullptr) {
		writer_->enqueue(std::make_unique<XAttrUpdateEvent>(inode, name, value));
	} else {
		dirtyXattrs_.emplace(inode, std::vector<uint8_t>(name.begin(), name.end()));
	}
}

void MetadataMutationPersistenceFDB::onXAttrRemoved(inode_t inode, std::span<const uint8_t> name) {
	if (writer_ != nullptr) {
		writer_->enqueue(std::make_unique<XAttrRemoveEvent>(inode, name));
	} else {
		dirtyXattrs_.emplace(inode, std::vector<uint8_t>(name.begin(), name.end()));
	}
}

PersistAction MetadataMutationPersistenceFDB::onQuotaChanged(QuotaOwnerType ownerType,
                                                             inode_t ownerId) {
	if (writer_ == nullptr) {
		dirtyQuotaOwners_.emplace(ownerType, ownerId);
		return PersistAction::kDeferred;
	}

	// Snapshot the owner's current soft/hard limits now (the signal fires synchronously, after the
	// quotaDatabase mutation). If the owner has no limits left, persist its removal instead.
	const auto *limits = gMetadata->quotaDatabase.get(ownerType, ownerId);
	if (limits == nullptr) {
		writer_->enqueue(std::make_unique<QuotaRemoveEvent>(ownerType, ownerId));
		return PersistAction::kRemoved;
	}

	std::vector<QuotaEntry> entries;
	for (const auto rigor : {QuotaRigor::kSoft, QuotaRigor::kHard}) {
		for (const auto resource : {QuotaResource::kInodes, QuotaResource::kSize}) {
			const uint64_t limit = (*limits)[static_cast<int>(rigor)][static_cast<int>(resource)];
			entries.emplace_back(QuotaEntryKey{QuotaOwner{ownerType, ownerId}, rigor, resource},
			                     limit);
		}
	}
	writer_->enqueue(std::make_unique<QuotaUpdateEvent>(ownerType, ownerId, std::move(entries)));
	return PersistAction::kUpdated;
}

PersistAction MetadataMutationPersistenceFDB::onAclChanged(inode_t inode) {
	if (writer_ == nullptr) {
		dirtyAcls_.insert(inode);
		return PersistAction::kDeferred;
	}

	// Snapshot the inode's current ACL now (the signal fires synchronously, after the aclStorage
	// mutation). If the inode has no ACL, persist its removal instead.
	const RichACL *acl = gMetadata->aclStorage.get(inode);
	if (acl == nullptr) {
		writer_->enqueue(std::make_unique<AclRemoveEvent>(inode));
		return PersistAction::kRemoved;
	}

	std::vector<uint8_t> buffer;
	serialize(buffer, *acl);
	writer_->enqueue(std::make_unique<AclUpdateEvent>(inode, std::move(buffer)));
	return PersistAction::kUpdated;
}

void MetadataMutationPersistenceFDB::onFreeInodeChanged(inode_t inode, uint32_t timestamp) {
	if (writer_ != nullptr) {
		writer_->enqueue(std::make_unique<FreeNodeUpdateEvent>(inode, timestamp));
	} else {
		dirtyFreeInodes_.insert(inode);
	}
}

void MetadataMutationPersistenceFDB::onFreeInodeRemoved(inode_t inode) {
	if (writer_ != nullptr) {
		writer_->enqueue(std::make_unique<FreeNodeUpdateEvent>(inode));
	} else {
		dirtyFreeInodes_.insert(inode);
	}
}

void MetadataMutationPersistenceFDB::onChunkChanged(uint64_t chunkId, uint32_t version,
                                                    uint32_t lockedTo, uint32_t lockId) {
	if (writer_ != nullptr) {
		writer_->enqueue(std::make_unique<ChunkUpdateEvent>(chunkId, version, lockedTo, lockId));
	} else {
		dirtyChunks_.insert(chunkId);
	}
}

void MetadataMutationPersistenceFDB::onChunkRemoved(uint64_t chunkId) {
	if (writer_ != nullptr) {
		writer_->enqueue(std::make_unique<ChunkRemoveEvent>(chunkId));
	} else {
		dirtyChunks_.insert(chunkId);
	}
}

void MetadataMutationPersistenceFDB::reconcilePromotion() {
	if (gMetadata == nullptr || writer_ == nullptr) {
		clearDirtyTracking();
		return;
	}

	uint64_t persisted = 0;
	uint64_t removed = 0;

	reconcileDirtyNodes(persisted, removed);
	reconcileDirtyEdges(persisted, removed);
	reconcileDirtyXAttrs(persisted, removed);
	reconcileDirtyQuotas(persisted, removed);
	reconcileDirtyAcls(persisted, removed);
	reconcileDirtyFreeInodes(persisted, removed);
	reconcileDirtyChunks(persisted, removed);

	safs::log_info("Promotion reconcile: persisted {} and removed {} dirty entries", persisted,
	               removed);
	clearDirtyTracking();
}

// Nodes: re-persist survivors, remove deletions.
void MetadataMutationPersistenceFDB::reconcileDirtyNodes(uint64_t &persisted, uint64_t &removed) {
	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadOnly);
	auto *nodeOps = gFSOperations->nodeOperations();

	for (const inode_t inode : dirtyNodes_) {
		FSNode *node = nodeOps->idToNode(fsOpContext, inode);
		if (node != nullptr) {
			onNodeChanged(node);
			++persisted;
		} else {
			onNodeRemoved(inode);
			++removed;
		}
	}
}

// Edges: resolve (parent, name) against the parent directory.
void MetadataMutationPersistenceFDB::reconcileDirtyEdges(uint64_t &persisted, uint64_t &removed) {
	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadOnly);
	auto *nodeOps = gFSOperations->nodeOperations();

	for (const auto &[parentId, name] : dirtyEdges_) {
		FSNode *parent = nodeOps->idToNode(fsOpContext, parentId);
		FSNode *child = nullptr;
		if (parent != nullptr && parent->type == FSNodeType::kDirectory) {
			auto *directory = static_cast<FSNodeDirectory *>(parent);
			auto it = directory->find(name);
			if (it != directory->end()) { child = it->second; }
		}
		if (child != nullptr) {
			onEdgeChanged(parentId, child->id, name);
			++persisted;
		} else {
			onEdgeRemoved(parentId, name);
			++removed;
		}
	}
}

// XAttrs: resolve (inode, name) against the in-memory attribute set, then whole-inode removals.
void MetadataMutationPersistenceFDB::reconcileDirtyXAttrs(uint64_t &persisted, uint64_t &removed) {
	for (const auto &[inode, name] : dirtyXattrs_) {
		const std::vector<uint8_t> *value = nullptr;
		for (const auto &inodeEntry : gMetadata->xattrInodeHash[get_xattr_inode_hash(inode)]) {
			if (inodeEntry->inode != inode) { continue; }
			for (const XAttributeDataEntry *dataEntry : inodeEntry->xattrDataEntries) {
				if (dataEntry->attributeName.size() == name.size() &&
				    std::equal(dataEntry->attributeName.begin(), dataEntry->attributeName.end(),
				               name.begin())) {
					value = &dataEntry->attributeValue;
					break;
				}
			}
			if (value != nullptr) { break; }
		}
		if (value != nullptr) {
			onXAttrChanged(inode, name, *value);
			++persisted;
		} else {
			onXAttrRemoved(inode, name);
			++removed;
		}
	}

	// Whole-inode xattr removals (e.g. node deletions).
	for (const inode_t inode : dirtyXattrInodes_) {
		onXAttrInodeRemoved(inode);
		++removed;
	}
}

// Quotas: the handler re-reads current state and enqueues an update or a remove.
void MetadataMutationPersistenceFDB::reconcileDirtyQuotas(uint64_t &persisted, uint64_t &removed) {
	for (const auto &[ownerType, ownerId] : dirtyQuotaOwners_) {
		const auto action = onQuotaChanged(ownerType, ownerId);
		if (action == PersistAction::kUpdated) {
			++persisted;
		} else if (action == PersistAction::kRemoved) {
			++removed;
		}
	}
}

// ACLs: the handler re-reads current state and enqueues an update or a remove.
void MetadataMutationPersistenceFDB::reconcileDirtyAcls(uint64_t &persisted, uint64_t &removed) {
	for (const inode_t inode : dirtyAcls_) {
		const auto action = onAclChanged(inode);
		if (action == PersistAction::kUpdated) {
			++persisted;
		} else if (action == PersistAction::kRemoved) {
			++removed;
		}
	}
}

// Free inodes: re-add still-detained ones (with their timestamp), remove released ones.
void MetadataMutationPersistenceFDB::reconcileDirtyFreeInodes(uint64_t &persisted,
                                                              uint64_t &removed) {
	std::unordered_map<inode_t, uint32_t> detained;
	for (const auto &freeEntry : gMetadata->inodePool) {
		detained.emplace(freeEntry.id, freeEntry.ts);
	}
	for (const inode_t inode : dirtyFreeInodes_) {
		auto it = detained.find(inode);
		if (it != detained.end()) {
			onFreeInodeChanged(inode, it->second);
			++persisted;
		} else {
			onFreeInodeRemoved(inode);
			++removed;
		}
	}
}

// Chunks: re-emit changes for survivors (the writer captures gChunkChangedSignal), remove gone.
void MetadataMutationPersistenceFDB::reconcileDirtyChunks(uint64_t &persisted, uint64_t &removed) {
	for (const uint64_t chunkId : dirtyChunks_) {
		if (chunk_exists(chunkId)) {
			chunk_emit_changed(chunkId);
			++persisted;
		} else {
			onChunkRemoved(chunkId);
			++removed;
		}
	}
}

void MetadataMutationPersistenceFDB::clearDirtyTracking() {
	dirtyNodes_.clear();
	dirtyEdges_.clear();
	dirtyXattrs_.clear();
	dirtyXattrInodes_.clear();
	dirtyQuotaOwners_.clear();
	dirtyAcls_.clear();
	dirtyFreeInodes_.clear();
	dirtyChunks_.clear();
}
