#include "chunkserver/folder_chunks.h"

#include "chunkserver/chunk.h"
#include "common/random.h"

const FolderChunks::Index FolderChunks::kInvalidIndex;

FolderChunks::FolderChunks() {
}

void FolderChunks::insert(Chunk* newChunk) {
	sassert(newChunk->indexInFolder == kInvalidIndex);
	chunks_.push_back(newChunk);
	newChunk->indexInFolder = chunks_.size() - 1;

	// We consider a new chunk to be tested, so we need to bring it into the "tested" section.
	markAsTestedInternal(newChunk);
}

void FolderChunks::remove(Chunk* removedChunk) {
	// First, we have to make sure that the removed chunk is in the untested section.
	if (removedChunk->indexInFolder < firstUntestedChunk_) {
		swap(removedChunk->indexInFolder, firstUntestedChunk_ - 1);
		--firstUntestedChunk_;
	}

	// We are now sure that the removed chunk is in the "untested" section.
	// We can just swap it with the last element and then remove it.
	swap(removedChunk->indexInFolder, chunks_.size() - 1);
	chunks_.pop_back();
	removedChunk->indexInFolder = kInvalidIndex;
}

void FolderChunks::markAsTested(Chunk* testedChunk) {
	markAsTestedInternal(testedChunk);
}

Chunk* FolderChunks::getRandomChunk() const {
	if (chunks_.empty()) {
		return nullptr;
	} else {
		return chunks_[rnd_ranged(Index(0), chunks_.size() - 1)];
	}
}

Chunk* FolderChunks::chunkToTest() const {
	if (chunks_.empty()) {
		return nullptr;
	} else {
		if (firstUntestedChunk_ == chunks_.size()) {
			// Start a new chunk test loop.
			firstUntestedChunk_ = 0;
		}
		return chunks_[firstUntestedChunk_];
	}
}

void FolderChunks::shuffle() {
	if (chunks_.size() <= 1) {
		return;
	}

	// This is the regular Fisher-Yates shuffle but we need to update
	// the `indexInFolder` values for chunks so we're using our own `swap()`.
	for (Index i = chunks_.size() - 1; i >= 1; --i) {
		swap(i, rnd_ranged(Index(0), i));
	}
	firstUntestedChunk_ = 0;
}

size_t FolderChunks::size() const {
	return chunks_.size();
}

void FolderChunks::swap(Index lhsIndex, Index rhsIndex) {
	Chunk* lhsChunk = chunks_[lhsIndex];
	Chunk* rhsChunk = chunks_[rhsIndex];
	std::swap(lhsChunk->indexInFolder, rhsChunk->indexInFolder);
	std::swap(chunks_[lhsIndex], chunks_[rhsIndex]);
}

void FolderChunks::markAsTestedInternal(Chunk* testedChunk) {
	if (testedChunk->indexInFolder >= firstUntestedChunk_) {
		swap(testedChunk->indexInFolder, firstUntestedChunk_);
		++firstUntestedChunk_;
	}
}
