/*
   Copyright 2026 Leil Storage

   This file is part of SaunaFS.

   SaunaFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   SaunaFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with SaunaFS. If not, see <http://www.gnu.org/licenses/>.
 */

#include "common/platform.h"

#include <gtest/gtest.h>
#include <array>
#include <atomic>
#include <sys/resource.h>
#include <unistd.h>
#include <cstdio>
#include <latch>
#include <memory>
#include <vector>

#include "chunkserver-common/hdd_utils.h"
#include "chunkserver/bgjobs.h"
#include "chunkserver/hddspacemgr.h"
#include "chunkserver/master_connection.h"
#include "chunkserver/masterconn.h"
#include "protocol/cstoma.h"
#include "protocol/matocs.h"
#include "unittests/packet.h"

TEST(MasterConnectionTests, DropsReplyFromPreviousSocket) {
	MasterConn connection("localhost", "9420", "default", nullptr, nullptr);
	connection.setMode(ConnectionMode::CONNECTED);
	auto callback = MasterConn::sauJobFinished(&connection);
	auto *packet = new OutputPacket;
	cstoma::createChunk::serialize(packet->packet, 123, slice_traits::standard::ChunkPartType(),
	                               SAUNAFS_STATUS_OK);

	connection.setMode(ConnectionMode::KILL);
	connection.setMode(ConnectionMode::CONNECTED);
	callback(SAUNAFS_STATUS_OK, packet);
	EXPECT_TRUE(connection.isOutputQueueEmpty());
}

TEST(MasterConnectionTests, DropsLockReplyFromPreviousSocket) {
	MasterConn connection("localhost", "9420", "default", nullptr, nullptr);
	connection.setMode(ConnectionMode::CONNECTED);
	auto callback = MasterConn::sauJobFinishedAndLock(&connection, 123,
	                                                  slice_traits::standard::ChunkPartType());
	auto *packet = new OutputPacket;
	cstoma::createChunk::serialize(packet->packet, 123, slice_traits::standard::ChunkPartType(),
	                               SAUNAFS_STATUS_OK);

	connection.setMode(ConnectionMode::KILL);
	connection.setMode(ConnectionMode::CONNECTED);
	callback(SAUNAFS_ERROR_NOTDONE, packet);
	EXPECT_TRUE(connection.isOutputQueueEmpty());
}

TEST(MasterConnectionTests, DestroyedConnectionDisarmsPendingReplies) {
	// masterconn_term releases the connection first and the job pools second. Dropping the last
	// pool reference runs the destructor, which flushes any status still queued into the callback
	// that was owed the reply. That callback must not reach the connection that no longer exists.
	//
	// This is a sanitizer reproducer and it asserts nothing on a plain build: the fault it guards
	// is a read of freed memory, which only a sanitizer turns into a failure. Run it under the
	// asan preset to gate the behaviour; a green plain build says only that it did not crash.
	std::vector<int> descriptors;
	auto pool = std::make_shared<MasterJobPool>("term-test", 1, 10, 1, descriptors);
	auto connection = std::make_unique<MasterConn>("localhost", "9420", "default", pool, nullptr);
	connection->setMode(ConnectionMode::CONNECTED);

	auto callback = MasterConn::sauJobFinished(connection.get());
	auto *packet = new OutputPacket;
	cstoma::createChunk::serialize(packet->packet, 123, slice_traits::standard::ChunkPartType(),
	                               SAUNAFS_STATUS_OK);

	connection.reset();
	callback(SAUNAFS_STATUS_OK, packet);
}

TEST(MasterConnectionTests, DiscoveredPeerCannotSelectInventory) {
	MasterConn connection("localhost", "9420", "default", nullptr, nullptr, 1, 42);
	connection.setMode(ConnectionMode::CONNECTED);
	auto packet = matocs::registerHost::build(SAUNAFS_STATUS_OK, SAUNAFS_VERSHEX, "default");
	removeHeaderInPlace(packet);
	connection.onRegistered(packet);
	EXPECT_EQ(connection.mode(), ConnectionMode::KILL);
	EXPECT_TRUE(connection.isOutputQueueEmpty());
}

TEST(MasterConnectionTests, LockJobsUseOriginatingListener) {
	std::vector<int> descriptors(2);
	auto pool = std::make_shared<MasterJobPool>("connection-test", 1, 10, 2, descriptors);
	MasterConn connection("localhost", "9420", "default", pool, nullptr, 1, 42);
	connection.setMode(ConnectionMode::CONNECTED);
	auto packet = matocs::chunkLock::build(123, slice_traits::standard::ChunkPartType());
	removeHeaderInPlace(packet);
	connection.lockChunk(packet);
	EXPECT_TRUE(pool->isListenerIdle(0));
	EXPECT_FALSE(pool->isListenerIdle(1));
	pool->eraseChunkLock(123, slice_traits::standard::ChunkPartType());
	EXPECT_TRUE(pool->isListenerIdle(1));
}

TEST(MasterConnectionTests, RetiredListenerWaitsForDeferredJob) {
	std::vector<int> descriptors(2);
	MasterJobPool pool("connection-test", 1, 10, 2, descriptors);
	const auto chunkType = slice_traits::standard::ChunkPartType();
	ASSERT_TRUE(pool.startChunkLock({}, nullptr, 123, chunkType, 0));
	ASSERT_TRUE(pool.enforceChunkLock(123, chunkType));
	pool.addJobIfNotLocked(
	    {123, chunkType}, JobPool::ChunkOperation::Read, {}, nullptr,
	    []() -> uint8_t { return SAUNAFS_STATUS_OK; }, 1);
	EXPECT_FALSE(pool.isListenerIdle(1));

	pool.eraseChunkLock(123, chunkType);
	pollfd descriptor{descriptors[1], POLLIN, 0};
	ASSERT_EQ(poll(&descriptor, 1, 1000), 1);
	pool.processCompletedJobs(1);
	EXPECT_TRUE(pool.isListenerIdle(1));
}

namespace {

// Keeps a worker inside the job until the test releases it, so the job stays in flight.
struct HeldJob {
	std::latch started{1};
	std::latch release{1};

	void releaseWorker() {
		if (!release.try_wait()) { release.count_down(); }
	}
};

// Releases every held worker when an assertion aborts the test, so pool shutdown can join them.
struct HeldJobs {
	std::vector<std::shared_ptr<HeldJob>> jobs;

	~HeldJobs() {
		for (auto &job : jobs) { job->releaseWorker(); }
	}

	std::shared_ptr<HeldJob> add() { return jobs.emplace_back(std::make_shared<HeldJob>()); }
};

uint32_t addHeldJob(MasterJobPool &pool, const std::shared_ptr<HeldJob> &held,
                    JobPool::JobCallback callback, uint32_t listenerId) {
	auto *packet = new OutputPacket;
	cstoma::createChunk::serialize(packet->packet, 100 + listenerId,
	                               slice_traits::standard::ChunkPartType(), SAUNAFS_STATUS_OK);
	return pool.addJob(
	    JobPool::ChunkOperation::Create, std::move(callback), packet,
	    [held]() -> uint8_t {
		    held->started.count_down();
		    held->release.wait();
		    return SAUNAFS_STATUS_OK;
	    },
	    listenerId);
}

void awaitCompletion(int descriptor) {
	pollfd pending{descriptor, POLLIN, 0};
	ASSERT_EQ(poll(&pending, 1, 5000), 1);
}

// Runs in a child process so a lowered descriptor limit cannot affect other test suites.
void checkAdmissionFailure(bool failSecondPool) {
	std::vector<int> jobDescriptors;
	std::vector<int> replicationDescriptors;
	auto jobPool = std::make_shared<MasterJobPool>("admission", 1, 10, 1, jobDescriptors);
	auto replicationPool =
	    std::make_shared<MasterJobPool>("admission-repl", 1, 10, 1, replicationDescriptors);
	MasterConn survivor("localhost", "9420", "default", jobPool, replicationPool);
	survivor.setMode(ConnectionMode::CONNECTED);
	auto completions = std::make_shared<int>(0);
	HeldJobs held;
	auto running = held.add();
	addHeldJob(
	    *jobPool, running,
	    [completions, finish = MasterConn::sauJobFinished(&survivor)](uint8_t status,
	                                                                  void *packet) {
		    ++*completions;
		    EXPECT_EQ(status, SAUNAFS_STATUS_OK);
		    finish(status, packet);
	    },
	    0);
	running->started.wait();
	if (failSecondPool) { ASSERT_GE(jobPool->allocateListener(1), 0); }

	struct rlimit original;
	ASSERT_EQ(getrlimit(RLIMIT_NOFILE, &original), 0);
	struct rlimit exhausted = original;
	exhausted.rlim_cur = 0;
	ASSERT_EQ(setrlimit(RLIMIT_NOFILE, &exhausted), 0);
	int jobDescriptor = -1;
	int replicationDescriptor = -1;
	bool admitted = true;
	bool threw = false;
	try {
		admitted = masterconn_prepare_listeners(*jobPool, *replicationPool, 1, jobDescriptor,
		                                        replicationDescriptor);
	} catch (const std::exception &exception) {
		threw = true;
		std::fprintf(stderr, "admission exception escaped: %s\n", exception.what());
	}
	ASSERT_EQ(setrlimit(RLIMIT_NOFILE, &original), 0);
	EXPECT_FALSE(threw);
	EXPECT_FALSE(admitted);
	EXPECT_EQ(jobDescriptor, -1);
	EXPECT_EQ(replicationDescriptor, -1);
	EXPECT_EQ(survivor.mode(), ConnectionMode::CONNECTED);
	EXPECT_EQ(jobPool->allocatedListenerCount(), failSecondPool ? 2U : 1U);
	EXPECT_EQ(replicationPool->allocatedListenerCount(), 1U);

	running->releaseWorker();
	ASSERT_NO_FATAL_FAILURE(awaitCompletion(jobDescriptors[0]));
	jobPool->processCompletedJobs(0);
	EXPECT_EQ(*completions, 1);
	EXPECT_FALSE(survivor.isOutputQueueEmpty());
	EXPECT_TRUE(jobPool->isListenerIdle(0));
	ASSERT_TRUE(masterconn_prepare_listeners(*jobPool, *replicationPool, 1, jobDescriptor,
	                                         replicationDescriptor));
	EXPECT_GE(jobDescriptor, 0);
	EXPECT_GE(replicationDescriptor, 0);
	EXPECT_EQ(jobPool->allocatedListenerCount(), 2U);
	EXPECT_EQ(replicationPool->allocatedListenerCount(), 2U);
}

}  // namespace

TEST(MasterConnectionDeathTest, FirstListenerFailureKeepsExistingJobsAlive) {
	ASSERT_EXIT(
	    {
		    checkAdmissionFailure(false);
		    _exit(::testing::Test::HasFailure() ? 1 : 0);
	    },
	    ::testing::ExitedWithCode(0), "");
}

TEST(MasterConnectionDeathTest, SecondListenerFailureKeepsExistingJobsAlive) {
	ASSERT_EXIT(
	    {
		    checkAdmissionFailure(true);
		    _exit(::testing::Test::HasFailure() ? 1 : 0);
	    },
	    ::testing::ExitedWithCode(0), "");
}

TEST(MasterConnectionTests, ListenerTablePublishesToRunningWorkers) {
	// The listener table is a table of atomic pointers: the event loop publishes a new entry with
	// release semantics in allocateListener, and worker threads load the entry of the job they
	// picked up with acquire semantics. This drives both sides at once, one thread allocating and
	// submitting exactly as the event loop does while four workers drain, so a thread sanitizer
	// sees the pair. It asserts only that every job completes on the listener that owns it.
	constexpr uint32_t kListeners = 8;
	constexpr int kJobsPerListener = 50;

	std::vector<int> descriptors;
	MasterJobPool pool("race-test", 4, 1000, 1, descriptors);
	ASSERT_FALSE(descriptors.empty());

	std::vector<int> notifiers{descriptors[0]};
	std::array<std::atomic<int>, kListeners> completed{};

	for (uint32_t listener = 0; listener < kListeners; ++listener) {
		if (listener > 0) {
			const int notifier = pool.allocateListener(listener);
			ASSERT_GE(notifier, 0);
			notifiers.push_back(notifier);
		}
		for (int job = 0; job < kJobsPerListener; ++job) {
			pool.addJob(
			    JobPool::ChunkOperation::Read,
			    [&completed, listener](uint8_t /*status*/, void * /*extra*/) {
				    completed[listener].fetch_add(1, std::memory_order_relaxed);
			    },
			    nullptr, []() -> uint8_t { return SAUNAFS_STATUS_OK; }, listener);
		}
	}

	// Drain the way the event loop does: poll each notifier, then process that listener only.
	for (int pass = 0; pass < 2000; ++pass) {
		bool allIdle = true;
		for (uint32_t listener = 0; listener < kListeners; ++listener) {
			pollfd descriptor{notifiers[listener], POLLIN, 0};
			if (poll(&descriptor, 1, 10) == 1) { pool.processCompletedJobs(listener); }
			if (!pool.isListenerIdle(listener)) { allIdle = false; }
		}
		if (allIdle) { break; }
	}

	for (uint32_t listener = 0; listener < kListeners; ++listener) {
		EXPECT_TRUE(pool.isListenerIdle(listener)) << "listener " << listener;
		EXPECT_EQ(completed[listener].load(), kJobsPerListener) << "listener " << listener;
	}
}

TEST(MasterConnectionTests, DisconnectLeavesOtherListenerJobInFlight) {
	int survivorCompletions = 0;
	uint8_t survivorStatus = SAUNAFS_ERROR_NOTDONE;
	std::vector<int> jobDescriptors(2);
	std::vector<int> replicationDescriptors(2);
	auto jobPool = std::make_shared<MasterJobPool>("isolation-test", 2, 10, 2, jobDescriptors);
	auto replicationPool =
	    std::make_shared<MasterJobPool>("isolation-repl", 1, 10, 2, replicationDescriptors);
	MasterConn survivor("localhost", "9420", "default", jobPool, replicationPool);
	MasterConn dropped("localhost", "9421", "default", jobPool, replicationPool, 1, 42);
	survivor.setMode(ConnectionMode::CONNECTED);
	dropped.setMode(ConnectionMode::CONNECTED);
	HeldJobs held;

	// Both workers are inside a job before the disconnect, one job per listener.
	auto droppedJob = held.add();
	auto survivorJob = held.add();
	addHeldJob(*jobPool, droppedJob, MasterConn::sauJobFinished(&dropped), 1);
	auto countingFinish = [&survivorCompletions, &survivorStatus,
	                       finish = MasterConn::sauJobFinished(&survivor)](uint8_t status,
	                                                                       void *packet) {
		++survivorCompletions;
		survivorStatus = status;
		finish(status, packet);
	};
	addHeldJob(*jobPool, survivorJob, countingFinish, 0);
	droppedJob->started.wait();
	survivorJob->started.wait();

	dropped.setMode(ConnectionMode::KILL);
	masterconn_close_connection(*jobPool, *replicationPool, dropped, 1);
	ASSERT_EQ(dropped.mode(), ConnectionMode::FREE);

	// The surviving job completes once, on its own connection, with its real status.
	survivorJob->release.count_down();
	ASSERT_NO_FATAL_FAILURE(awaitCompletion(jobDescriptors[0]));
	jobPool->processCompletedJobs(0);
	EXPECT_EQ(survivorCompletions, 1);
	EXPECT_EQ(survivorStatus, SAUNAFS_STATUS_OK);
	EXPECT_FALSE(survivor.isOutputQueueEmpty());
	EXPECT_TRUE(dropped.isOutputQueueEmpty());
	EXPECT_TRUE(jobPool->isListenerIdle(0));
	EXPECT_FALSE(jobPool->isListenerIdle(1));

	// The dropped connection reconnects before its late completion, which must be discarded.
	dropped.setMode(ConnectionMode::CONNECTED);
	droppedJob->release.count_down();
	ASSERT_NO_FATAL_FAILURE(awaitCompletion(jobDescriptors[1]));
	jobPool->processCompletedJobs(1);
	EXPECT_TRUE(dropped.isOutputQueueEmpty());
	EXPECT_TRUE(jobPool->isListenerIdle(1));
	EXPECT_EQ(survivorCompletions, 1);
}

TEST(MasterConnectionTests, UnsentNegativeReportsSurviveDisconnect) {
	std::vector<int> jobDescriptors(1);
	std::vector<int> replicationDescriptors(1);
	auto jobPool = std::make_shared<MasterJobPool>("report-test", 1, 10, 1, jobDescriptors);
	auto replicationPool =
	    std::make_shared<MasterJobPool>("report-repl", 1, 10, 1, replicationDescriptors);
	MasterConn connection("localhost", "9420", "default", jobPool, replicationPool);
	connection.setMode(ConnectionMode::CONNECTED);
	connection.setSendInventory(false);
	const auto chunkType = slice_traits::standard::ChunkPartType();
	std::vector<ChunkWithType> reports;
	hddGetLostChunks(reports, 1000);
	hddGetDamagedChunks(reports, 1000);

	// Same queue-to-packet handoff as the report loop, before any socket write.
	hddReportLostChunk(701, chunkType);
	hddGetLostChunks(reports, 1);
	ASSERT_EQ(reports.size(), 1U);
	connection.createAttachedPacket(cstoma::chunkLost::build(reports));
	hddReportDamagedChunk(702, chunkType);
	hddGetDamagedChunks(reports, 1);
	ASSERT_EQ(reports.size(), 1U);
	connection.createAttachedPacket(cstoma::chunkDamaged::build(reports));
	ASSERT_FALSE(connection.isOutputQueueEmpty());

	connection.setMode(ConnectionMode::KILL);
	masterconn_close_connection(*jobPool, *replicationPool, connection, 0);
	connection.setMode(ConnectionMode::CONNECTED);

	// Without an inventory, only the report queues can carry the loss to the next connection.
	hddGetLostChunks(reports, 1);
	ASSERT_EQ(reports.size(), 1U);
	EXPECT_EQ(reports[0].id, 701U);
	hddGetDamagedChunks(reports, 1);
	ASSERT_EQ(reports.size(), 1U);
	EXPECT_EQ(reports[0].id, 702U);
}

TEST(MasterConnectionTests, UnsentReportsDiscardedWhenSendingInventory) {
	std::vector<int> jobDescriptors(1);
	std::vector<int> replicationDescriptors(1);
	auto jobPool = std::make_shared<MasterJobPool>("report-test", 1, 10, 1, jobDescriptors);
	auto replicationPool =
	    std::make_shared<MasterJobPool>("report-repl", 1, 10, 1, replicationDescriptors);
	MasterConn connection("localhost", "9420", "default", jobPool, replicationPool);
	connection.setMode(ConnectionMode::CONNECTED);
	ASSERT_TRUE(connection.sendsInventory());
	const auto chunkType = slice_traits::standard::ChunkPartType();
	std::vector<ChunkWithType> reports;
	hddGetLostChunks(reports, 1000);
	hddGetDamagedChunks(reports, 1000);

	hddReportLostChunk(703, chunkType);
	hddGetLostChunks(reports, 1);
	ASSERT_EQ(reports.size(), 1U);
	connection.createAttachedPacket(cstoma::chunkLost::build(reports));
	hddReportDamagedChunk(704, chunkType);
	hddGetDamagedChunks(reports, 1);
	ASSERT_EQ(reports.size(), 1U);
	connection.createAttachedPacket(cstoma::chunkDamaged::build(reports));
	ASSERT_FALSE(connection.isOutputQueueEmpty());

	connection.setMode(ConnectionMode::KILL);
	masterconn_close_connection(*jobPool, *replicationPool, connection, 0);

	// A connection that sends full inventory discards unconfirmed reports on disconnect.
	hddGetLostChunks(reports, 1);
	EXPECT_TRUE(reports.empty());
	hddGetDamagedChunks(reports, 1);
	EXPECT_TRUE(reports.empty());
}

TEST(MasterConnectionTests, LateDeleteCompletionAlwaysReportsTheLoss) {
	const auto chunkType = slice_traits::standard::ChunkPartType();
	std::vector<ChunkWithType> reports;
	hddGetLostChunks(reports, 1000);

	// The released build polled job completions only while connected, so this callback ran after
	// the reconnect and its report always reached the metadata server. These pools drain while
	// disconnected, so the report has to be queued whatever the connection is doing.
	auto completion = masterconn_jobDeleteAfterErrorFinished(ChunkWithType(903, chunkType));

	std::vector<int> jobDescriptors;
	std::vector<int> replicationDescriptors;
	MasterJobPool jobPool("late-delete", 1, 10, 1, jobDescriptors);
	MasterJobPool replicationPool("late-delete-repl", 1, 10, 1, replicationDescriptors);
	MasterConn connection("localhost", "9420", "default", nullptr, nullptr);
	connection.setMode(ConnectionMode::CONNECTED);
	connection.setMode(ConnectionMode::KILL);
	masterconn_close_connection(jobPool, replicationPool, connection, 0);

	for (const auto mode : {ConnectionMode::FREE, ConnectionMode::CONNECTING,
	                        ConnectionMode::HANDSHAKE, ConnectionMode::CONNECTED}) {
		connection.setMode(mode);
		completion(SAUNAFS_STATUS_OK, nullptr);
		hddGetLostChunks(reports, 1000);
		ASSERT_EQ(reports.size(), 1U);
		EXPECT_EQ(reports.front().id, 903U);
	}

	// A delete that failed leaves the copy where it is, so there is nothing to report.
	completion(SAUNAFS_ERROR_IO, nullptr);
	hddGetLostChunks(reports, 1000);
	EXPECT_TRUE(reports.empty());
}
