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

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "common/serialization.h"
#include "kv/ifuture.h"
#include "kv/ikv_engine.h"
#include "kv/itransaction.h"
#include "kv/kv_utils.h"
#include "master/kv_common_keys.h"
#include "master/metadata_checkpoint_helpers.h"
#include "master/metadata_checkpoint_manager.h"

namespace {

struct CheckpointSnapshot {
	inode_t maxInodeId;
	uint64_t metadataVersion;
	uint32_t nextSessionId;
	uint64_t nextChunkId;
	std::vector<uint64_t> retainedVersions;
};

class SnapshotReadTransaction final : public kv::IReadOnlyTransaction {
public:
	explicit SnapshotReadTransaction(CheckpointSnapshot snapshot)
	    : snapshot_(std::move(snapshot)) {}

	std::optional<kv::Value> get(const kv::Key &key) override {
		if (key == kv::toBytes(kMetaMaxInodeIdKey)) { return serializeValue(snapshot_.maxInodeId); }
		if (key == kv::toBytes(kMetaVersionKey)) {
			return serializeValue(snapshot_.metadataVersion);
		}
		if (key == kv::toBytes(kMetaNextSessionKey)) {
			return serializeValue(snapshot_.nextSessionId);
		}
		if (key == kv::toBytes(kMetaNextChunkIdKey)) {
			return serializeValue(snapshot_.nextChunkId);
		}
		if (key == kv::toBytes(kMetaCheckpointVersionsKey)) {
			return checkpoints::serializeCheckpointVersions(snapshot_.retainedVersions);
		}
		return std::nullopt;
	}

	std::optional<kv::Value> getSnapshot(const kv::Key &key) override { return get(key); }
	std::unique_ptr<kv::IFuture> getAsync(const kv::Key & /*key*/) override { return nullptr; }
	std::unique_ptr<kv::IFuture> getSnapshotAsync(const kv::Key & /*key*/) override {
		return nullptr;
	}

	kv::GetRangeResult getRange(const kv::KeySelector & /*start*/, const kv::KeySelector & /*end*/,
	                            int /*limit*/) override {
		return {{}, false};
	}
	std::unique_ptr<kv::IRangeFuture> getRangeAsync(const kv::KeySelector & /*start*/,
	                                                const kv::KeySelector & /*end*/,
	                                                int /*limit*/) override {
		return nullptr;
	}

private:
	template <typename T>
	static kv::Value serializeValue(T value) {
		kv::Value encoded;
		serialize(encoded, value);
		return encoded;
	}

	CheckpointSnapshot snapshot_;
};

/// Hands each newly created transaction the next configured immutable snapshot.
///
/// Reads through one transaction remain on one snapshot, as they do in FDB. An accidental second
/// transaction advances to a different snapshot, making split-snapshot loading deterministic
/// instead of depending on the timing of a concurrent checkpoint seal.
class AdvancingSnapshotKVEngine final : public kv::IKVEngine {
public:
	explicit AdvancingSnapshotKVEngine(std::vector<CheckpointSnapshot> snapshots)
	    : snapshots_(std::move(snapshots)) {}

	std::unique_ptr<kv::IReadOnlyTransaction> createReadOnlyTransaction() override {
		if (nextSnapshot_ >= snapshots_.size()) {
			throw std::logic_error("No checkpoint snapshot configured for transaction");
		}
		return std::make_unique<SnapshotReadTransaction>(snapshots_[nextSnapshot_++]);
	}

	std::unique_ptr<kv::IReadWriteTransaction> createReadWriteTransaction() override {
		throw std::logic_error("Unexpected read-write transaction");
	}

	size_t readTransactionCount() const { return nextSnapshot_; }

private:
	std::vector<CheckpointSnapshot> snapshots_;
	size_t nextSnapshot_{0};
};

TEST(MetadataCheckpointManager, LoadsDescriptorAndCatalogFromOneSnapshot) {
	// The first snapshot represents load starting before a concurrent seal. The second represents
	// the state after that seal and is a tripwire: loadLatestCheckpoint() must not request it while
	// loading the descriptor and retained catalog.
	AdvancingSnapshotKVEngine engine({
	    {.maxInodeId = 101,
	     .metadataVersion = 11,
	     .nextSessionId = 21,
	     .nextChunkId = 31,
	     .retainedVersions = {7, 11}},
	    {.maxInodeId = 202,
	     .metadataVersion = 12,
	     .nextSessionId = 22,
	     .nextChunkId = 32,
	     .retainedVersions = {8, 12}},
	});
	MetadataCheckpointManager manager(&engine);

	const auto descriptor = manager.loadLatestCheckpoint();

	EXPECT_EQ(descriptor.maxInodeId, inode_t{101});
	EXPECT_EQ(descriptor.metadataVersion, uint64_t{11});
	EXPECT_EQ(descriptor.nextSessionId, uint32_t{21});
	EXPECT_EQ(descriptor.nextChunkId, uint64_t{31});
	// The active version comes from the retained catalog, so matching version 11 proves that the
	// catalog came from the same snapshot as the descriptor.
	EXPECT_EQ(manager.activeCheckpointVersion(), uint64_t{11});
	// A second transaction would consume the post-seal tripwire snapshot and recreate D2.
	EXPECT_EQ(engine.readTransactionCount(), size_t{1});
}

}  // namespace
