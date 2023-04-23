#include <bitset>
#include <numeric>

#include <gtest/gtest.h>

#include "chunkserver-common/disk_chunks.h"
#include "chunkserver/cmr_chunk.h"
#include "common/slice_traits.h"

class DiskChunksTest : public ::testing::Test {
public:
	void SetUp() override {
		for (size_t i = 0; i < 100; ++i) {
			chunks_.emplace_back(
			    new CmrChunk(i,
			             slice_traits::standard::ChunkPartType(),
			             ChunkState::Available));
		}
	}
protected:
	std::vector<IChunk*> chunks_;
};

TEST_F(DiskChunksTest, AddRemoveMarkAsTested) {
	DiskChunks diskChunks;
	for (size_t index = 0; index < 100; ++index) {
		EXPECT_EQ(DiskChunks::kInvalidIndex, chunks_[index]->indexInDisk());
		diskChunks.insert(chunks_[index]);
		EXPECT_NE(DiskChunks::kInvalidIndex, chunks_[index]->indexInDisk());
	}

	diskChunks.shuffle();

	for (size_t index : {0, 30, 60, 95, 15, 30, 95}) {
		diskChunks.markAsTested(chunks_[index]);
	}

	std::vector<size_t> toRemove = {0, 50, 3, 15, 11, 99, 4, 95};
	for (const size_t index : toRemove) {
		diskChunks.remove(chunks_[index]);
		EXPECT_EQ(DiskChunks::kInvalidIndex, chunks_[index]->indexInDisk());
	}

	for (size_t index = 0; index < chunks_.size(); ++index) {
		if (std::count(toRemove.begin(), toRemove.end(), index) > 0) {
			ASSERT_EQ(DiskChunks::kInvalidIndex, chunks_[index]->indexInDisk());
		} else {
			ASSERT_LT(chunks_[index]->indexInDisk(), 92U);
		}
	}

	ASSERT_EQ(92U, diskChunks.size());
}

TEST_F(DiskChunksTest, TestOrder) {
	// This verifies that chunks which are marked as "tested" are tested later
	// than untested chunks.
	DiskChunks diskChunks;
	for (size_t index = 0; index < 100; ++index) {
		diskChunks.insert(chunks_[index]);
	}

	diskChunks.shuffle();

	// Mark every third chunk as tested. This results in 34 tested and 66
	// untested chunks.
	for (size_t index = 0; index <= 99; index += 3) {
		diskChunks.markAsTested(chunks_[index]);
	}

	std::bitset<100> testedChunks;
	for (size_t count = 0; count < 100; ++count) {
		IChunk* chunk = diskChunks.chunkToTest();
		SCOPED_TRACE("As the " + std::to_string(count) + " chunk, testing "
		             + std::to_string(chunk->id()));
		diskChunks.markAsTested(chunk);
		ASSERT_FALSE(testedChunks[chunk->id()])
		    << (chunk->id()) << " already tested!";
		testedChunks.set(chunk->id());
		if (count < 66U) {
			ASSERT_NE(0U, chunk->id() % 3);
		} else {
			ASSERT_EQ(0U, chunk->id() % 3);
		}
	}

	ASSERT_EQ(100U, testedChunks.count());
}

TEST_F(DiskChunksTest, TestOrderInSubsequentLoops) {
	// We verify that in absence of other operations, chunks
	// are tested in the same order in subsequent loops.
	static const size_t kCount = 10;
	DiskChunks diskChunks;
	for (size_t index = 0; index < kCount; ++index) {
		diskChunks.insert(chunks_[index]);
	}

	auto getTestingSequence = [&diskChunks]() {
		std::vector<uint64_t> testingSequence;
		for (size_t i = 0; i < kCount; ++i) {
			IChunk* chunk = diskChunks.chunkToTest();
			diskChunks.markAsTested(chunk);
			testingSequence.push_back(chunk->id());
		}
		return testingSequence;
	};

	diskChunks.shuffle();

	auto firstSequence = getTestingSequence();

	// Verify if all chunks are tested exactly once.
	auto firstSequenceSorted = firstSequence;
	std::sort(firstSequenceSorted.begin(), firstSequenceSorted.end());
	auto integers = std::vector<uint64_t>(kCount);
	std::iota(integers.begin(), integers.end(), 0);
	ASSERT_EQ(integers, firstSequenceSorted);

	// Verify that subsequent loops repeat the same sequence.
	for (size_t loop = 0; loop < 10; ++loop) {
		auto nextSequence = getTestingSequence();
		ASSERT_EQ(nextSequence, firstSequence) << "Testing sequence not equal "
		                                          "in loop" << loop;
	}
}

TEST_F(DiskChunksTest, CornerCases) {
	DiskChunks diskChunks;
	diskChunks.insert(chunks_[0]);
	diskChunks.shuffle();
	ASSERT_EQ(chunks_[0], diskChunks.chunkToTest());
	ASSERT_EQ(chunks_[0], diskChunks.getRandomChunk());
	diskChunks.remove(chunks_[0]);
	ASSERT_EQ(nullptr, diskChunks.chunkToTest());
	ASSERT_EQ(nullptr, diskChunks.getRandomChunk());

	diskChunks.insert(chunks_[0]);
	diskChunks.shuffle();
	diskChunks.markAsTested(chunks_[0]);
	ASSERT_EQ(chunks_[0], diskChunks.chunkToTest());
	ASSERT_EQ(chunks_[0], diskChunks.getRandomChunk());
	diskChunks.remove(chunks_[0]);
	ASSERT_EQ(nullptr, diskChunks.chunkToTest());
	ASSERT_EQ(nullptr, diskChunks.getRandomChunk());

	diskChunks.insert(chunks_[0]);
	diskChunks.insert(chunks_[1]);
	diskChunks.shuffle();
	diskChunks.markAsTested(chunks_[0]);
	diskChunks.remove(chunks_[1]);
	diskChunks.remove(chunks_[0]);
	ASSERT_EQ(nullptr, diskChunks.chunkToTest());
	ASSERT_EQ(nullptr, diskChunks.getRandomChunk());
}
