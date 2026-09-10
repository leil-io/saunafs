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
#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/datapack.h"
#include "common/quota_database.h"
#include "common/richacl.h"
#include "common/serialization.h"
#include "kv/itransaction.h"
#include "kv/kv_utils.h"
#include "master/acl_storage.h"
#include "master/chunks.h"
#include "master/exceptions.h"
#include "master/filesystem_metadata.h"
#include "master/filesystem_operations.h"
#include "master/filesystem_operations_interface.h"
#include "master/filesystem_xattr.h"
#include "master/kv_common_keys.h"
#include "master/metadata_mutation_persistence_fdb.h"
#include "master/metadata_writer_fdb.h"
#include "slogger/slogger.h"

namespace {

/// Exact-key removal fallback for malformed rows encountered during an overflow replacement.
/// Valid rows use their typed removal event so checkpoint undo is retained.
class ExactKeyRemoveEvent final : public IMetadataUpdateEvent {
public:
	explicit ExactKeyRemoveEvent(kv::Key key) : key_(std::move(key)) {}

	void applyEvent(const MetadataWriteContext &context) override {
		if (context.transaction == nullptr) {
			safs::log_err("ExactKeyRemoveEvent requires a valid transaction in the context");
			return;
		}
		context.transaction->remove(key_);
	}

private:
	kv::Key key_;
};

template <typename Callback>
uint64_t forEachPersistedSectionKey(kv::IKVEngine *kvEngine, std::string_view prefix,
                                    Callback callback) {
	kv::Key startKey = kv::toBytes(prefix);
	const kv::Key endKey = kv::prefixEnd(startKey);
	kv::KeySelector startSelector(startKey, true, 0);
	const kv::KeySelector endSelector(endKey, true, 0);
	uint64_t count = 0;

	while (true) {
		auto transaction = kvEngine->createReadOnlyTransaction();
		auto page = transaction->getRange(startSelector, endSelector, kv::kDefaultGetRangeLimit);
		for (const auto &pair : page.getPairs()) {
			callback(pair.key);
			++count;
		}
		if (!page.hasMore() || page.getPairs().empty()) { break; }
		startSelector = kv::KeySelector(page.getPairs().back().key, false, 0);
	}

	return count;
}

}  // namespace

void MetadataMutationPersistenceFDB::setDirtyEntryLimit(size_t limit) {
	dirtyEntryLimit_ = std::max<size_t>(1, limit);
}

void MetadataMutationPersistenceFDB::attachWriter(MetadataWriterFDB &writer,
                                                  kv::IKVEngine &kvEngine) {
	writer_ = &writer;
	kvEngine_ = &kvEngine;
}

template <typename Map, typename Key>
void MetadataMutationPersistenceFDB::recordDirtyEntry(Map &entries, Key &&key,
                                                      DirtySectionState &state,
                                                      std::string_view section) {
	const uint64_t version = gMetadata != nullptr ? gMetadata->metadataVersion : 0;
	state.newestVersion = std::max(state.newestVersion, version);
	if (state.overflowed) { return; }

	entries.insert_or_assign(std::forward<Key>(key), version);
	if (entries.size() <= dirtyEntryLimit_) { return; }

	entries.clear();
	state.overflowed = true;
	safs::log_warn(
	    "Forkless shadow dirty {} tracking exceeded {} entries; promotion will replace the section",
	    section, dirtyEntryLimit_);
}

void MetadataMutationPersistenceFDB::recordDirtyXAttr(inode_t inode, std::vector<uint8_t> name) {
	const uint64_t version = gMetadata != nullptr ? gMetadata->metadataVersion : 0;
	dirtyXattrsState_.newestVersion = std::max(dirtyXattrsState_.newestVersion, version);
	if (dirtyXattrsState_.overflowed) { return; }

	dirtyXattrs_.insert_or_assign(std::make_pair(inode, std::move(name)), version);
	if (dirtyXattrs_.size() + dirtyXattrInodes_.size() <= dirtyEntryLimit_) { return; }

	dirtyXattrs_.clear();
	dirtyXattrInodes_.clear();
	dirtyXattrsState_.overflowed = true;
	safs::log_warn(
	    "Forkless shadow dirty XATR tracking exceeded {} entries; promotion will replace the section",
	    dirtyEntryLimit_);
}

void MetadataMutationPersistenceFDB::recordDirtyXAttrInode(inode_t inode) {
	const uint64_t version = gMetadata != nullptr ? gMetadata->metadataVersion : 0;
	dirtyXattrsState_.newestVersion = std::max(dirtyXattrsState_.newestVersion, version);
	if (dirtyXattrsState_.overflowed) { return; }

	dirtyXattrInodes_.insert_or_assign(inode, version);
	if (dirtyXattrs_.size() + dirtyXattrInodes_.size() <= dirtyEntryLimit_) { return; }

	dirtyXattrs_.clear();
	dirtyXattrInodes_.clear();
	dirtyXattrsState_.overflowed = true;
	safs::log_warn(
	    "Forkless shadow dirty XATR tracking exceeded {} entries; promotion will replace the section",
	    dirtyEntryLimit_);
}

void MetadataMutationPersistenceFDB::onNodeChanged(FSNode *node) {
	if (node == nullptr) {
		safs::log_err("{}: received null node, skipping metadata update", __func__);
		return;
	}
	if (writer_ != nullptr) {
		writer_->enqueue(std::make_unique<NodeUpdateEvent>(node));
	} else {
		recordDirtyEntry(dirtyNodes_, node->id, dirtyNodesState_, "NODE");
	}
}

void MetadataMutationPersistenceFDB::onNodeRemoved(inode_t nodeId) {
	if (writer_ != nullptr) {
		writer_->enqueue(std::make_unique<NodeRemoveEvent>(nodeId));
	} else {
		recordDirtyEntry(dirtyNodes_, nodeId, dirtyNodesState_, "NODE");
	}
}

void MetadataMutationPersistenceFDB::onEdgeChanged(inode_t parentId, inode_t childId,
                                                   const HString &name) {
	if (writer_ != nullptr) {
		writer_->enqueue(std::make_unique<EdgeUpdateEvent>(parentId, name, childId));
	} else {
		recordDirtyEntry(dirtyEdges_, std::make_pair(parentId, name), dirtyEdgesState_, "EDGE");
	}
}

void MetadataMutationPersistenceFDB::onEdgeRemoved(inode_t parentId, const HString &name) {
	if (writer_ != nullptr) {
		writer_->enqueue(std::make_unique<EdgeRemoveEvent>(parentId, name));
	} else {
		recordDirtyEntry(dirtyEdges_, std::make_pair(parentId, name), dirtyEdgesState_, "EDGE");
	}
}

void MetadataMutationPersistenceFDB::onXAttrInodeRemoved(inode_t inode) {
	if (writer_ != nullptr) {
		writer_->enqueue(std::make_unique<XAttrInodeRemoveEvent>(inode));
	} else {
		recordDirtyXAttrInode(inode);
	}
}

void MetadataMutationPersistenceFDB::onXAttrChanged(inode_t inode, std::span<const uint8_t> name,
                                                    std::span<const uint8_t> value) {
	if (writer_ != nullptr) {
		writer_->enqueue(std::make_unique<XAttrUpdateEvent>(inode, name, value));
	} else {
		recordDirtyXAttr(inode, {name.begin(), name.end()});
	}
}

void MetadataMutationPersistenceFDB::onXAttrRemoved(inode_t inode, std::span<const uint8_t> name) {
	if (writer_ != nullptr) {
		writer_->enqueue(std::make_unique<XAttrRemoveEvent>(inode, name));
	} else {
		recordDirtyXAttr(inode, {name.begin(), name.end()});
	}
}

PersistAction MetadataMutationPersistenceFDB::onQuotaChanged(QuotaOwnerType ownerType,
                                                             inode_t ownerId) {
	if (writer_ == nullptr) {
		recordDirtyEntry(dirtyQuotaOwners_, std::make_pair(ownerType, ownerId), dirtyQuotasState_,
		                 "QUOT");
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
		recordDirtyEntry(dirtyAcls_, inode, dirtyAclsState_, "ACLS");
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
		recordDirtyEntry(dirtyFreeInodes_, inode, dirtyFreeInodesState_, "FREE");
	}
}

void MetadataMutationPersistenceFDB::onFreeInodeRemoved(inode_t inode) {
	if (writer_ != nullptr) {
		writer_->enqueue(std::make_unique<FreeNodeUpdateEvent>(inode));
	} else {
		recordDirtyEntry(dirtyFreeInodes_, inode, dirtyFreeInodesState_, "FREE");
	}
}

void MetadataMutationPersistenceFDB::onChunkChanged(uint64_t chunkId, uint32_t version,
                                                    uint32_t lockedTo, uint32_t lockId) {
	if (writer_ != nullptr) {
		writer_->enqueue(std::make_unique<ChunkUpdateEvent>(chunkId, version, lockedTo, lockId));
	} else {
		recordDirtyEntry(dirtyChunks_, chunkId, dirtyChunksState_, "CHNK");
	}
}

void MetadataMutationPersistenceFDB::onChunkRemoved(uint64_t chunkId) {
	if (writer_ != nullptr) {
		writer_->enqueue(std::make_unique<ChunkRemoveEvent>(chunkId));
	} else {
		recordDirtyEntry(dirtyChunks_, chunkId, dirtyChunksState_, "CHNK");
	}
}

void MetadataMutationPersistenceFDB::pruneDirtyTracking(uint64_t persistedVersion) {
	if (persistedVersion == 0) { return; }

	uint64_t pruned = 0;
	uint64_t clearedOverflows = 0;
	auto pruneSection = [&](auto &entries, DirtySectionState &state) {
		if (state.overflowed) {
			// Signals may run immediately before or after replay increments metadataVersion. A
			// strict comparison is safe in both cases; <= could discard the first unsealed mutation
			// when its pre-increment version equals META_VERSION.
			if (state.newestVersion < persistedVersion) {
				state = {};
				++clearedOverflows;
			}
			return;
		}

		pruned += std::erase_if(entries, [persistedVersion](const auto &entry) {
			return entry.second < persistedVersion;
		});
		state.newestVersion = 0;
		for (const auto &[key, version] : entries) {
			(void)key;
			state.newestVersion = std::max(state.newestVersion, version);
		}
	};

	pruneSection(dirtyNodes_, dirtyNodesState_);
	pruneSection(dirtyEdges_, dirtyEdgesState_);

	if (dirtyXattrsState_.overflowed) {
		if (dirtyXattrsState_.newestVersion < persistedVersion) {
			dirtyXattrsState_ = {};
			++clearedOverflows;
		}
	} else {
		pruned += std::erase_if(dirtyXattrs_, [persistedVersion](const auto &entry) {
			return entry.second < persistedVersion;
		});
		pruned += std::erase_if(dirtyXattrInodes_, [persistedVersion](const auto &entry) {
			return entry.second < persistedVersion;
		});
		dirtyXattrsState_.newestVersion = 0;
		for (const auto &[key, version] : dirtyXattrs_) {
			(void)key;
			dirtyXattrsState_.newestVersion = std::max(dirtyXattrsState_.newestVersion, version);
		}
		for (const auto &[key, version] : dirtyXattrInodes_) {
			(void)key;
			dirtyXattrsState_.newestVersion = std::max(dirtyXattrsState_.newestVersion, version);
		}
	}

	pruneSection(dirtyQuotaOwners_, dirtyQuotasState_);
	pruneSection(dirtyAcls_, dirtyAclsState_);
	pruneSection(dirtyFreeInodes_, dirtyFreeInodesState_);
	pruneSection(dirtyChunks_, dirtyChunksState_);

	if (pruned > 0 || clearedOverflows > 0) {
		safs::log_info(
		    "Forkless shadow pruned {} dirty entries and cleared {} overflow markers through "
		    "durable metadata version {}",
		    pruned, clearedOverflows, persistedVersion);
	}
}

void MetadataMutationPersistenceFDB::reconcilePromotion() {
	if (gMetadata == nullptr || writer_ == nullptr) {
		clearDirtyTracking();
		return;
	}
	if (kvEngine_ == nullptr) {
		throw MetadataConsistencyException("Promotion reconciliation has no KV engine");
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

	// An overflow replacement removes every persisted row before rebuilding the current section.
	// Make the complete ordered sequence durable before promotion returns, so a process failure
	// cannot leave a partially reconstructed section behind.
	if (!writer_->flushAndWait()) {
		throw MetadataConsistencyException("Failed to flush promotion dirty reconciliation");
	}

	safs::log_info("Promotion reconcile: persisted {} and removed {} dirty entries", persisted,
	               removed);
	clearDirtyTracking();
}

void MetadataMutationPersistenceFDB::replaceNodes(uint64_t &persisted, uint64_t &removed) {
	safs::log_info("Promotion reconcile: replacing overflowed NODE section");
	removed += forEachPersistedSectionKey(kvEngine_, kNodeKeyPrefix, [this](const kv::Key &key) {
		if (key.size() != kNodeKeyPrefix.size() + sizeof(inode_t)) {
			writer_->enqueue(std::make_unique<ExactKeyRemoveEvent>(key));
			return;
		}
		const uint8_t *data = key.data() + kNodeKeyPrefix.size();
		inode_t inode{};
		getINode(&data, inode);
		onNodeRemoved(inode);
	});

	for (const auto &bucket : gMetadata->nodeHash) {
		for (FSNode *node : bucket) {
			onNodeChanged(node);
			++persisted;
		}
	}
}

void MetadataMutationPersistenceFDB::replaceEdges(uint64_t &persisted, uint64_t &removed) {
	safs::log_info("Promotion reconcile: replacing overflowed EDGE section");
	constexpr size_t kMinKeySize = kEdgeKeyPrefix.size() + sizeof(inode_t) + 1;
	removed += forEachPersistedSectionKey(kvEngine_, kEdgeKeyPrefix, [this](const kv::Key &key) {
		if (key.size() < kMinKeySize) {
			writer_->enqueue(std::make_unique<ExactKeyRemoveEvent>(key));
			return;
		}
		const uint8_t *data = key.data() + kEdgeKeyPrefix.size();
		inode_t parentId{};
		getINode(&data, parentId);
		const std::string name(reinterpret_cast<const char *>(data),
		                       key.data() + key.size() - data);
		onEdgeRemoved(parentId, HString(name));
	});

	for (const auto &bucket : gMetadata->nodeHash) {
		for (FSNode *node : bucket) {
			if (node->type != FSNodeType::kDirectory) { continue; }
			const auto *directory = static_cast<const FSNodeDirectory *>(node);
			for (const auto &entry : directory->entries) {
				onEdgeChanged(node->id, entry.second->id, static_cast<HString>(*entry.first));
				++persisted;
			}
		}
	}

	// Detached trash and reserved files are represented as EDGE rows with parent id zero.
	for (const auto &entry : gMetadata->trash) {
		onEdgeChanged(0, entry.first.id, entry.second.get());
		++persisted;
	}
	for (const auto &entry : gMetadata->reserved) {
		onEdgeChanged(0, entry.first, entry.second.get());
		++persisted;
	}
}

void MetadataMutationPersistenceFDB::replaceXAttrs(uint64_t &persisted, uint64_t &removed) {
	safs::log_info("Promotion reconcile: replacing overflowed XATR section");
	constexpr size_t kMinKeySize = kXAttrKeyPrefix.size() + sizeof(inode_t) + 1;
	removed += forEachPersistedSectionKey(kvEngine_, kXAttrKeyPrefix, [this](const kv::Key &key) {
		if (key.size() < kMinKeySize) {
			writer_->enqueue(std::make_unique<ExactKeyRemoveEvent>(key));
			return;
		}
		const uint8_t *data = key.data() + kXAttrKeyPrefix.size();
		inode_t inode{};
		getINode(&data, inode);
		onXAttrRemoved(inode, {data, key.data() + key.size()});
	});

	for (const auto &bucket : gMetadata->xattrInodeHash) {
		for (const auto &inodeEntry : bucket) {
			for (const XAttributeDataEntry *dataEntry : inodeEntry->xattrDataEntries) {
				onXAttrChanged(inodeEntry->inode, dataEntry->attributeName,
				               dataEntry->attributeValue);
				++persisted;
			}
		}
	}
}

void MetadataMutationPersistenceFDB::replaceQuotas(uint64_t &persisted, uint64_t &removed) {
	safs::log_info("Promotion reconcile: replacing overflowed QUOT section");
	constexpr size_t kKeySize = kQuotasKeyPrefix.size() + sizeof(uint8_t) + sizeof(inode_t) +
	                            sizeof(uint8_t) + sizeof(uint8_t);
	std::set<std::pair<QuotaOwnerType, inode_t>> ownersToRemove;
	removed += forEachPersistedSectionKey(
	    kvEngine_, kQuotasKeyPrefix, [this, &ownersToRemove](const kv::Key &key) {
		    if (key.size() != kKeySize) {
			    writer_->enqueue(std::make_unique<ExactKeyRemoveEvent>(key));
			    return;
		    }
		    const uint8_t *data = key.data() + kQuotasKeyPrefix.size();
		    const auto ownerType = static_cast<QuotaOwnerType>(*data++);
		    if (ownerType > QuotaOwnerType::kInode) {
			    writer_->enqueue(std::make_unique<ExactKeyRemoveEvent>(key));
			    return;
		    }
		    inode_t ownerId{};
		    getINode(&data, ownerId);
		    ownersToRemove.emplace(ownerType, ownerId);
	    });
	for (const auto &[ownerType, ownerId] : ownersToRemove) {
		writer_->enqueue(std::make_unique<QuotaRemoveEvent>(ownerType, ownerId));
	}

	std::set<std::pair<QuotaOwnerType, inode_t>> owners;
	for (const QuotaEntry &entry : gMetadata->quotaDatabase.getEntries()) {
		owners.emplace(entry.entryKey.owner.ownerType, entry.entryKey.owner.ownerId);
	}
	for (const auto &[ownerType, ownerId] : owners) {
		if (onQuotaChanged(ownerType, ownerId) == PersistAction::kUpdated) { ++persisted; }
	}
}

void MetadataMutationPersistenceFDB::replaceAcls(uint64_t &persisted, uint64_t &removed) {
	safs::log_info("Promotion reconcile: replacing overflowed ACLS section");
	removed += forEachPersistedSectionKey(kvEngine_, kACLsKeyPrefix, [this](const kv::Key &key) {
		if (key.size() != kACLsKeyPrefix.size() + sizeof(inode_t)) {
			writer_->enqueue(std::make_unique<ExactKeyRemoveEvent>(key));
			return;
		}
		const uint8_t *data = key.data() + kACLsKeyPrefix.size();
		inode_t inode{};
		getINode(&data, inode);
		writer_->enqueue(std::make_unique<AclRemoveEvent>(inode));
	});

	for (const auto &bucket : gMetadata->nodeHash) {
		for (const FSNode *node : bucket) {
			if (gMetadata->aclStorage.get(node->id) == nullptr) { continue; }
			if (onAclChanged(node->id) == PersistAction::kUpdated) { ++persisted; }
		}
	}
}

void MetadataMutationPersistenceFDB::replaceFreeInodes(uint64_t &persisted, uint64_t &removed) {
	safs::log_info("Promotion reconcile: replacing overflowed FREE section");
	removed += forEachPersistedSectionKey(kvEngine_, kFreeKeyPrefix, [this](const kv::Key &key) {
		if (key.size() != kFreeKeyPrefix.size() + sizeof(inode_t)) {
			writer_->enqueue(std::make_unique<ExactKeyRemoveEvent>(key));
			return;
		}
		const uint8_t *data = key.data() + kFreeKeyPrefix.size();
		inode_t inode{};
		getINode(&data, inode);
		onFreeInodeRemoved(inode);
	});

	for (const auto &freeEntry : gMetadata->inodePool) {
		onFreeInodeChanged(freeEntry.id, freeEntry.ts);
		++persisted;
	}
}

void MetadataMutationPersistenceFDB::replaceChunks(uint64_t &persisted, uint64_t &removed) {
	safs::log_info("Promotion reconcile: replacing overflowed CHNK section");
	removed +=
	    forEachPersistedSectionKey(kvEngine_, kChunkLatestKeyPrefix, [this](const kv::Key &key) {
		    if (key.size() != kChunkLatestKeyPrefix.size() + sizeof(uint64_t)) {
			    writer_->enqueue(std::make_unique<ExactKeyRemoveEvent>(key));
			    return;
		    }
		    const uint8_t *data = key.data() + kChunkLatestKeyPrefix.size();
		    const uint64_t chunkId = get64bit(&data);
		    onChunkRemoved(chunkId);
	    });
	persisted += chunk_emit_all_changed();
}

// Nodes: re-persist survivors, remove deletions.
void MetadataMutationPersistenceFDB::reconcileDirtyNodes(uint64_t &persisted, uint64_t &removed) {
	if (dirtyNodesState_.overflowed) {
		replaceNodes(persisted, removed);
		return;
	}
	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadOnly);
	auto *nodeOps = gFSOperations->nodeOperations();

	for (const auto &[inode, dirtyVersion] : dirtyNodes_) {
		(void)dirtyVersion;
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
	if (dirtyEdgesState_.overflowed) {
		replaceEdges(persisted, removed);
		return;
	}
	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadOnly);
	auto *nodeOps = gFSOperations->nodeOperations();

	for (const auto &[edge, dirtyVersion] : dirtyEdges_) {
		const auto &[parentId, name] = edge;
		(void)dirtyVersion;
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

// XAttrs: resolve range removals and point mutations against the in-memory attribute set.
void MetadataMutationPersistenceFDB::reconcileDirtyXAttrs(uint64_t &persisted, uint64_t &removed) {
	if (dirtyXattrsState_.overflowed) {
		replaceXAttrs(persisted, removed);
		return;
	}

	auto persistCurrentInodeXattrs = [this, &persisted](inode_t inode) {
		for (const auto &inodeEntry : gMetadata->xattrInodeHash[get_xattr_inode_hash(inode)]) {
			if (inodeEntry->inode != inode) { continue; }
			for (const XAttributeDataEntry *dataEntry : inodeEntry->xattrDataEntries) {
				onXAttrChanged(inode, dataEntry->attributeName, dataEntry->attributeValue);
				++persisted;
			}
			return;
		}
	};

	// A whole-inode removal can be followed by inode reuse while a shadow runs for a long time.
	// Clear the persisted range first and then reconstruct the inode's current attributes. Point
	// mutations for the same inode are covered by that reconstruction and must not be replayed a
	// second time before the range deletion.
	for (const auto &[inode, dirtyVersion] : dirtyXattrInodes_) {
		(void)dirtyVersion;
		onXAttrInodeRemoved(inode);
		++removed;
		persistCurrentInodeXattrs(inode);
	}

	for (const auto &[xattr, dirtyVersion] : dirtyXattrs_) {
		const auto &[inode, name] = xattr;
		(void)dirtyVersion;
		if (dirtyXattrInodes_.contains(inode)) { continue; }
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
}

// Quotas: the handler re-reads current state and enqueues an update or a remove.
void MetadataMutationPersistenceFDB::reconcileDirtyQuotas(uint64_t &persisted, uint64_t &removed) {
	if (dirtyQuotasState_.overflowed) {
		replaceQuotas(persisted, removed);
		return;
	}
	for (const auto &[owner, dirtyVersion] : dirtyQuotaOwners_) {
		const auto &[ownerType, ownerId] = owner;
		(void)dirtyVersion;
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
	if (dirtyAclsState_.overflowed) {
		replaceAcls(persisted, removed);
		return;
	}
	for (const auto &[inode, dirtyVersion] : dirtyAcls_) {
		(void)dirtyVersion;
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
	if (dirtyFreeInodesState_.overflowed) {
		replaceFreeInodes(persisted, removed);
		return;
	}
	std::unordered_map<inode_t, uint32_t> detained;
	for (const auto &freeEntry : gMetadata->inodePool) {
		detained.emplace(freeEntry.id, freeEntry.ts);
	}
	for (const auto &[inode, dirtyVersion] : dirtyFreeInodes_) {
		(void)dirtyVersion;
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
	if (dirtyChunksState_.overflowed) {
		replaceChunks(persisted, removed);
		return;
	}
	for (const auto &[chunkId, dirtyVersion] : dirtyChunks_) {
		(void)dirtyVersion;
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
	dirtyNodesState_ = {};
	dirtyEdgesState_ = {};
	dirtyXattrsState_ = {};
	dirtyQuotasState_ = {};
	dirtyAclsState_ = {};
	dirtyFreeInodesState_ = {};
	dirtyChunksState_ = {};
}
