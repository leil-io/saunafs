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
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "kv/ikv_engine.h"
#include "master/filesystem_operation_context.h"
#include "master/kv_connector_interface.h"
#include "master/metadata_backend_interface.h"
#include "master/metadata_checkpoint_manager.h"
#include "master/metadata_section_bootstrap_fdb.h"

// Forward declarations to avoid heavy includes in the header
struct ChangelogEvent;
class FSNode;
class MetadataMutationPersistenceFDB;
class MetadataWriterFDB;
namespace hstorage { class Handle; }

/// Simplified Metadata Section structure for FoundationDB
struct MetadataSectionFDB {
	std::string name;         ///< Name of the section
	std::string_view prefix;  ///< Prefix for the section keys

	std::function<int8_t(bool)> loadFunction;  ///< Function to load the section
};

class MetadataBackendForkless : public IMetadataBackend {
public:
	MetadataBackendForkless();
	~MetadataBackendForkless() override;

	// This instance registers itself in the gForklessBackend global; copying or
	// moving it would break that identity, so disable both.
	MetadataBackendForkless(const MetadataBackendForkless &) = delete;
	MetadataBackendForkless &operator=(const MetadataBackendForkless &) = delete;
	MetadataBackendForkless(MetadataBackendForkless &&) = delete;
	MetadataBackendForkless &operator=(MetadataBackendForkless &&) = delete;

	/// Initializes the metadata backend.
	/// This method should be called before any other methods of the backend.
	void init() override;

	/// Returns version of the metadata.
	/// @param file -- path to the metadata binary file (Ignored in FDB).
	uint64_t getVersion(const std::string &file) override;

	/// Returns the current metadata header signature
	std::string getHeaderSignature() override;

	std::string backendType() override { return "MetadataBackendForkless"; }

	/// Forkless keeps metadata in FDB, not in a downloadable metadata.sfs on the master.
	bool supportsMetadataFileDownload() override { return false; }

#ifndef METALOGGER
	/// Store metadata to the given file descriptor.
	void store_fd(FILE *fd) override;

	/// Load complete metadata.
	void loadall(int ignoreflag) override;
#endif  // #ifndef METALOGGER

#if !defined(METARESTORE) && !defined(METALOGGER)
	/// Broadcasts information about status of the freshly finished
	/// metadata save process to interested modules.
	void broadcast_metadata_saved(uint8_t status) override;

	/// Commits the metadata dump by rotating the metadata files and renaming
	/// the temporary file.
	///
	/// This function attempts to rotate the metadata files and rename the
	/// temporary metadata file to the main metadata file. If the renaming
	/// fails, it tries to create an emergency metadata file with a unique name
	/// based on the current time.
	/// @return true if the metadata dump was successfully committed.
	bool commit_metadata_dump() override;

	/// Save metadata to an emergency location (most likely an error occurred
	/// during the metadata dump).
	int emergency_saves() override;

	/// Performs the actual metadata dump to persistent location.
	/// @param dumpType -- type of the dump (foreground, background, etc.).
	/// @return false in case of error.
	uint8_t fs_storeall(DumpType dumpType) override;

	IMetadataDumper *dumper() override { return dumper_.get(); }
#endif  // #if !defined(METARESTORE) && !defined(METALOGGER)

	/// Returns a pointer to the underlying key-value engine.
	kv::IKVEngine *getKVEngine() { return kvConnector_->getKVEngine(); }

	/// Flush pending batched updates.
	///
	/// Drains all pending metadata updates and waits for the background writer to become idle.
	///
	/// Used as a checkpoint barrier before persisting restore-relevant keys in fs_storeall().
	///
	/// @return true on success; false if the writer is unavailable or a KV commit failed.
	bool flushPendingUpdates();

	/// Called when this server is promoted from Shadow to Master.
	///
	/// Creates the asynchronous metadata writer so that FDB persistence begins from this point
	/// forward, attaches it to the mutation-persistence component, and reconciles changes replayed
	/// while this server was a shadow.
	void onPromotedToMaster();

private:
	/// Periodically prunes shadow mutations already covered by the master's durable checkpoint.
	static void pruneDirtyTrackingTimer();

	/// Connects the process-global signals (gChunkChangedSignal, gXAttr*,
	/// initializeNewMetadataHeaderSignal) exactly once per process.
	///
	/// These signals are never recreated and Signal has no per-slot disconnect, so connecting
	/// them on every backend init would stack duplicate slots and enqueue each mutation once per
	/// stale slot after a recreation. The slots route through gForklessBackend (not `this`), so a
	/// single permanent connection always targets the current live backend. Static because it
	/// needs no instance state.
	static void connectGlobalSignalsOnce();

	/// Initializes the vector of metadata sections for later loading
	void initSections();

	///  Initializes new metadata in FDB
	void initializeNewMetadataHeader();
	void initializeEmptyMetadataHeader();

	///  Registers observers/watchers on selected metadata properties.
	///  Wires the process-global signals (once) and the per-load gMetadata signals for the
	///  currently live gMetadata instance.
	void createConnections();

	/// Connects the per-load gMetadata signals (node/edge changed/removed) for the current
	/// gMetadata instance. gMetadata is destroyed and recreated on every shadow (re)load
	/// (fs_strinit), and Signal has no per-slot disconnect, so these must be reconnected once per
	/// fresh instance -- on every loadall -- not once at init. Without this, a shadow that is later
	/// promoted has a live writer but no signal->writer wiring, so its mutations never reach FDB.
	void connectPerLoadSignals();

	// FS Load from FDB

	/// Loads all sections
	int fsLoad(bool ignoreFlag);

	/// Captures the restore-relevant state that defines one checkpoint boundary.
	MetadataCheckpointDescriptor buildCheckpointDescriptor() const;

	/// Applies a loaded checkpoint descriptor to the in-memory metadata globals.
	void applyCheckpointDescriptor(const MetadataCheckpointDescriptor &descriptor);

	/// Loads NODE_ metadata
	/// Loads all nodes from the KV store and reconstructs the in-memory filesystem node table.
	///
	/// Nodes are stored as `NODE_<nodeId>: <serializedNode>` entries. This method paginates
	/// through the full NODE_ keyspace starting from SPECIAL_INODE_ROOT, deserializes each
	/// node, and calls loadNode() to register it in gMetadata.
	///
	/// @param ignoreFlag Currently unused; reserved for consistency with other load functions.
	/// @return kOpSuccess on success, kOpFailure on error.
	int8_t loadNodes(bool ignoreFlag);

	/// Register a single deserialized node in the in-memory metadata structures.
	///
	/// Updates per-type counters (dirNodes, fileNodes, linkNodes), registers open sessions
	/// for file-like nodes, applies quota accounting, and inserts the node into the global
	/// node hash table and inode pool.
	///
	/// @param fsOpContext Filesystem operation context (transaction).
	/// @param node        Deserialized FSNode to register. Ownership is transferred to gMetadata.
	/// @return kOpSuccess on success, kOpFailure if the node type is unrecognized.
	int8_t loadNode(const FilesystemOperationContext &fsOpContext, FSNode *node);

	/// Loads FREE_ metadata
	/// Loads all free (detained) inodes from the KV store into the in-memory inode pool.
	///
	/// Free inodes are stored as `FREE_<nodeId>: <timestamp>` entries. This method paginates
	/// through the full FREE_ keyspace, calls `gMetadata->inodePool.detain()` for each entry,
	/// and then connects signal handlers so that future detain/release operations are
	/// automatically propagated to the KV store via FreeNodeUpdateEvent.
	///
	/// @param ignoreFlag Currently unused; reserved for consistency with other load functions.
	/// @return kOpSuccess on success, kOpFailure on error.
	int8_t loadFree(bool ignoreFlag);

	/// Loads XATR_ metadata
	/// Loads all extended attributes from the KV store and reconstructs the in-memory xattr
	/// hash tables (xattrDataHash and xattrInodeHash).
	///
	/// Xattrs are stored as `XATR_<InodeId><AttributeName>: <AttributeValue>` entries. This
	/// method paginates through the full XATR_ keyspace, validates name and value lengths
	/// against protocol limits (SFS_XATTR_NAME_MAX, SFS_XATTR_SIZE_MAX, SFS_XATTR_LIST_MAX),
	/// and inserts each entry into gMetadata.
	///
	/// @param ignoreFlag When true, entries with oversized names or values are skipped.
	/// @return kOpSuccess on success, kOpFailure or SAUNAFS_ERROR_ERANGE on error.
	int8_t loadXAttr(bool ignoreFlag);

	/// Loads QUOT_ metadata
	/// Loads all quota limits from the KV store into gMetadata->quotaDatabase and recomputes the
	/// quota checksum.
	///
	/// Quota limits are stored as `QUOT_<OwnerType><OwnerId><Rigor><Resource>: <Limit>` entries
	/// (soft/hard only). Usage (kUsed) is not persisted; it is rebuilt from node loading and is
	/// excluded from the quota checksum.
	///
	/// @param ignoreFlag Currently unused; reserved for consistency with other load functions.
	/// @return kOpSuccess on success, kOpFailure on error.
	int8_t loadQuotas(bool ignoreFlag);

	/// Loads ACLS_ metadata
	/// Loads all per-inode ACLs from the KV store into gMetadata->aclStorage.
	///
	/// ACLs are stored as `ACLS_<InodeId>: <serialized RichACL>` entries. ACLs are not part of the
	/// metadata checksum, but they must be persisted so a forkless reload/shadow reconstructs them.
	///
	/// @param ignoreFlag Currently unused; reserved for consistency with other load functions.
	/// @return kOpSuccess on success, kOpFailure on error.
	int8_t loadACLs(bool ignoreFlag);

	/// Loads EDGE_ metadata
	/// Loads all edges from the KV store and reconstructs the in-memory directory tree.
	///
	/// Edges are stored as `EDGE_<ParentId><Name>: <ChildId>` entries. This method paginates
	/// through the full EDGE_ keyspace, deserializes each entry, and calls loadEdge() to
	/// attach the child node to its parent directory (or to the trash/reserved containers
	/// when parentId is 0).
	///
	/// @param ignoreFlag When true, missing parent/child nodes are tolerated and orphan nodes
	///                   are attached to the root directory instead of causing a failure.
	/// @return kOpSuccess on success, kOpFailure on error.
	int8_t loadEdges(bool ignoreFlag);

	/// Load a single edge into the in-memory filesystem tree.
	///
	/// Depending on parentId:
	/// - parentId == 0: the child is inserted into the trash or reserved container based on
	///   its node type.
	/// - parentId != 0: the child is inserted into the parent directory's entry map, its
	///   parent back-pointer is set, and directory statistics are propagated upward.
	///
	/// On the first call, pass `init = true` to reset the internal "current parent"
	/// tracker used to detect out-of-order edges).
	///
	/// @param fsOpContext Filesystem operation context (transaction).
	/// @param parentId   Inode of the parent directory (0 for trash/reserved).
	/// @param childId    Inode of the child node.
	/// @param name       Edge name (filename component).
	/// @param ignoreFlag When true, tolerate missing nodes (see loadEdges()).
	/// @param init       When true, reset state without loading an edge.
	/// @return kOpSuccess on success, kOpFailure on error.
	int8_t loadEdge(const FilesystemOperationContext &fsOpContext, inode_t parentId,
	                inode_t childId, const std::string &name, bool ignoreFlag, bool init = false);

	/// Loads CHNK_ metadata
	/// Loads all chunks from the KV store and reconstructs the in-memory chunk table.
	///
	/// Chunks are stored as `CHNK_<chunkId><version>: <lockedTo><lockId>` entries. This method
	/// first restores the next-chunk-id generator from the KV store, then paginates through the
	/// full CHNK_ keyspace to load each chunk.
	///
	/// @param ignoreFlag Currently unused; reserved for consistency with other load functions.
	/// @return kOpSuccess on success, kOpFailure on error.
	int8_t loadChunks(bool ignoreFlag);

	/// Provides connection to the key-value store (FoundationDB for this implementation)
	std::shared_ptr<IKVConnector> kvConnector_;

	/// Manager for metadata checkpoints
	std::unique_ptr<MetadataCheckpointManager> checkpointManager_;

	/// Metadata sections with their loading functions
	std::vector<MetadataSectionFDB> metadataSections_;

#if !defined(METARESTORE) && !defined(METALOGGER)
	std::unique_ptr<IMetadataDumper> dumper_;
#endif  // #ifndef METARESTORE

	/// Metadata writer for all metadata updates
	std::unique_ptr<MetadataWriterFDB> metadataWriter_;

	/// Routes live mutations to metadataWriter_ and owns the dirty shadow delta. Declared after
	/// metadataWriter_ so its non-owning writer pointer is destroyed first.
	std::unique_ptr<MetadataMutationPersistenceFDB> mutationPersistence_;

	/// Descriptor of the currently loaded checkpoint
	MetadataCheckpointDescriptor loadedCheckpointDescriptor_{};

	inode_t currentLoadParentId_ = 0;

#ifndef METARESTORE
	/// Bootstrapper for metadata sections
	std::unique_ptr<MetadataSectionBootstrapFDB> sectionBootstrapper_ = nullptr;
#endif  // #ifndef METARESTORE

	void *dirtyPruneTimer_{nullptr};
};
