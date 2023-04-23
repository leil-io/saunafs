#pragma once

#include "common/platform.h"

#include "chunkserver-common/chunk_interface.h"
#include "common/chunk_part_type.h"
#include "protocol/chunks_with_type.h"

/// Defines hash and equal operations on ChunkWithType type, so it can be used
/// as the key type in an std::unordered_map.
struct KeyOperations {
	constexpr KeyOperations() = default;
	constexpr std::size_t operator()(const ChunkWithType &chunkWithType) const {
		return hash(chunkWithType);
	}
	constexpr bool operator()(const ChunkWithType &lhs,
	                          const ChunkWithType &rhs) const {
		return equal(lhs, rhs);
	}

private:
	constexpr std::size_t hash(const ChunkWithType &chunkWithType) const {
		return chunkWithType.id;
	}
	constexpr bool equal(const ChunkWithType &lhs,
	                     const ChunkWithType &rhs) const {
		return (lhs.id == rhs.id && lhs.type == rhs.type);
	}
};

/// std::unique_ptr on Chunk is used here as the stored objects are of Chunk's
/// subclasses types.
using ChunkMap = std::unordered_map<ChunkWithType, std::unique_ptr<IChunk>,
                                    KeyOperations, KeyOperations>;

inline ChunkWithType makeChunkKey(uint64_t id, ChunkPartType type) {
	return {id, type};
}
inline ChunkWithType chunkToKey(const IChunk &chunk) {
	return makeChunkKey(chunk.id(), chunk.type());
}
