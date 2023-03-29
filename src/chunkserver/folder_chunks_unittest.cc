#include "chunkserver/folder_chunks.h"

#include <bitset>
#include <numeric>

#include <gtest/gtest.h>

#include "chunkserver/chunk.h"
#include "common/slice_traits.h"

class FolderChunksTest : public ::testing::Test {
public:
	void SetUp() override {
		chunks_.reserve(100);
		for (size_t i = 0; i < 100; ++i) {
			chunks_.push_back(InterleavedChunk(i,
			                                   slice_traits::standard::ChunkPartType(), ChunkState::CH_AVAIL));
		}
	}
protected:
	std::vector<InterleavedChunk> chunks_;
};

TEST_F(FolderChunksTest, AddRemoveMarkAsTested) {
	FolderChunks folderChunks;
	for (size_t index = 0; index < 100; ++index) {
		EXPECT_EQ(FolderChunks::kInvalidIndex, chunks_[index].indexInFolder);
		folderChunks.insert(&chunks_[index]);
		EXPECT_NE(FolderChunks::kInvalidIndex, chunks_[index].indexInFolder);
	}

	folderChunks.shuffle();

	for (size_t index : {0, 30, 60, 95, 15, 30, 95}) {
		folderChunks.markAsTested(&chunks_[index]);
	}

	std::vector<size_t> toRemove = {0, 50, 3, 15, 11, 99, 4, 95};
	for (size_t index : toRemove) {
		folderChunks.remove(&chunks_[index]);
		EXPECT_EQ(FolderChunks::kInvalidIndex, chunks_[index].indexInFolder);
	}

	for (size_t index = 0; index < chunks_.size(); ++index) {
		if (std::count(toRemove.begin(), toRemove.end(), index) > 0) {
			ASSERT_EQ(FolderChunks::kInvalidIndex, chunks_[index].indexInFolder);
		} else {
			ASSERT_LT(chunks_[index].indexInFolder, 92U);
		}
	}

	ASSERT_EQ(92U, folderChunks.size());
}

TEST_F(FolderChunksTest, TestOrder) {
	// This verifies that chunks which are marked as "tested" are tested later than untested chunks.
	FolderChunks folderChunks;
	for (size_t index = 0; index < 100; ++index) {
		folderChunks.insert(&chunks_[index]);
	}

	folderChunks.shuffle();

	// Mark every third chunk as tested. This results in 34 tested and 66 untested chunks.
	for (size_t index = 0; index <= 99; index += 3) {
		folderChunks.markAsTested(&chunks_[index]);
	}

	std::bitset<100> testedChunks;
	for (size_t count = 0; count < 100; ++count) {
		Chunk* chunk = folderChunks.chunkToTest();
		SCOPED_TRACE("As the " + std::to_string(count) + " chunk, "
		                                                 "testing " + std::to_string(chunk->chunkid));
		folderChunks.markAsTested(chunk);
		ASSERT_FALSE(testedChunks[chunk->chunkid]) << (chunk->chunkid) << " already tested!";
		testedChunks.set(chunk->chunkid);
		if (count < 66U) {
			ASSERT_NE(0U, chunk->chunkid % 3);
		} else {
			ASSERT_EQ(0U, chunk->chunkid % 3);
		}
	}

	ASSERT_EQ(100U, testedChunks.count());
}

TEST_F(FolderChunksTest, TestOrderInSubsequentLoops) {
	// We verify that in absence of other operations, chunks
	// are tested in the same order in subsequent loops.
	static const size_t kCount = 10;
	FolderChunks folderChunks;
	for (size_t index = 0; index < kCount; ++index) {
		folderChunks.insert(&chunks_[index]);
	}

	auto getTestingSequence = [&folderChunks]() {
		std::vector<uint64_t> testingSequence;
		for (size_t i = 0; i < kCount; ++i) {
			Chunk* c = folderChunks.chunkToTest();
			folderChunks.markAsTested(c);
			testingSequence.push_back(c->chunkid);
		}
		return testingSequence;
	};

	folderChunks.shuffle();

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
		ASSERT_EQ(nextSequence, firstSequence) << "Testing sequence not equal in loop" << loop;
	}
}

TEST_F(FolderChunksTest, CornerCases) {
	FolderChunks folderChunks;
	folderChunks.insert(&chunks_[0]);
	folderChunks.shuffle();
	ASSERT_EQ(&chunks_[0], folderChunks.chunkToTest());
	ASSERT_EQ(&chunks_[0], folderChunks.getRandomChunk());
	folderChunks.remove(&chunks_[0]);
	ASSERT_EQ(nullptr, folderChunks.chunkToTest());
	ASSERT_EQ(nullptr, folderChunks.getRandomChunk());

	folderChunks.insert(&chunks_[0]);
	folderChunks.shuffle();
	folderChunks.markAsTested(&chunks_[0]);
	ASSERT_EQ(&chunks_[0], folderChunks.chunkToTest());
	ASSERT_EQ(&chunks_[0], folderChunks.getRandomChunk());
	folderChunks.remove(&chunks_[0]);
	ASSERT_EQ(nullptr, folderChunks.chunkToTest());
	ASSERT_EQ(nullptr, folderChunks.getRandomChunk());

	folderChunks.insert(&chunks_[0]);
	folderChunks.insert(&chunks_[1]);
	folderChunks.shuffle();
	folderChunks.markAsTested(&chunks_[0]);
	folderChunks.remove(&chunks_[1]);
	folderChunks.remove(&chunks_[0]);
	ASSERT_EQ(nullptr, folderChunks.chunkToTest());
	ASSERT_EQ(nullptr, folderChunks.getRandomChunk());
}
