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

#pragma once

#include "common/platform.h"

#include <cstdint>
#include <set>
#include <span>
#include <utility>
#include <vector>

#include "common/type_defs.h"
#include "master/hstring.h"
#include "protocol/quota.h"

class FSNode;
class MetadataWriterFDB;

/// Describes how a metadata-mutation handler dealt with a persistence request.
/// Promotion reconciliation uses the result to report updates and removals accurately.
enum class PersistAction : uint8_t {
	kDeferred,
	kUpdated,
	kRemoved
};

/// Routes metadata mutations to FDB on a master and tracks them for promotion on a shadow.
///
/// The writer is deliberately detached while shadowing, so mutation handlers record only the
/// logical keys touched by changelog replay. Once a writer is attached during promotion, the
/// recorded delta can be resolved against the authoritative in-memory metadata and persisted.
class MetadataMutationPersistenceFDB {
public:
	MetadataMutationPersistenceFDB() = default;
	~MetadataMutationPersistenceFDB() = default;

	MetadataMutationPersistenceFDB(const MetadataMutationPersistenceFDB &) = delete;
	MetadataMutationPersistenceFDB &operator=(const MetadataMutationPersistenceFDB &) = delete;
	MetadataMutationPersistenceFDB(MetadataMutationPersistenceFDB &&) = delete;
	MetadataMutationPersistenceFDB &operator=(MetadataMutationPersistenceFDB &&) = delete;

	/// Attaches the non-owning writer used for all subsequent mutation persistence.
	void attachWriter(MetadataWriterFDB &writer);

	/// Resolves and persists the dirty shadow delta after promotion.
	void reconcilePromotion();

	/// Clears changes made since the last FDB load or promotion reconcile.
	void clearDirtyTracking();

	/// Enqueue a node update event to the metadata writer.
	///
	/// Called when a filesystem node is created or modified. The event serializes the node and
	/// writes `NODE_<nodeId>: <serializedNode>` to the KV store on the next flush.
	///
	/// @param node Pointer to the modified filesystem node.
	void onNodeChanged(FSNode *node);

	/// Enqueue a node removal event to the metadata writer.
	///
	/// Called when a filesystem node is removed. The event removes the `NODE_<nodeId>` key
	/// from the KV store on the next flush.
	///
	/// @param nodeId Inode of the removed filesystem node.
	void onNodeRemoved(inode_t nodeId);

	/// Enqueue an edge update event to the metadata writer.
	///
	/// Called when a directory entry is created or renamed. The event writes
	/// `EDGE_<parentId><name>: <childId>` to the KV store on the next flush.
	///
	/// @param parentId Inode of the parent directory.
	/// @param childId  Inode of the child node.
	/// @param name     Edge name (filename component).
	void onEdgeChanged(inode_t parentId, inode_t childId, const HString &name);

	/// Enqueue an edge removal event to the metadata writer.
	///
	/// Called when a directory entry is unlinked. The event removes the `EDGE_<parentId><name>` key
	/// from the KV store on the next flush.
	///
	/// @param parentId Inode of the parent directory.
	/// @param name     Edge name (filename component) to remove.
	void onEdgeRemoved(inode_t parentId, const HString &name);

	/// Enqueue an xattr inode removal event to the metadata writer.
	///
	/// Called when all xattrs of an inode are removed. The event removes all `XATR_<inode><name>`
	/// keys for the given inode from the KV store on the next flush.
	/// @param inode Inode of the xattr entries to remove.
	void onXAttrInodeRemoved(inode_t inode);

	/// Enqueue an xattr creation or update event to the metadata writer.
	///
	/// Called when an xattr is created or its value is modified. The event writes
	/// `XATR_<inode><name>: <value>` to the KV store on the next flush.
	///
	/// @param inode Inode that owns the xattr.
	/// @param name  Attribute name bytes.
	/// @param value Attribute value bytes.
	void onXAttrChanged(inode_t inode, std::span<const uint8_t> name,
	                    std::span<const uint8_t> value);

	/// Enqueue a single xattr removal event to the metadata writer.
	///
	/// Called when one xattr entry is removed via XATTR_SMODE_REMOVE. The event removes
	/// `XATR_<inode><name>` from the KV store on the next flush.
	///
	/// @param inode Inode that owns the xattr.
	/// @param name  Attribute name bytes.
	void onXAttrRemoved(inode_t inode, std::span<const uint8_t> name);

	/// Enqueue a quota update or removal event to the metadata writer.
	///
	/// Called when an owner's quota limits change. Reads the owner's current limits from
	/// gMetadata->quotaDatabase: if the owner still has limits, enqueues a QuotaUpdateEvent that
	/// rewrites its soft/hard rows; otherwise enqueues a QuotaRemoveEvent that drops them.
	///
	/// @param ownerType Quota owner type (user, group, inode/directory).
	/// @param ownerId   Quota owner id.
	/// @return Whether persistence was deferred or an update/removal was enqueued.
	PersistAction onQuotaChanged(QuotaOwnerType ownerType, inode_t ownerId);

	/// Enqueue an ACL update or removal event to the metadata writer.
	///
	/// Called when an inode's ACL changes. Reads the inode's current ACL from
	/// gMetadata->aclStorage: if present, serializes it and enqueues an AclUpdateEvent; otherwise
	/// enqueues an AclRemoveEvent.
	///
	/// @param inode Inode whose ACL changed.
	/// @return Whether persistence was deferred or an update/removal was enqueued.
	PersistAction onAclChanged(inode_t inode);

	/// Persists a detained inode and its timestamp, or tracks it while shadowing.
	void onFreeInodeChanged(inode_t inode, uint32_t timestamp);

	/// Persists the release of a detained inode, or tracks it while shadowing.
	void onFreeInodeRemoved(inode_t inode);

	/// Enqueue a chunk update event to the metadata writer. On a shadow (no writer) the chunk id is
	/// recorded in the dirty set instead, to be reconciled on promotion.
	///
	/// @param chunkId  Chunk whose metadata changed.
	/// @param version  Chunk version.
	/// @param lockedTo Lock expiry timestamp.
	/// @param lockId   Lock id.
	void onChunkChanged(uint64_t chunkId, uint32_t version, uint32_t lockedTo, uint32_t lockId);

	/// Enqueue a chunk removal event to the metadata writer. On a shadow (no writer) the chunk id
	/// is recorded in the dirty set instead, to be reconciled on promotion.
	///
	/// @param chunkId Chunk whose metadata was removed.
	void onChunkRemoved(uint64_t chunkId);

private:
	void reconcileDirtyNodes(uint64_t &persisted, uint64_t &removed);
	void reconcileDirtyEdges(uint64_t &persisted, uint64_t &removed);
	void reconcileDirtyXAttrs(uint64_t &persisted, uint64_t &removed);
	void reconcileDirtyQuotas(uint64_t &persisted, uint64_t &removed);
	void reconcileDirtyAcls(uint64_t &persisted, uint64_t &removed);
	void reconcileDirtyFreeInodes(uint64_t &persisted, uint64_t &removed);
	void reconcileDirtyChunks(uint64_t &persisted, uint64_t &removed);

	MetadataWriterFDB *writer_{nullptr};

	/// Keys touched by changelog replay while running without a writer. Reset after an FDB load
	/// and drained after promotion reconciliation.
	///
	/// TODO: Bound these sets for long-running shadows. A shadow must never flush them directly to
	/// FDB because that races the live master. The follow-up should combine persisted-version
	/// pruning with a per-section cap and complete-section reconstruction after overflow.
	std::set<inode_t> dirtyNodes_;
	std::set<std::pair<inode_t, HString>> dirtyEdges_;
	std::set<std::pair<inode_t, std::vector<uint8_t>>> dirtyXattrs_;
	std::set<inode_t> dirtyXattrInodes_;
	std::set<std::pair<QuotaOwnerType, inode_t>> dirtyQuotaOwners_;
	std::set<inode_t> dirtyAcls_;
	std::set<inode_t> dirtyFreeInodes_;
	std::set<uint64_t> dirtyChunks_;
};
