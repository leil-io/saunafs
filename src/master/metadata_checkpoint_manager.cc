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
#include <chrono>
#include <cstring>
#include <functional>
#include <random>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

#include "common/datapack.h"
#include "common/serialization.h"
#include "kv/itransaction.h"
#include "kv/kv_utils.h"
#include "master/exceptions.h"
#include "master/kv_common_keys.h"
#include "master/metadata_backend_interface.h"
#include "master/metadata_checkpoint_helpers.h"
#include "master/metadata_checkpoint_manager.h"
#include "master/metadata_section_undo_recorder.h"
#include "slogger/slogger.h"

namespace {
using namespace std::chrono_literals;

// A heartbeat normally has 25 seconds to be scheduled before the lease expires. Sealers keep an
// additional ten seconds of clock-skew grace before treating the lease as dead, so modest
// wall-clock disagreement cannot make one host prune history that another still owns.
constexpr auto kLoadLeaseDuration = 30s;
constexpr auto kLoadLeaseHeartbeatInterval = 5s;
constexpr auto kLoadLeaseClockSkewGrace = 10s;

struct LoadLeaseRecord {
	uint64_t targetVersion;
	uint64_t expiryUnixMs;
};

uint64_t unixTimeMilliseconds() {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
	                                 std::chrono::system_clock::now().time_since_epoch())
	                                 .count());
}

constexpr uint64_t durationMilliseconds(std::chrono::milliseconds duration) {
	return static_cast<uint64_t>(duration.count());
}

bool leaseIsLive(uint64_t expiryUnixMs, uint64_t nowUnixMs) {
	if (expiryUnixMs >= nowUnixMs) { return true; }
	return nowUnixMs - expiryUnixMs <= durationMilliseconds(kLoadLeaseClockSkewGrace);
}

kv::Value serializeLoadLease(uint64_t targetVersion, uint64_t expiryUnixMs) {
	auto value = kv::toBytesBE(targetVersion);
	auto encodedExpiry = kv::toBytesBE(expiryUnixMs);
	value.insert(value.end(), encodedExpiry.begin(), encodedExpiry.end());
	return value;
}

std::optional<LoadLeaseRecord> deserializeLoadLease(const kv::Value &value) {
	if (value.size() != 2 * sizeof(uint64_t)) { return std::nullopt; }

	const uint8_t *valuePtr = value.data();
	LoadLeaseRecord record{.targetVersion = get64bit(&valuePtr),
	                       .expiryUnixMs = get64bit(&valuePtr)};
	if (record.targetVersion == 0) { return std::nullopt; }
	return record;
}

bool isValidCheckpointCatalog(const std::vector<uint64_t> &versions) {
	if (versions.empty() || versions.front() == 0) { return false; }
	return std::ranges::adjacent_find(versions, std::greater_equal<>()) == versions.end();
}

kv::Key makeLoadLeaseKey() {
	std::random_device randomDevice;
	auto random64 = [&randomDevice] {
		return (static_cast<uint64_t>(randomDevice()) << 32U) ^
		       static_cast<uint64_t>(randomDevice());
	};
	return kv::encodeKeyBE(kMetaLoadLeaseKeyPrefix, random64(), random64());
}
}  // namespace

MetadataCheckpointManager::MetadataCheckpointManager(kv::IKVEngine *kvEngine)
    : kvEngine_(kvEngine), loadLeaseKey_(makeLoadLeaseKey()) {
	initializeRecorders();
}

MetadataCheckpointManager::~MetadataCheckpointManager() { releaseLoadLease(); }

bool MetadataCheckpointManager::beginCheckpoint(const MetadataCheckpointDescriptor &descriptor) {
	if (pendingCheckpoint_.has_value()) {
		safs::log_warn("Replacing pending metadata checkpoint at version {} with version {}",
		               pendingCheckpoint_->metadataVersion, descriptor.metadataVersion);
	}

	pendingCheckpoint_ = descriptor;
	return true;
}

bool MetadataCheckpointManager::sealCheckpoint(const MetadataCheckpointDescriptor &descriptor) {
	if (descriptor.metadataVersion == 0) {
		safs::log_warn("{}: cannot seal a checkpoint with version 0", __func__);
		return false;
	}

	auto transaction = kvEngine_->createReadWriteTransaction();
	if (!checkpointVersionsLoaded_) { loadCheckpointVersions(transaction.get()); }
	if (!retainedCheckpointVersions_.empty() &&
	    !isValidCheckpointCatalog(retainedCheckpointVersions_)) {
		safs::log_err("Cannot seal checkpoint version {}: retained catalog is malformed",
		              descriptor.metadataVersion);
		return false;
	}

	std::optional<uint64_t> protectedVersion;
	if (!collectProtectedCheckpointVersion(transaction.get(), protectedVersion)) { return false; }

	if (!persistCheckpointDescriptor(transaction.get(), descriptor)) {
		safs::log_err(
		    "Failed to persist metadata checkpoint descriptor version {}: transaction error",
		    descriptor.metadataVersion);
		return false;
	}

	// Compute the next catalog and the dropped versions on local copies so the manager's
	// in-memory state is mutated only after the transaction commits successfully.
	std::vector<uint64_t> nextCheckpointVersions;
	std::vector<uint64_t> droppedVersions;
	computeRetainedCheckpointVersions(descriptor.metadataVersion, protectedVersion,
	                                  nextCheckpointVersions, droppedVersions);

	if (checkpoints::saveCheckpointVersions(transaction.get(), nextCheckpointVersions) !=
	    kOpSuccess) {
		safs::log_err("Failed to persist checkpoint catalog for version {}",
		              descriptor.metadataVersion);
		return false;
	}

	removeDroppedCheckpointVersions(transaction.get(), droppedVersions);

	if (!transaction->commit()) {
		safs::log_err("Failed to seal checkpoint version {}: transaction commit failed",
		              descriptor.metadataVersion);
		return false;
	}

	// Commit succeeded: now it is safe to update the in-memory state.
	retainedCheckpointVersions_ = std::move(nextCheckpointVersions);
	resetIntervalState();
	pendingCheckpoint_.reset();
	activeCheckpointVersion_ = descriptor.metadataVersion;

	safs::log_info("Checkpoint version {} sealed successfully", descriptor.metadataVersion);
	return true;
}

MetadataCheckpointDescriptor MetadataCheckpointManager::loadLatestCheckpoint() {
	{
		std::lock_guard<std::mutex> lock(loadLeaseMutex_);
		if (loadLeaseActive_) {
			throw MetadataConsistencyException(
			    "checkpoint reconstruction already owns a load lease");
		}
	}

	MetadataCheckpointDescriptor descriptor;
	auto transaction = kvEngine_->createReadWriteTransaction();

	// A present-but-undersized key means corrupted restore state; fail fast instead of
	// silently falling back to defaults (which could reuse inode/chunk/session ids).
	auto maxInodeValue = transaction->get(kv::toBytes(kMetaMaxInodeIdKey));
	if (maxInodeValue.has_value()) {
		if (maxInodeValue->size() < sizeof(inode_t)) {
			throw MetadataConsistencyException("Invalid size for META_MAX_INODE_ID key");
		}
		const uint8_t *data = maxInodeValue->data();
		getINode(&data, descriptor.maxInodeId);
	}

	auto versionValue = transaction->get(kv::toBytes(kMetaVersionKey));
	if (versionValue.has_value()) {
		if (versionValue->size() < sizeof(uint64_t)) {
			throw MetadataConsistencyException("Invalid size for META_VERSION key");
		}
		const uint8_t *data = versionValue->data();
		descriptor.metadataVersion = get64bit(&data);
	}

	auto nextSessionValue = transaction->get(kv::toBytes(kMetaNextSessionKey));
	if (nextSessionValue.has_value()) {
		if (nextSessionValue->size() < sizeof(uint32_t)) {
			throw MetadataConsistencyException("Invalid size for META_NEXT_SESSION key");
		}
		const uint8_t *data = nextSessionValue->data();
		get32bit(&data, descriptor.nextSessionId);
	}

	auto nextChunkValue = transaction->get(kv::toBytes(kMetaNextChunkIdKey));
	if (nextChunkValue.has_value()) {
		if (nextChunkValue->size() < sizeof(uint64_t)) {
			throw MetadataConsistencyException("Invalid size for META_NEXT_CHUNK_ID key");
		}
		const uint8_t *data = nextChunkValue->data();
		descriptor.nextChunkId = get64bit(&data);
	}

	auto checkpointVersions = checkpoints::loadCheckpointVersions(transaction.get());
	if (!isValidCheckpointCatalog(checkpointVersions)) {
		throw MetadataConsistencyException("Checkpoint catalog is empty or malformed");
	}
	if (descriptor.metadataVersion == 0 ||
	    checkpointVersions.back() != descriptor.metadataVersion) {
		throw MetadataConsistencyException(
		    "Checkpoint descriptor version does not match the retained catalog");
	}

	const uint64_t expiryUnixMs = unixTimeMilliseconds() + durationMilliseconds(kLoadLeaseDuration);
	transaction->set(loadLeaseKey_, serializeLoadLease(descriptor.metadataVersion, expiryUnixMs));
	if (!transaction->commit()) {
		throw MetadataConsistencyException(
		    "Failed to acquire checkpoint reconstruction load lease");
	}

	retainedCheckpointVersions_ = std::move(checkpointVersions);
	activeCheckpointVersion_ = descriptor.metadataVersion;

	resetIntervalState();
	pendingCheckpoint_.reset();
	checkpointVersionsLoaded_ = true;

	{
		std::lock_guard<std::mutex> lock(loadLeaseMutex_);
		loadLeaseTargetVersion_ = descriptor.metadataVersion;
		loadLeaseExpiryUnixMs_ = expiryUnixMs;
		loadLeaseStop_ = false;
		loadLeaseLost_ = false;
		loadLeaseActive_ = true;
	}

	try {
		loadLeaseHeartbeat_ = std::thread(&MetadataCheckpointManager::loadLeaseHeartbeatLoop, this);
	} catch (...) {
		releaseLoadLease();
		throw;
	}

	safs::log_info("Acquired checkpoint load lease for version {}", descriptor.metadataVersion);

	return descriptor;
}

bool MetadataCheckpointManager::validateLoadLease() {
	kv::Key leaseKey;
	uint64_t targetVersion = 0;
	{
		std::lock_guard<std::mutex> lock(loadLeaseMutex_);
		if (!loadLeaseActive_ || loadLeaseLost_ ||
		    !leaseIsLive(loadLeaseExpiryUnixMs_, unixTimeMilliseconds())) {
			return false;
		}
		leaseKey = loadLeaseKey_;
		targetVersion = loadLeaseTargetVersion_;
	}

	try {
		auto transaction = kvEngine_->createReadOnlyTransaction();
		auto leaseValue = transaction->get(leaseKey);
		auto checkpointVersions = checkpoints::loadCheckpointVersions(transaction.get());
		if (!leaseValue.has_value()) {
			markLoadLeaseLost("durable lease key is missing");
			return false;
		}

		auto record = deserializeLoadLease(*leaseValue);
		if (!record.has_value() || record->targetVersion != targetVersion ||
		    !leaseIsLive(record->expiryUnixMs, unixTimeMilliseconds())) {
			markLoadLeaseLost("durable lease is malformed, changed, or expired");
			return false;
		}
		if (!isValidCheckpointCatalog(checkpointVersions) ||
		    !std::ranges::binary_search(checkpointVersions, targetVersion)) {
			markLoadLeaseLost("checkpoint catalog is malformed or no longer contains the target");
			return false;
		}
	} catch (const std::exception &exception) {
		safs::log_err("Failed to validate checkpoint load lease: {}", exception.what());
		markLoadLeaseLost("lease validation transaction failed");
		return false;
	} catch (...) {
		markLoadLeaseLost("lease validation transaction failed with an unknown exception");
		return false;
	}

	return true;
}

void MetadataCheckpointManager::releaseLoadLease() noexcept {
	bool hadActiveLease = false;
	{
		std::lock_guard<std::mutex> lock(loadLeaseMutex_);
		hadActiveLease = loadLeaseActive_;
		loadLeaseStop_ = true;
	}
	loadLeaseCv_.notify_all();

	if (loadLeaseHeartbeat_.joinable()) { loadLeaseHeartbeat_.join(); }

	if (hadActiveLease) {
		try {
			auto transaction = kvEngine_->createReadWriteTransaction();
			transaction->remove(loadLeaseKey_);
			if (!transaction->commit()) {
				safs::log_warn(
				    "Failed to release checkpoint load lease; it will be removed after expiry");
			} else {
				safs::log_info("Released checkpoint load lease");
			}
		} catch (const std::exception &exception) {
			safs::log_warn("Failed to release checkpoint load lease: {}; it will expire",
			               exception.what());
		} catch (...) {
			safs::log_warn(
			    "Failed to release checkpoint load lease: unknown error; it will expire");
		}
	}

	std::lock_guard<std::mutex> lock(loadLeaseMutex_);
	loadLeaseTargetVersion_ = 0;
	loadLeaseExpiryUnixMs_ = 0;
	loadLeaseActive_ = false;
	loadLeaseStop_ = false;
	loadLeaseLost_ = false;
}

void MetadataCheckpointManager::loadLeaseHeartbeatLoop() {
	while (true) {
		std::unique_lock<std::mutex> lock(loadLeaseMutex_);
		if (loadLeaseCv_.wait_for(lock, kLoadLeaseHeartbeatInterval, [this] {
			    return loadLeaseStop_ || loadLeaseLost_ || !loadLeaseActive_;
		    })) {
			return;
		}
		lock.unlock();

		if (!renewLoadLease()) { return; }
	}
}

bool MetadataCheckpointManager::renewLoadLease() {
	kv::Key leaseKey;
	uint64_t targetVersion = 0;
	uint64_t cachedExpiryUnixMs = 0;
	{
		std::lock_guard<std::mutex> lock(loadLeaseMutex_);
		if (!loadLeaseActive_ || loadLeaseStop_ || loadLeaseLost_) { return false; }
		leaseKey = loadLeaseKey_;
		targetVersion = loadLeaseTargetVersion_;
		cachedExpiryUnixMs = loadLeaseExpiryUnixMs_;
	}

	try {
		auto transaction = kvEngine_->createReadWriteTransaction();
		auto leaseValue = transaction->get(leaseKey);
		if (!leaseValue.has_value()) {
			markLoadLeaseLost("heartbeat found that the durable lease key is missing");
			return false;
		}

		auto record = deserializeLoadLease(*leaseValue);
		const uint64_t nowUnixMs = unixTimeMilliseconds();
		if (!record.has_value() || record->targetVersion != targetVersion ||
		    !leaseIsLive(record->expiryUnixMs, nowUnixMs)) {
			markLoadLeaseLost("heartbeat found that the durable lease changed or expired");
			return false;
		}

		const uint64_t nextExpiryUnixMs = nowUnixMs + durationMilliseconds(kLoadLeaseDuration);
		transaction->set(leaseKey, serializeLoadLease(targetVersion, nextExpiryUnixMs));
		if (transaction->commit()) {
			std::lock_guard<std::mutex> lock(loadLeaseMutex_);
			if (loadLeaseActive_ && loadLeaseTargetVersion_ == targetVersion) {
				loadLeaseExpiryUnixMs_ = nextExpiryUnixMs;
			}
			return true;
		}
		safs::log_warn("Checkpoint load lease heartbeat commit failed; retrying while live");
	} catch (const std::exception &exception) {
		safs::log_warn("Checkpoint load lease heartbeat failed: {}; retrying while live",
		               exception.what());
	} catch (...) {
		safs::log_warn(
		    "Checkpoint load lease heartbeat failed with an unknown error; retrying while live");
	}

	if (leaseIsLive(cachedExpiryUnixMs, unixTimeMilliseconds())) { return true; }
	markLoadLeaseLost("heartbeat could not renew the lease before its expiry margin");
	return false;
}

void MetadataCheckpointManager::markLoadLeaseLost(std::string_view reason) {
	bool newlyLost = false;
	{
		std::lock_guard<std::mutex> lock(loadLeaseMutex_);
		if (loadLeaseActive_ && !loadLeaseLost_) {
			loadLeaseLost_ = true;
			newlyLost = true;
		}
	}
	loadLeaseCv_.notify_all();
	if (newlyLost) { safs::log_err("Checkpoint load lease lost: {}", reason); }
}

void MetadataCheckpointManager::reloadDurableCheckpointState() {
	auto transaction = kvEngine_->createReadOnlyTransaction();
	loadCheckpointVersions(transaction.get());
	resetIntervalState();
	pendingCheckpoint_.reset();
}

void MetadataCheckpointManager::recordPreMutation(const MetadataMutationContext &context,
	                       const MetadataMutation &mutation) {
	if (context.transaction == nullptr || context.checkpointVersion == 0) { return; }

	MetadataSectionKind section = std::visit(
	    [](const auto &typedMutation) -> MetadataSectionKind {
		    using T = std::decay_t<decltype(typedMutation)>;

		    if constexpr (std::is_same_v<T, ChunkSetMutation>) {
			    return MetadataSectionKind::Chunk;
		    } else if constexpr (std::is_same_v<T, NodeSetMutation> ||
		                         std::is_same_v<T, NodeRemoveMutation>) {
			    return MetadataSectionKind::Node;
		    } else if constexpr (std::is_same_v<T, FreeNodeSetMutation> ||
		                         std::is_same_v<T, FreeNodeRemoveMutation>) {
			    return MetadataSectionKind::FreeNode;
		    } else if constexpr (std::is_same_v<T, EdgeSetMutation> ||
		                         std::is_same_v<T, EdgeRemoveMutation>) {
			    return MetadataSectionKind::Edge;
		    } else if constexpr (std::is_same_v<T, XAttrSetMutation> ||
		                         std::is_same_v<T, XAttrRemoveMutation> ||
		                         std::is_same_v<T, XAttrRangeRemoveMutation>) {
			    return MetadataSectionKind::XAttr;
		    } else if constexpr (std::is_same_v<T, QuotaSetMutation> ||
		                         std::is_same_v<T, QuotaRemoveMutation>) {
			    return MetadataSectionKind::Quota;
		    } else {
			    // Force a compile error if a new MetadataMutation alternative is added
			    // without being mapped to a section here.
			    static_assert(!sizeof(T *), "Unhandled mutation type in std::visit");
		    }
	    },
	    mutation);

	if (auto *recorder = recorderFor(section)) { recorder->beforeMutation(context, mutation); }
}

bool MetadataCheckpointManager::restoreSectionToCheckpointVersion(MetadataSectionKind section,
                                                                  uint64_t targetVersion) {
	auto section_ = sectionName(section);
	safs::log_info("Restoring section {} to checkpoint version {}", section_, targetVersion);

	if (targetVersion == 0 || section_ == "Unknown") {
		safs::log_info(
		    "Invalid target checkpoint version {} or section {}, skipping restore for section {}",
		    targetVersion, section_, section_);
		return false;
	}

	if (auto *recorder = recorderFor(section)) {
		return recorder->restoreToCheckpointVersion(targetVersion);
	}

	return false;
}

const std::unordered_set<uint64_t> &MetadataCheckpointManager::nodesRemovedDuringRestore() const {
	static const std::unordered_set<uint64_t> kEmpty;
	return nodeUndoRecorder_ ? nodeUndoRecorder_->removedDuringRestore() : kEmpty;
}

void MetadataCheckpointManager::initializeRecorders() {
	chunkUndoRecorder_ = std::make_unique<ChunkUndoRecorder>(kvEngine_);
	nodeUndoRecorder_ = std::make_unique<NodeUndoRecorder>(kvEngine_);
	edgeUndoRecorder_ = std::make_unique<EdgeUndoRecorder>(kvEngine_);
	xattrUndoRecorder_ = std::make_unique<XAttrUndoRecorder>(kvEngine_);
	quotaUndoRecorder_ = std::make_unique<QuotaUndoRecorder>(kvEngine_);

	recorders_[static_cast<size_t>(MetadataSectionKind::Chunk)] = chunkUndoRecorder_.get();
	recorders_[static_cast<size_t>(MetadataSectionKind::Node)] = nodeUndoRecorder_.get();
	recorders_[static_cast<size_t>(MetadataSectionKind::Edge)] = edgeUndoRecorder_.get();
	recorders_[static_cast<size_t>(MetadataSectionKind::XAttr)] = xattrUndoRecorder_.get();
	recorders_[static_cast<size_t>(MetadataSectionKind::Quota)] = quotaUndoRecorder_.get();
}

ISectionUndoRecorder *MetadataCheckpointManager::recorderFor(MetadataSectionKind section) {
	for (auto *recorder : recorders_) {
		if (recorder != nullptr && recorder->sectionKind() == section) { return recorder; }
	}

	return nullptr;
}

void MetadataCheckpointManager::resetIntervalState() {
	for (auto *recorder : recorders_) {
		if (recorder != nullptr) { recorder->resetIntervalState(); }
	}
}

bool MetadataCheckpointManager::persistCheckpointDescriptor(
    kv::IReadWriteTransaction *transaction, const MetadataCheckpointDescriptor &descriptor) const {
	if (transaction == nullptr) { return false; }

	kv::Value maxInodeIdValue;
	serialize(maxInodeIdValue, descriptor.maxInodeId);
	transaction->set(kv::toBytes(kMetaMaxInodeIdKey), maxInodeIdValue);

	kv::Value metadataVersionValue;
	serialize(metadataVersionValue, descriptor.metadataVersion);
	transaction->set(kv::toBytes(kMetaVersionKey), metadataVersionValue);

	kv::Value nextSessionIdValue;
	serialize(nextSessionIdValue, descriptor.nextSessionId);
	transaction->set(kv::toBytes(kMetaNextSessionKey), nextSessionIdValue);

	kv::Value nextChunkIdValue;
	serialize(nextChunkIdValue, descriptor.nextChunkId);
	transaction->set(kv::toBytes(kMetaNextChunkIdKey), nextChunkIdValue);

	return true;
}

void MetadataCheckpointManager::loadCheckpointVersions(kv::IReadOnlyTransaction *transaction) {
	retainedCheckpointVersions_ = checkpoints::loadCheckpointVersions(transaction);

	std::string checkpoints;
	for (const auto &checkpoint : retainedCheckpointVersions_) {
		checkpoints += std::to_string(checkpoint) + " ";
	}
	safs::log_info("Loaded {} checkpoint versions from FDB: [ {}]",
	               retainedCheckpointVersions_.size(), checkpoints);

	activeCheckpointVersion_ =
	    retainedCheckpointVersions_.empty() ? 0 : retainedCheckpointVersions_.back();
	checkpointVersionsLoaded_ = true;
}

bool MetadataCheckpointManager::collectProtectedCheckpointVersion(
    kv::IReadWriteTransaction *transaction, std::optional<uint64_t> &oldestTarget) {
	if (transaction == nullptr) { return false; }

	const kv::Key leasePrefix = kv::toBytes(kMetaLoadLeaseKeyPrefix);
	const kv::Key leaseRangeEnd = kv::prefixEnd(leasePrefix);
	kv::KeySelector startSelector(leasePrefix, true, 0);
	const kv::KeySelector endSelector(leaseRangeEnd, true, 0);
	const uint64_t nowUnixMs = unixTimeMilliseconds();
	oldestTarget.reset();

	while (true) {
		auto page = transaction->getRange(startSelector, endSelector, kv::kDefaultGetRangeLimit);
		for (const auto &pair : page.getPairs()) {
			if (pair.key.size() != kMetaLoadLeaseKeyPrefix.size() + (2 * sizeof(uint64_t))) {
				safs::log_err("Cannot seal checkpoint: malformed load lease key {}",
				              kv::keyToEscapedAscii(pair.key));
				return false;
			}

			auto record = deserializeLoadLease(pair.value);
			if (!record.has_value()) {
				safs::log_err("Cannot seal checkpoint: malformed load lease value at {}",
				              kv::keyToEscapedAscii(pair.key));
				return false;
			}

			if (!leaseIsLive(record->expiryUnixMs, nowUnixMs)) {
				transaction->remove(pair.key);
				continue;
			}

			if (!std::ranges::binary_search(retainedCheckpointVersions_, record->targetVersion)) {
				safs::log_err(
				    "Cannot seal checkpoint: live load lease targets unretained version {}",
				    record->targetVersion);
				return false;
			}

			if (!oldestTarget.has_value() || record->targetVersion < *oldestTarget) {
				oldestTarget = record->targetVersion;
			}
		}

		if (!page.hasMore() || page.getPairs().empty()) { break; }
		startSelector = kv::KeySelector(page.getPairs().back().key, false, 0);
	}

	return true;
}

void MetadataCheckpointManager::computeRetainedCheckpointVersions(
    uint64_t newVersion, std::optional<uint64_t> protectedVersion, std::vector<uint64_t> &retained,
    std::vector<uint64_t> &dropped) const {
	std::set<uint64_t> checkpointVersionsSet(retainedCheckpointVersions_.begin(),
	                                         retainedCheckpointVersions_.end());

	// Add the new checkpoint version to the checkpoint versions set
	checkpointVersionsSet.insert(newVersion);

	// gStoredPreviousBackMetaCopies is uint32_t (never negative); the +1 keeps the active
	// checkpoint, and on 64-bit size_t the addition cannot overflow.
	const size_t maxRetainedCheckpoints = static_cast<size_t>(gStoredPreviousBackMetaCopies) + 1;

	// Trim the set to keep only the last 'gStoredPreviousBackMetaCopies + 1' versions
	dropped.clear();
	while (checkpointVersionsSet.size() > maxRetainedCheckpoints &&
	       (!protectedVersion.has_value() || *checkpointVersionsSet.begin() < *protectedVersion)) {
		dropped.push_back(*checkpointVersionsSet.begin());
		checkpointVersionsSet.erase(checkpointVersionsSet.begin());
	}

	if (checkpointVersionsSet.size() > maxRetainedCheckpoints) {
		safs::log_warn(
		    "Checkpoint load lease for version {} retains {} checkpoints, exceeding configured "
		    "retention of {}",
		    *protectedVersion, checkpointVersionsSet.size(), maxRetainedCheckpoints);
	}

	retained.assign(checkpointVersionsSet.begin(), checkpointVersionsSet.end());
}

int8_t MetadataCheckpointManager::removeDroppedCheckpointVersions(
    kv::IReadWriteTransaction *transaction, const std::vector<uint64_t> &droppedVersions) {
	if (transaction == nullptr) { return kOpFailure; }

	// Best-effort cleanup of per-checkpoint data for the dropped versions using the recorders.
	for (uint64_t droppedVersion : droppedVersions) {
		for (auto *recorder : recorders_) {
			if (recorder != nullptr) {
				if (recorder->dropCheckpointData(transaction, droppedVersion) != kOpSuccess) {
					safs::log_warn("Failed to drop checkpoint version {} in section {}",
					               droppedVersion, sectionName(recorder->sectionKind()));
				}
			}
		}
	}

	return kOpSuccess;
}
