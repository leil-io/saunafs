#pragma once

#include <cstddef>
#include <limits>
#include <mutex>
#include <vector>

class Chunk;

/// A class which holds pointers to the chunks of a single folder (disk).
///
/// The collection keeps track of which chunks were already tested for correctness.
/// The tests may occur in an arbitrary order, as long as each chunk is eventually tested.
///
/// The class aims to achieve the following constraints:
///  * selecting a single chunk at random in constant time,
///  * able to shuffle all chunks into a random order,
///  * constant-time operations of insertion, deletion, mark as tested, and get-chunk-to-test.
///
/// This collection uses the `indexInFolder` field in the Chunk structure to be
/// able to achieve constant-time operations. Elements of this collection are swapped
/// around rather than being inserted into the underlying vector at arbitrary locations;
/// when elements are swapped, their `indexInFolder` field needs to be updated as well.
class FolderChunks {
public:
	/// The type of the index which is associated with each chunk to designate
	/// its position in the collection.
	using Index = size_t;

	/// The value of `Index` which indicates an invalid index.
	///
	/// This can be used for chunks which are not assigned a position in the index yet.
	static const Index kInvalidIndex = std::numeric_limits<Index>::max();

	/// Creates a new collection of chunks.
	FolderChunks();

	/// Disable copying.
	FolderChunks(const FolderChunks&) = delete;

	/// Disable moving.
	FolderChunks(FolderChunks&& other) = delete;

	/// Destructor.
	~FolderChunks() = default;

	/// Disable copying.
	FolderChunks& operator=(const FolderChunks&) = delete;

	/// Disable moving.
	FolderChunks& operator=(FolderChunks&& other) = delete;

	/// Inserts a new chunk into the collection.
	///
	/// New chunks are marked as "tested" because they have just been written to.
	void insert(Chunk* newChunk);

	/// Removes the given chunk from the collection.
	void remove(Chunk* removedChunk);

	/// Returns the next chunk to be tested.
	///
	/// This returns `nullptr` if there are no chunks in the collection.
	Chunk* chunkToTest() const;

	/// Marks the given chunk as "tested".
	void markAsTested(Chunk* chunk);

	/// Returns a random chunk from the folder.
	///
	/// The sequence in which chunks are returned from this method
	/// is not related to the random sequence coming from `shuffle`.
	///
	/// A `nullptr` is returned if the collection of chunks is empty.
	Chunk* getRandomChunk() const;

	/// Shuffles all chunks in the collection into a random order.
	///
	/// This also marks all chunks as untested.
	void shuffle();

	/// Returns the number of chunks in the collection.
	size_t size() const;

private:
	/// Swaps the elements at the given indices.
	///
	/// This also updates the indexes stored in chunks. This should be used as the
	/// only mechanism of moving chunks around in the collection.
	void swap(Index lhs, Index rhs);

	/// An internal function for marking the given chunk as tested.
	void markAsTestedInternal(Chunk* testedChunk);

	/// The vector containing all chunks in the collection.
	///
	/// This vector has two "sections": one holding chunks which were already tested,
	/// and one holding tested chunks. The sections are delimited by the index
	/// `firstUntestedChunk_`. The order of chunks within a single section is not
	/// important, but it is important that all untested chunks come after all
	/// tested chunks.
	std::vector<Chunk*> chunks_;

	/// The index of the first chunk which was not tested yet.
	///
	/// This can be equal to "chunks_.size()", which means that all chunks
	/// are tested, When the next chunk is requested to be tested, a new test loop
	/// should be started by changing this to "0" again.
	mutable Index firstUntestedChunk_ = 0;
};
