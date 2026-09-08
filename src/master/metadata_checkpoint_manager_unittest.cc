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
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "common/serialization.h"
#include "kv/ifuture.h"
#include "kv/ikv_engine.h"
#include "kv/itransaction.h"
#include "kv/kv_utils.h"
#include "master/exceptions.h"
#include "master/kv_common_keys.h"
#include "master/metadata_backend_interface.h"
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

class SnapshotReadTransaction final : public kv::IReadWriteTransaction {
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

	void set(const kv::Key & /*key*/, const kv::Value & /*value*/) override { ++mutationCount_; }
	void atomicAdd(const kv::Key & /*key*/, const kv::Value & /*delta*/) override {
		++mutationCount_;
	}
	void atomicMax(const kv::Key & /*key*/, const kv::Value & /*value*/) override {
		++mutationCount_;
	}
	void remove(const kv::Key & /*key*/) override { ++mutationCount_; }
	void removeRange(const kv::Key & /*start*/, const kv::Key & /*end*/) override {
		++mutationCount_;
	}
	void addReadConflictKey(const kv::Key & /*key*/) override {}
	bool commit() override { return true; }
	std::unique_ptr<kv::ICommitFuture> commitAsync() override {
		return std::make_unique<kv::ImmediateCommitFuture>(true);
	}
	std::optional<int64_t> getCommittedVersion() const override { return std::nullopt; }
	uint64_t mutationCount() const override { return mutationCount_; }
	std::unique_ptr<kv::IVoidFuture> recoverAsync(int /*backendErrorCode*/) override {
		return std::make_unique<kv::ImmediateVoidFuture>();
	}

private:
	template <typename T>
	static kv::Value serializeValue(T value) {
		kv::Value encoded;
		serialize(encoded, value);
		return encoded;
	}

	CheckpointSnapshot snapshot_;
	uint64_t mutationCount_{0};
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
		if (nextSnapshot_ >= snapshots_.size()) {
			throw std::logic_error("No checkpoint snapshot configured for transaction");
		}
		return std::make_unique<SnapshotReadTransaction>(snapshots_[nextSnapshot_++]);
	}

	size_t transactionCount() const { return nextSnapshot_; }

private:
	std::vector<CheckpointSnapshot> snapshots_;
	size_t nextSnapshot_{0};
};

using DurableStore = std::map<kv::Key, kv::Value>;

struct SharedDurableStore {
	std::mutex mutex;
	DurableStore values;
};

class MemoryTransaction final : public kv::IReadWriteTransaction {
public:
	explicit MemoryTransaction(std::shared_ptr<SharedDurableStore> store)
	    : store_(std::move(store)), snapshot_(store_->values) {
		std::lock_guard<std::mutex> lock(store_->mutex);
	}

	std::optional<kv::Value> get(const kv::Key &key) override {
		if (const auto pending = writes_.find(key); pending != writes_.end()) {
			return pending->second;
		}
		if (const auto value = snapshot_.find(key); value != snapshot_.end()) {
			return value->second;
		}
		return std::nullopt;
	}
	std::optional<kv::Value> getSnapshot(const kv::Key &key) override { return get(key); }
	std::unique_ptr<kv::IFuture> getAsync(const kv::Key & /*key*/) override { return nullptr; }
	std::unique_ptr<kv::IFuture> getSnapshotAsync(const kv::Key & /*key*/) override {
		return nullptr;
	}

	kv::GetRangeResult getRange(const kv::KeySelector &start, const kv::KeySelector &end,
	                            int limit) override {
		const auto &startKey = start.getKey();
		const auto &endKey = end.getKey();
		auto iterator =
		    start.isInclusive() ? snapshot_.lower_bound(startKey) : snapshot_.upper_bound(startKey);
		std::vector<kv::KeyValuePair> pairs;
		while (iterator != snapshot_.end() && iterator->first < endKey &&
		       static_cast<int>(pairs.size()) < limit) {
			pairs.push_back({.key=iterator->first, .value=iterator->second});
			++iterator;
		}
		const bool hasMore = iterator != snapshot_.end() && iterator->first < endKey;
		return {std::move(pairs), hasMore};
	}
	std::unique_ptr<kv::IRangeFuture> getRangeAsync(const kv::KeySelector & /*start*/,
	                                                const kv::KeySelector & /*end*/,
	                                                int /*limit*/) override {
		return nullptr;
	}

	void set(const kv::Key &key, const kv::Value &value) override {
		writes_[key] = value;
		++mutationCount_;
	}
	void atomicAdd(const kv::Key & /*key*/, const kv::Value & /*delta*/) override {
		++mutationCount_;
	}
	void atomicMax(const kv::Key & /*key*/, const kv::Value & /*value*/) override {
		++mutationCount_;
	}
	void remove(const kv::Key &key) override {
		writes_[key] = std::nullopt;
		++mutationCount_;
	}
	void removeRange(const kv::Key &start, const kv::Key &end) override {
		removedRanges_.emplace_back(start, end);
		++mutationCount_;
	}
	void addReadConflictKey(const kv::Key & /*key*/) override {}

	bool commit() override {
		std::lock_guard<std::mutex> lock(store_->mutex);
		for (const auto &[start, end] : removedRanges_) {
			auto iterator = store_->values.lower_bound(start);
			while (iterator != store_->values.end() && iterator->first < end) {
				iterator = store_->values.erase(iterator);
			}
		}
		for (const auto &[key, value] : writes_) {
			if (value.has_value()) {
				store_->values[key] = *value;
			} else {
				store_->values.erase(key);
			}
		}
		return true;
	}
	std::unique_ptr<kv::ICommitFuture> commitAsync() override {
		return std::make_unique<kv::ImmediateCommitFuture>(commit());
	}
	std::optional<int64_t> getCommittedVersion() const override { return std::nullopt; }
	uint64_t mutationCount() const override { return mutationCount_; }
	std::unique_ptr<kv::IVoidFuture> recoverAsync(int /*backendErrorCode*/) override {
		return std::make_unique<kv::ImmediateVoidFuture>();
	}

private:
	std::shared_ptr<SharedDurableStore> store_;
	DurableStore snapshot_;
	std::map<kv::Key, std::optional<kv::Value>> writes_;
	std::vector<std::pair<kv::Key, kv::Key>> removedRanges_;
	uint64_t mutationCount_{0};
};

class MemoryKVEngine final : public kv::IKVEngine {
public:
	std::unique_ptr<kv::IReadOnlyTransaction> createReadOnlyTransaction() override {
		return std::make_unique<MemoryTransaction>(store_);
	}
	std::unique_ptr<kv::IReadWriteTransaction> createReadWriteTransaction() override {
		return std::make_unique<MemoryTransaction>(store_);
	}

	void set(const kv::Key &key, const kv::Value &value) {
		std::lock_guard<std::mutex> lock(store_->mutex);
		store_->values[key] = value;
	}

	void remove(const kv::Key &key) {
		std::lock_guard<std::mutex> lock(store_->mutex);
		store_->values.erase(key);
	}

	void removePrefix(std::string_view prefix) {
		std::lock_guard<std::mutex> lock(store_->mutex);
		const auto start = kv::toBytes(prefix);
		const auto end = kv::prefixEnd(start);
		auto iterator = store_->values.lower_bound(start);
		while (iterator != store_->values.end() && iterator->first < end) {
			iterator = store_->values.erase(iterator);
		}
	}

	std::vector<kv::KeyValuePair> valuesWithPrefix(std::string_view prefix) const {
		std::lock_guard<std::mutex> lock(store_->mutex);
		const auto start = kv::toBytes(prefix);
		const auto end = kv::prefixEnd(start);
		std::vector<kv::KeyValuePair> values;
		for (auto iterator = store_->values.lower_bound(start);
		     iterator != store_->values.end() && iterator->first < end; ++iterator) {
			values.push_back({.key = iterator->first, .value = iterator->second});
		}
		return values;
	}

	std::vector<uint64_t> checkpointVersions() const {
		std::lock_guard<std::mutex> lock(store_->mutex);
		const auto iterator = store_->values.find(kv::toBytes(kMetaCheckpointVersionsKey));
		if (iterator == store_->values.end()) { return {}; }
		return checkpoints::deserializeCheckpointVersions(iterator->second);
	}

private:
	std::shared_ptr<SharedDurableStore> store_ = std::make_shared<SharedDurableStore>();
};

template <typename T>
kv::Value serializeValue(T value) {
	kv::Value encoded;
	serialize(encoded, value);
	return encoded;
}

void seedCheckpoint(MemoryKVEngine &engine, uint64_t descriptorVersion,
                    const std::vector<uint64_t> &checkpointVersions) {
	engine.set(kv::toBytes(kMetaMaxInodeIdKey), serializeValue(inode_t{101}));
	engine.set(kv::toBytes(kMetaVersionKey), serializeValue(descriptorVersion));
	engine.set(kv::toBytes(kMetaNextSessionKey), serializeValue(uint32_t{21}));
	engine.set(kv::toBytes(kMetaNextChunkIdKey), serializeValue(uint64_t{31}));
	if (!checkpointVersions.empty()) {
		engine.set(kv::toBytes(kMetaCheckpointVersionsKey),
		           checkpoints::serializeCheckpointVersions(checkpointVersions));
	}
}

kv::Value serializeLease(uint64_t targetVersion, uint64_t expiryUnixMs) {
	auto value = kv::toBytesBE(targetVersion);
	auto expiry = kv::toBytesBE(expiryUnixMs);
	value.insert(value.end(), expiry.begin(), expiry.end());
	return value;
}

class ScopedCheckpointRetention {
public:
	explicit ScopedCheckpointRetention(uint32_t retainedCopies)
	    : previous_(gStoredPreviousBackMetaCopies) {
		gStoredPreviousBackMetaCopies = retainedCopies;
	}
	~ScopedCheckpointRetention() { gStoredPreviousBackMetaCopies = previous_; }

private:
	uint32_t previous_;
};

TEST(MetadataCheckpointManager, LoadsDescriptorAndCatalogFromOneSnapshot) {
	// The first snapshot represents load starting before a concurrent seal. The second represents
	// the state after that seal and is a tripwire: descriptor and catalog reads plus the lease
	// write must all use the first read-write transaction.
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
	// A second transaction would consume the post-seal tripwire snapshot and recreate D2's race
	// between selecting a target and protecting it from pruning.
	EXPECT_EQ(engine.transactionCount(), size_t{1});
	manager.releaseLoadLease();
}

TEST(MetadataCheckpointManager, ValidatesDurableLoadLeaseOwnership) {
	MemoryKVEngine engine;
	seedCheckpoint(engine, 11, {7, 11});
	MetadataCheckpointManager manager(&engine);

	const auto descriptor = manager.loadLatestCheckpoint();
	ASSERT_EQ(descriptor.metadataVersion, uint64_t{11});
	const auto leases = engine.valuesWithPrefix(kMetaLoadLeaseKeyPrefix);
	ASSERT_EQ(leases.size(), size_t{1});
	ASSERT_EQ(leases.front().value.size(), 2 * sizeof(uint64_t));
	const uint8_t *leaseValue = leases.front().value.data();
	EXPECT_EQ(get64bit(&leaseValue), uint64_t{11});
	EXPECT_TRUE(manager.validateLoadLease());

	// Losing the durable row is terminal: reconstruction must fail at its next section boundary or
	// final publication check instead of continuing after its undo-retention guarantee disappeared.
	engine.removePrefix(kMetaLoadLeaseKeyPrefix);
	EXPECT_FALSE(manager.validateLoadLease());
	manager.releaseLoadLease();
}

TEST(MetadataCheckpointManager, LiveLoadLeaseProtectsCheckpointHistoryUntilRelease) {
	ScopedCheckpointRetention retention(/*retainedCopies=*/0);
	MemoryKVEngine engine;
	seedCheckpoint(engine, 30, {10, 20, 30});
	MetadataCheckpointManager loader(&engine);
	MetadataCheckpointManager sealer(&engine);

	ASSERT_EQ(loader.loadLatestCheckpoint().metadataVersion, uint64_t{30});
	ASSERT_TRUE(sealer.sealCheckpoint(
	    {.maxInodeId = 101, .metadataVersion = 40, .nextSessionId = 21, .nextChunkId = 31}));
	EXPECT_EQ(engine.checkpointVersions(), (std::vector<uint64_t>{30, 40}));

	ASSERT_TRUE(sealer.sealCheckpoint(
	    {.maxInodeId = 101, .metadataVersion = 50, .nextSessionId = 21, .nextChunkId = 31}));
	EXPECT_EQ(engine.checkpointVersions(), (std::vector<uint64_t>{30, 40, 50}));
	EXPECT_TRUE(loader.validateLoadLease());

	loader.releaseLoadLease();
	ASSERT_TRUE(sealer.sealCheckpoint(
	    {.maxInodeId = 101, .metadataVersion = 60, .nextSessionId = 21, .nextChunkId = 31}));
	EXPECT_EQ(engine.checkpointVersions(), (std::vector<uint64_t>{60}));
	EXPECT_TRUE(engine.valuesWithPrefix(kMetaLoadLeaseKeyPrefix).empty());
}

TEST(MetadataCheckpointManager, ExpiredLoadLeaseDoesNotBlockCheckpointPruning) {
	ScopedCheckpointRetention retention(/*retainedCopies=*/0);
	MemoryKVEngine engine;
	seedCheckpoint(engine, 30, {10, 20, 30});
	engine.set(kv::encodeKeyBE(kMetaLoadLeaseKeyPrefix, uint64_t{1}, uint64_t{2}),
	           serializeLease(/*targetVersion=*/10, /*expiryUnixMs=*/0));
	MetadataCheckpointManager sealer(&engine);

	ASSERT_TRUE(sealer.sealCheckpoint(
	    {.maxInodeId = 101, .metadataVersion = 40, .nextSessionId = 21, .nextChunkId = 31}));
	EXPECT_EQ(engine.checkpointVersions(), (std::vector<uint64_t>{40}));
	EXPECT_TRUE(engine.valuesWithPrefix(kMetaLoadLeaseKeyPrefix).empty());
}

TEST(MetadataCheckpointManager, RejectsInvalidCheckpointCatalogBeforeLeasing) {
	MemoryKVEngine emptyCatalogEngine;
	seedCheckpoint(emptyCatalogEngine, 11, {});
	MetadataCheckpointManager emptyCatalogManager(&emptyCatalogEngine);
	EXPECT_THROW(emptyCatalogManager.loadLatestCheckpoint(), MetadataConsistencyException);
	EXPECT_TRUE(emptyCatalogEngine.valuesWithPrefix(kMetaLoadLeaseKeyPrefix).empty());

	MemoryKVEngine malformedCatalogEngine;
	seedCheckpoint(malformedCatalogEngine, 11, {11, 7});
	MetadataCheckpointManager malformedCatalogManager(&malformedCatalogEngine);
	EXPECT_THROW(malformedCatalogManager.loadLatestCheckpoint(), MetadataConsistencyException);
	EXPECT_TRUE(malformedCatalogEngine.valuesWithPrefix(kMetaLoadLeaseKeyPrefix).empty());

	MemoryKVEngine unretainedTargetEngine;
	seedCheckpoint(unretainedTargetEngine, 11, {7, 10});
	MetadataCheckpointManager unretainedTargetManager(&unretainedTargetEngine);
	EXPECT_THROW(unretainedTargetManager.loadLatestCheckpoint(), MetadataConsistencyException);
	EXPECT_TRUE(unretainedTargetEngine.valuesWithPrefix(kMetaLoadLeaseKeyPrefix).empty());
}

}  // namespace
