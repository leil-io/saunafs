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

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "kv/ifuture.h"
#include "kv/ikv_engine.h"
#include "kv/itransaction.h"
#include "kv/kv_utils.h"
#include "master/kv_common_keys.h"
#include "master/metadata_node_undo_recorder.h"
#include "master/metadata_writer_fdb.h"

namespace {

// Minimal in-memory transaction: writes are no-ops and commit always succeeds, so a flush drains
// the queue without a real FDB cluster. getApproximateSize() is left at the interface default
// (nullopt), so the writer falls back to count-based batching.
class NoopTransaction : public kv::IReadWriteTransaction {
public:
	/// @param appliedSink Optional cross-transaction counter incremented once per buffered
	///   mutation. The writer destroys each transaction after committing it, so a test that needs
	///   to know how many events actually reached the backend has to accumulate outside them.
	explicit NoopTransaction(std::atomic<uint64_t> *appliedSink = nullptr)
	    : appliedSink_(appliedSink) {}

	std::optional<kv::Value> get(const kv::Key & /*key*/) override { return std::nullopt; }
	std::optional<kv::Value> getSnapshot(const kv::Key & /*key*/) override { return std::nullopt; }
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

	void set(const kv::Key & /*key*/, const kv::Value & /*value*/) override { noteMutation(); }
	void atomicAdd(const kv::Key & /*key*/, const kv::Value & /*delta*/) override {
		noteMutation();
	}
	void atomicMax(const kv::Key & /*key*/, const kv::Value & /*value*/) override {
		noteMutation();
	}
	void remove(const kv::Key & /*key*/) override { noteMutation(); }
	void removeRange(const kv::Key & /*start*/, const kv::Key & /*end*/) override {
		noteMutation();
	}
	// Conflict annotation, not a buffered write: leave mutationCount_ untouched.
	void addReadConflictKey(const kv::Key & /*key*/) override {}

	bool commit() override { return true; }
	// The async pipeline dereferences this future unconditionally when it reaps the commit, so it
	// must not be nullptr. An in-memory backend is durable the moment the mutations are buffered.
	std::unique_ptr<kv::ICommitFuture> commitAsync() override {
		return std::make_unique<kv::ImmediateCommitFuture>(/*success=*/true);
	}
	// Contract forbids nullptr; an in-memory backend has no recovery work, so hand back an
	// already-successful future (this mock never fails a commit, so it is never called).
	std::unique_ptr<kv::IVoidFuture> recoverAsync(int /*backendErrorCode*/) override {
		return std::make_unique<kv::ImmediateVoidFuture>();
	}
	std::optional<int64_t> getCommittedVersion() const override { return std::nullopt; }
	uint64_t mutationCount() const override { return mutationCount_; }

private:
	void noteMutation() {
		++mutationCount_;
		if (appliedSink_ != nullptr) { appliedSink_->fetch_add(1, std::memory_order_relaxed); }
	}

	std::atomic<uint64_t> *appliedSink_;
	uint64_t mutationCount_{0};
};

class NoopKVEngine : public kv::IKVEngine {
public:
	std::unique_ptr<kv::IReadOnlyTransaction> createReadOnlyTransaction() override {
		return std::make_unique<NoopTransaction>();
	}
	std::unique_ptr<kv::IReadWriteTransaction> createReadWriteTransaction() override {
		return std::make_unique<NoopTransaction>();
	}
};

// Engine whose first read-write transaction construction throws, modelling an FDB client failure
// before a commit can be submitted. The worker's retry then blocks in the factory until released,
// giving tests a deterministic point at which the failed batch has already been requeued.
class ThrowingKVEngine : public kv::IKVEngine {
public:
	std::unique_ptr<kv::IReadOnlyTransaction> createReadOnlyTransaction() override {
		return std::make_unique<NoopTransaction>();
	}

	std::unique_ptr<kv::IReadWriteTransaction> createReadWriteTransaction() override {
		std::unique_lock lock(mutex_);
		if (!failureInjected_) {
			failureInjected_ = true;
			throw std::runtime_error("simulated FDB client failure");
		}

		if (!recovered_) {
			retryBlocked_ = true;
			cv_.notify_all();
			cv_.wait(lock, [this] { return recovered_; });
		}
		lock.unlock();
		return std::make_unique<NoopTransaction>(&applied_);
	}

	/// Waits until the worker retries after handling and requeuing the failed transaction build.
	bool waitForBlockedRetry(std::chrono::milliseconds timeout) {
		std::unique_lock lock(mutex_);
		return cv_.wait_for(lock, timeout, [this] { return retryBlocked_; });
	}

	/// Lets the blocked retry build its transaction, simulating the client recovering.
	void stopThrowing() {
		{
			std::lock_guard lock(mutex_);
			recovered_ = true;
		}
		cv_.notify_all();
	}

	/// Total mutations buffered across every transaction this engine handed out.
	uint64_t applied() const { return applied_.load(std::memory_order_relaxed); }

private:
	std::mutex mutex_;
	std::condition_variable cv_;
	bool failureInjected_{false};
	bool retryBlocked_{false};
	bool recovered_{false};
	std::atomic<uint64_t> applied_{0};
};

std::unique_ptr<IMetadataUpdateEvent> makeEvent(uint64_t seed) {
	return std::make_unique<NodeRemoveEvent>(static_cast<inode_t>(seed + 1));
}

/// Spins (bounded) until `predicate` holds, so the async worker's progress is awaited rather than
/// slept on: fast on an idle machine, tolerant on a loaded CI box. Returns whether it held.
template <typename Predicate>
bool waitFor(Predicate predicate, std::chrono::milliseconds limit) {
	const auto deadline = std::chrono::steady_clock::now() + limit;
	while (std::chrono::steady_clock::now() < deadline) {
		if (predicate()) { return true; }
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return predicate();
}

// Shared gate used to hold one asynchronous commit at a deterministic point.
struct CommitGate {
	std::mutex mutex;
	std::condition_variable cv;
	bool ready{false};
	bool getResultEntered{false};
	bool consumed{false};
	bool success{true};
	int error{0};
	bool retryable{false};
	void (*readyCallback)(void *){nullptr};
	void *readyCallbackArgument{nullptr};
};

void releaseCommitGate(const std::shared_ptr<CommitGate> &gate, bool success = true, int error = 0,
                       bool retryable = false) {
	void (*callback)(void *) = nullptr;
	void *callbackArgument = nullptr;
	{
		std::lock_guard<std::mutex> lock(gate->mutex);
		if (gate->ready) { return; }
		gate->success = success;
		gate->error = error;
		gate->retryable = retryable;
		gate->ready = true;
		callback = gate->readyCallback;
		callbackArgument = gate->readyCallbackArgument;
	}
	gate->cv.notify_all();
	if (callback != nullptr) { callback(callbackArgument); }
}

class BlockingCommitFuture final : public kv::ICommitFuture {
public:
	explicit BlockingCommitFuture(std::shared_ptr<CommitGate> gate) : gate_(std::move(gate)) {}

	bool isReady() override {
		std::lock_guard<std::mutex> lock(gate_->mutex);
		return gate_->ready;
	}

	bool getResult(int *error, bool *retryable) override {
		std::unique_lock<std::mutex> lock(gate_->mutex);
		gate_->getResultEntered = true;
		gate_->cv.notify_all();
		gate_->cv.wait(lock, [this] { return gate_->ready; });

		if (error != nullptr) { *error = gate_->error; }
		if (retryable != nullptr) { *retryable = gate_->retryable; }
		if (gate_->consumed) { return false; }
		gate_->consumed = true;
		return gate_->success;
	}

	void setReadyCallback(void (*callback)(void *), void *arg) override {
		bool callNow = false;
		{
			std::lock_guard<std::mutex> lock(gate_->mutex);
			if (gate_->ready) {
				callNow = true;
			} else {
				gate_->readyCallback = callback;
				gate_->readyCallbackArgument = arg;
			}
		}
		if (callNow && callback != nullptr) { callback(arg); }
	}

private:
	std::shared_ptr<CommitGate> gate_;
};

class BlockingTransaction final : public NoopTransaction {
public:
	explicit BlockingTransaction(std::shared_ptr<CommitGate> gate) : gate_(std::move(gate)) {}

	std::unique_ptr<kv::ICommitFuture> commitAsync() override {
		return std::make_unique<BlockingCommitFuture>(gate_);
	}

private:
	std::shared_ptr<CommitGate> gate_;
};

class BlockingKVEngine final : public kv::IKVEngine {
public:
	std::unique_ptr<kv::IReadOnlyTransaction> createReadOnlyTransaction() override {
		return std::make_unique<NoopTransaction>();
	}

	std::unique_ptr<kv::IReadWriteTransaction> createReadWriteTransaction() override {
		auto gate = std::make_shared<CommitGate>();
		size_t index = 0;
		bool releaseImmediately = false;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			index = commitGates_.size();
			commitGates_.push_back(gate);
			releaseImmediately = releaseAll_;
		}
		cv_.notify_all();

		if (index == 0) {
			std::unique_lock<std::mutex> lock(mutex_);
			cv_.wait(lock, [this] { return allowFirstTransaction_ || releaseAll_; });
			releaseImmediately = releaseImmediately || releaseAll_;
		}

		if (releaseImmediately) { releaseCommitGate(gate); }
		return std::make_unique<BlockingTransaction>(std::move(gate));
	}

	bool waitForTransactionCount(size_t count, std::chrono::milliseconds limit) {
		std::unique_lock<std::mutex> lock(mutex_);
		return cv_.wait_for(lock, limit, [this, count] { return commitGates_.size() >= count; });
	}

	bool waitForCommitWaiter(size_t index, std::chrono::milliseconds limit) {
		std::shared_ptr<CommitGate> gate;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			if (index >= commitGates_.size()) { return false; }
			gate = commitGates_[index];
		}

		std::unique_lock<std::mutex> lock(gate->mutex);
		return gate->cv.wait_for(lock, limit, [&gate] { return gate->getResultEntered; });
	}

	size_t transactionCount() const {
		std::lock_guard<std::mutex> lock(mutex_);
		return commitGates_.size();
	}

	void allowFirstTransaction() {
		{
			std::lock_guard<std::mutex> lock(mutex_);
			allowFirstTransaction_ = true;
		}
		cv_.notify_all();
	}

	void releaseCommit(size_t index) {
		std::shared_ptr<CommitGate> gate;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			if (index >= commitGates_.size()) { return; }
			gate = commitGates_[index];
		}
		releaseCommitGate(gate);
	}

	void failCommit(size_t index, bool retryable) {
		std::shared_ptr<CommitGate> gate;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			if (index >= commitGates_.size()) { return; }
			gate = commitGates_[index];
		}
		const int error = retryable ? 1020 : 1021;
		releaseCommitGate(gate, /*success=*/false, error, retryable);
	}

	void releaseAllCommits() {
		std::vector<std::shared_ptr<CommitGate>> gates;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			releaseAll_ = true;
			allowFirstTransaction_ = true;
			gates = commitGates_;
		}
		cv_.notify_all();
		for (const auto &gate : gates) { releaseCommitGate(gate); }
	}

private:
	mutable std::mutex mutex_;
	std::condition_variable cv_;
	std::vector<std::shared_ptr<CommitGate>> commitGates_;
	bool allowFirstTransaction_{false};
	bool releaseAll_{false};
};

using DurableStore = std::map<kv::Key, kv::Value>;

class RecordingTransaction final : public NoopTransaction {
public:
	RecordingTransaction(DurableStore &store, bool commitSucceeds)
	    : store_(store), commitSucceeds_(commitSucceeds) {}

	std::optional<kv::Value> get(const kv::Key &key) override {
		if (const auto pending = writes_.find(key); pending != writes_.end()) {
			return pending->second;
		}
		if (const auto durable = store_.find(key); durable != store_.end()) {
			return durable->second;
		}
		return std::nullopt;
	}

	void set(const kv::Key &key, const kv::Value &value) override { writes_[key] = value; }

	void remove(const kv::Key &key) override { writes_[key] = std::nullopt; }

	bool commit() override {
		if (!commitSucceeds_) { return false; }
		for (const auto &[key, value] : writes_) {
			if (value.has_value()) {
				store_[key] = *value;
			} else {
				store_.erase(key);
			}
		}
		return true;
	}

private:
	DurableStore &store_;
	bool commitSucceeds_;
	std::map<kv::Key, std::optional<kv::Value>> writes_;
};

}  // namespace

// The pending-update backlog is a health signal: crossing the injected high-watermark flips
// isBacklogCritical() and logs an escalation; growth within a step does not re-log; the next step
// re-logs; draining below the low-watermark clears the state; and after recovery the signal
// re-arms. A no-op engine lets the flush drain without a real FDB cluster.
TEST(MetadataWriterFDBBacklog, SignalEscalatesAndRecovers) {
	NoopKVEngine engine;
	constexpr size_t kHighWatermark = 8;  // step == high-watermark, low-watermark == 4
	MetadataWriterFDB writer(&engine, /*checkpointManager=*/nullptr,
	                         MetadataWriterFDB::WriterMode::kSynchronous, kHighWatermark);

	auto enqueueN = [&](size_t count) {
		for (size_t i = 0; i < count; ++i) { writer.enqueue(makeEvent(i)); }
	};

	// Below the high-watermark: not critical, nothing logged.
	enqueueN(kHighWatermark - 1);
	EXPECT_FALSE(writer.isBacklogCritical());
	EXPECT_EQ(writer.backlogEscalationCount(), 0U);

	// Crossing the high-watermark: critical, one escalation.
	enqueueN(1);  // depth == kHighWatermark
	EXPECT_TRUE(writer.isBacklogCritical());
	EXPECT_EQ(writer.backlogEscalationCount(), 1U);

	// Growing within the same step must not re-log.
	enqueueN(kHighWatermark - 1);  // depth in (high, 2*high)
	EXPECT_EQ(writer.backlogEscalationCount(), 1U);

	// Crossing the next step re-logs.
	enqueueN(1);  // depth == 2*high
	EXPECT_EQ(writer.backlogEscalationCount(), 2U);

	// Draining below the low-watermark clears the critical state.
	ASSERT_TRUE(writer.flush(MetadataWriterFDB::FlushMode::kDrainUntilEmpty));
	EXPECT_EQ(writer.pendingCount(), 0U);
	EXPECT_FALSE(writer.isBacklogCritical());

	// After recovery the signal re-arms: crossing again is a fresh escalation.
	enqueueN(kHighWatermark);
	EXPECT_TRUE(writer.isBacklogCritical());
	EXPECT_EQ(writer.backlogEscalationCount(), 3U);

	// Drain so the destructor's final flush has nothing to do.
	ASSERT_TRUE(writer.flush(MetadataWriterFDB::FlushMode::kDrainUntilEmpty));
	EXPECT_EQ(writer.pendingCount(), 0U);
}

// A transaction that never gets built leaves the pipeline empty, so the worker skips the reap
// block that normally notifies drainedCv_. flushAndWait() must still be released: its predicate is
// already satisfied (the failure sets lastFlushFailed_), and without a notification the checkpoint
// seal would park forever behind a failing FDB client while the worker retried in the background.
TEST(MetadataWriterFDBAsync, FlushAndWaitReturnsWhenTransactionBuildThrows) {
	ThrowingKVEngine engine;
	MetadataWriterFDB writer(&engine, /*checkpointManager=*/nullptr,
	                         MetadataWriterFDB::WriterMode::kAsync);

	writer.enqueue(makeEvent(0));

	std::promise<bool> outcome;
	auto released = outcome.get_future();
	std::thread sealer([&] { outcome.set_value(writer.flushAndWait()); });

	const bool returned = released.wait_for(std::chrono::seconds(5)) == std::future_status::ready;

	// Release the worker before joining, whether or not the wait succeeded: on a regression the
	// sealer is parked until something can commit, and joining it would hang the whole suite
	// instead of failing this one test.
	engine.stopThrowing();
	sealer.join();

	EXPECT_TRUE(returned) << "flushAndWait() was not released by the build-failure path; a "
	                         "checkpoint seal would hang while the FDB client is unavailable";
	if (returned) {
		EXPECT_FALSE(released.get()) << "a failed transaction build must be reported as a failed "
		                                "flush, not a successful drain";
	}
}

// A build failure must requeue its batch intact rather than drop it: the changelog is the
// durability record, but the FDB mirror still has to converge once the client recovers.
TEST(MetadataWriterFDBAsync, BuildFailureRequeuesEventsInsteadOfDroppingThem) {
	ThrowingKVEngine engine;
	MetadataWriterFDB writer(&engine, /*checkpointManager=*/nullptr,
	                         MetadataWriterFDB::WriterMode::kAsync);

	constexpr size_t kEvents = 16;
	for (size_t i = 0; i < kEvents; ++i) { writer.enqueue(makeEvent(i)); }

	// The blocked retry proves that the worker completed the failure path and requeued the batch.
	// Do not inspect pendingCount() here: the retry already owns the batch outside the pending
	// queue.
	ASSERT_TRUE(engine.waitForBlockedRetry(std::chrono::seconds(5)))
	    << "worker did not retry the batch after the transaction-build failure";
	EXPECT_EQ(engine.applied(), 0U) << "nothing may reach the backend while the build fails";

	// Once the client recovers, every event must land exactly once and in one drain.
	engine.stopThrowing();
	EXPECT_TRUE(writer.flushAndWait());
	EXPECT_EQ(writer.pendingCount(), 0U);
	EXPECT_EQ(engine.applied(), kEvents) << "events were lost or duplicated across the requeue";
}

TEST(MetadataWriterFDBAsync, DoesNotSubmitNewerBatchUntilOlderCommitCompletes) {
	BlockingKVEngine engine;
	MetadataWriterFDB writer(&engine, /*checkpointManager=*/nullptr,
	                         MetadataWriterFDB::WriterMode::kAsync);

	constexpr inode_t kInode = 42;
	writer.enqueue(std::make_unique<NodeRemoveEvent>(kInode));

	const bool firstCreated = engine.waitForTransactionCount(1, std::chrono::seconds(5));
	writer.enqueue(std::make_unique<NodeRemoveEvent>(kInode));
	engine.allowFirstTransaction();

	// With a pipeline depth greater than one, the worker creates the second transaction before
	// blocking in the first future's getResult(). The stabilized writer must reach getResult()
	// while the newer event is still queued.
	const bool firstCommitWaiting = engine.waitForCommitWaiter(0, std::chrono::seconds(5));
	const size_t transactionsBeforeRelease = engine.transactionCount();

	engine.releaseCommit(0);
	const bool secondCreated = engine.waitForTransactionCount(2, std::chrono::seconds(5));
	engine.releaseAllCommits();
	const bool drained = writer.flushAndWait();

	EXPECT_TRUE(firstCreated);
	EXPECT_TRUE(firstCommitWaiting);
	EXPECT_EQ(transactionsBeforeRelease, 1U)
	    << "a newer transaction was submitted before the older commit completed";
	EXPECT_TRUE(secondCreated);
	EXPECT_TRUE(drained);
	EXPECT_EQ(writer.pendingCount(), 0U);
}

TEST(MetadataWriterFDBAsync, FlushDoesNotCommitAlongsideWorker) {
	BlockingKVEngine engine;
	MetadataWriterFDB writer(&engine, /*checkpointManager=*/nullptr,
	                         MetadataWriterFDB::WriterMode::kAsync);

	writer.enqueue(makeEvent(0));
	const bool firstCreated = engine.waitForTransactionCount(1, std::chrono::seconds(5));
	writer.enqueue(makeEvent(1));

	std::promise<bool> outcome;
	auto flushResult = outcome.get_future();
	std::thread flusher([&] { outcome.set_value(writer.flush()); });

	// The first transaction factory is deliberately blocked. A direct synchronous flush would
	// create and commit the newer transaction here; delegation to the worker leaves it queued.
	const bool newerTransactionCreatedEarly =
	    engine.waitForTransactionCount(2, std::chrono::milliseconds(100));

	engine.allowFirstTransaction();
	const bool firstCommitWaiting = engine.waitForCommitWaiter(0, std::chrono::seconds(5));
	engine.releaseCommit(0);
	const bool secondCreated = engine.waitForTransactionCount(2, std::chrono::seconds(5));
	engine.releaseAllCommits();
	flusher.join();

	EXPECT_TRUE(firstCreated);
	EXPECT_FALSE(newerTransactionCreatedEarly)
	    << "flush() committed a newer batch alongside the worker's older transaction";
	EXPECT_TRUE(firstCommitWaiting);
	EXPECT_TRUE(secondCreated);
	EXPECT_TRUE(flushResult.get());
	EXPECT_EQ(writer.pendingCount(), 0U);
}

TEST(MetadataWriterFDBAsync, FlushWaitsThroughRetryableCommitFailure) {
	BlockingKVEngine engine;
	MetadataWriterFDB writer(&engine, /*checkpointManager=*/nullptr,
	                         MetadataWriterFDB::WriterMode::kAsync);

	writer.enqueue(makeEvent(0));
	const bool firstCreated = engine.waitForTransactionCount(1, std::chrono::seconds(5));

	std::promise<bool> outcome;
	auto flushResult = outcome.get_future();
	std::thread flusher([&] { outcome.set_value(writer.flushAndWait()); });

	// Keep the first commit blocked long enough for flushAndWait() to install its barrier, then
	// fail it with a transient conflict. The worker must replay the event and keep the barrier
	// pending until that retry becomes durable.
	const bool initiallyBlocked =
	    flushResult.wait_for(std::chrono::milliseconds(100)) == std::future_status::timeout;
	engine.allowFirstTransaction();
	const bool firstCommitWaiting = engine.waitForCommitWaiter(0, std::chrono::seconds(5));
	engine.failCommit(0, /*retryable=*/true);
	const bool retryCreated = engine.waitForTransactionCount(2, std::chrono::seconds(5));
	const bool waitedForRetry =
	    flushResult.wait_for(std::chrono::milliseconds(100)) == std::future_status::timeout;
	const bool retryCommitWaiting = engine.waitForCommitWaiter(1, std::chrono::seconds(5));
	engine.releaseCommit(1);
	const bool returned =
	    flushResult.wait_for(std::chrono::seconds(5)) == std::future_status::ready;

	engine.releaseAllCommits();
	flusher.join();

	EXPECT_TRUE(firstCreated);
	EXPECT_TRUE(initiallyBlocked);
	EXPECT_TRUE(firstCommitWaiting);
	EXPECT_TRUE(retryCreated);
	EXPECT_TRUE(waitedForRetry) << "a retryable conflict prematurely failed the flush barrier";
	EXPECT_TRUE(retryCommitWaiting);
	EXPECT_TRUE(returned);
	EXPECT_TRUE(flushResult.get());
	EXPECT_EQ(writer.pendingCount(), 0U);
}

TEST(MetadataWriterFDBAsync, FlushFailsAfterRetryBudgetIsExhausted) {
	BlockingKVEngine engine;
	MetadataWriterFDB writer(&engine, /*checkpointManager=*/nullptr,
	                         MetadataWriterFDB::WriterMode::kAsync);

	writer.enqueue(makeEvent(0));
	const bool firstCreated = engine.waitForTransactionCount(1, std::chrono::seconds(5));

	std::promise<bool> outcome;
	auto flushResult = outcome.get_future();
	std::thread flusher([&] { outcome.set_value(writer.flushAndWait()); });
	const bool initiallyBlocked =
	    flushResult.wait_for(std::chrono::milliseconds(100)) == std::future_status::timeout;
	engine.allowFirstTransaction();

	constexpr size_t kAttemptsThroughRetryBudget = 6;  // initial attempt plus five retries
	bool allAttemptsObserved = firstCreated;
	bool returnedBeforeBudget = false;
	for (size_t attempt = 0; attempt < kAttemptsThroughRetryBudget; ++attempt) {
		allAttemptsObserved =
		    allAttemptsObserved && engine.waitForCommitWaiter(attempt, std::chrono::seconds(5));
		engine.failCommit(attempt, /*retryable=*/true);
		if (attempt + 1 < kAttemptsThroughRetryBudget) {
			allAttemptsObserved = allAttemptsObserved && engine.waitForTransactionCount(
			                                                 attempt + 2, std::chrono::seconds(5));
			returnedBeforeBudget =
			    returnedBeforeBudget ||
			    flushResult.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
		}
	}

	const bool returnedAfterBudget =
	    flushResult.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
	engine.releaseAllCommits();
	flusher.join();

	EXPECT_TRUE(initiallyBlocked);
	EXPECT_TRUE(allAttemptsObserved);
	EXPECT_FALSE(returnedBeforeBudget);
	EXPECT_TRUE(returnedAfterBudget);
	EXPECT_FALSE(flushResult.get());
}

TEST(MetadataWriterFDBAsync, FlushReportsNonRetryableCommitFailure) {
	BlockingKVEngine engine;
	MetadataWriterFDB writer(&engine, /*checkpointManager=*/nullptr,
	                         MetadataWriterFDB::WriterMode::kAsync);

	writer.enqueue(makeEvent(0));
	const bool firstCreated = engine.waitForTransactionCount(1, std::chrono::seconds(5));

	std::promise<bool> outcome;
	auto flushResult = outcome.get_future();
	std::thread flusher([&] { outcome.set_value(writer.flushAndWait()); });
	const bool initiallyBlocked =
	    flushResult.wait_for(std::chrono::milliseconds(100)) == std::future_status::timeout;
	engine.allowFirstTransaction();
	const bool commitWaiting = engine.waitForCommitWaiter(0, std::chrono::seconds(5));
	engine.failCommit(0, /*retryable=*/false);
	const bool returned =
	    flushResult.wait_for(std::chrono::seconds(5)) == std::future_status::ready;

	engine.releaseAllCommits();
	flusher.join();

	EXPECT_TRUE(firstCreated);
	EXPECT_TRUE(initiallyBlocked);
	EXPECT_TRUE(commitWaiting);
	EXPECT_TRUE(returned);
	EXPECT_FALSE(flushResult.get());
}

TEST(MetadataUndoRecorder, FailedFirstTouchRetryPreservesOriginalNodePreimage) {
	NoopKVEngine engine;
	NodeUndoRecorder recorder(&engine);

	constexpr uint64_t kCheckpointVersion = 17;
	constexpr inode_t kInode = 42;
	const kv::Key liveKey = kv::encodeKeyBE(kNodeKeyPrefix, kInode);
	const kv::Key undoKey = kv::encodeKeyBE(kNodeUndoKeyPrefix, kCheckpointVersion, kInode);
	const kv::Value originalValue{0x01, 0x02, 0x03};
	const kv::Value updatedValue{0x04, 0x05, 0x06};

	DurableStore store{{liveKey, originalValue}};
	const MetadataMutation mutation = NodeSetMutation{
	    .inode = kInode,
	    .liveKey = liveKey,
	};

	{
		RecordingTransaction failedTransaction(store, /*commitSucceeds=*/false);
		recorder.beforeMutation(
		    MetadataMutationContext{
		        .transaction = &failedTransaction,
		        .checkpointVersion = kCheckpointVersion,
		    },
		    mutation);
		failedTransaction.set(liveKey, updatedValue);
		EXPECT_FALSE(failedTransaction.commit());
	}

	EXPECT_EQ(store.count(undoKey), 0U);
	ASSERT_EQ(store.at(liveKey), originalValue);

	{
		RecordingTransaction retryTransaction(store, /*commitSucceeds=*/true);
		recorder.beforeMutation(
		    MetadataMutationContext{
		        .transaction = &retryTransaction,
		        .checkpointVersion = kCheckpointVersion,
		    },
		    mutation);
		retryTransaction.set(liveKey, updatedValue);
		ASSERT_TRUE(retryTransaction.commit());
	}

	ASSERT_EQ(store.count(undoKey), 1U);
	EXPECT_EQ(store.at(undoKey), originalValue);
	EXPECT_EQ(store.at(liveKey), updatedValue);
}
