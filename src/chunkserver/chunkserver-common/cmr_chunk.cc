#include "cmr_chunk.h"
#include "common/slice_traits.h"

CmrChunk::CmrChunk(uint64_t chunkId, ChunkPartType type, ChunkState state)
    : FDChunk(chunkId, type, state) {}

std::string CmrChunk::generateDataFilenameForVersion(uint32_t _version) const {
	return generateFilenameForVersion(_version, false);
}

int CmrChunk::renameChunkFile(uint32_t new_version) {
	const std::string oldMetaFilename = metaFilename();
	const std::string oldDataFilename = dataFilename();

	const std::string newMetaFilename =
	    generateMetadataFilenameForVersion(new_version);
	const std::string newDataFilename =
	    generateDataFilenameForVersion(new_version);

	int status = rename(oldMetaFilename.c_str(), newMetaFilename.c_str());
	if (status < 0) {
		return status;
	}

	status = rename(oldDataFilename.c_str(), newDataFilename.c_str());
	if (status < 0) {
		return status;
	}

	setVersion(new_version);
	setMetaFilename(newMetaFilename);
	setDataFilename(newDataFilename);

	return 0;
}

uint8_t *CmrChunk::getChunkHeaderBuffer() const {
#ifdef SAUNAFS_HAVE_THREAD_LOCAL
	static thread_local std::array<uint8_t, kMaxHeaderSize> hdrbuffer;
	return hdrbuffer.data();
#else  // SAUNAFS_HAVE_THREAD_LOCAL
	uint8_t *hdrbuffer =
	    static_cast<uint8_t *>(pthread_getspecific(hdrbufferkey));
	if (hdrbuffer == NULL) {
		hdrbuffer = static_cast<uint8_t *>(malloc(kMaxHeaderSize));
		passert(hdrbuffer);
		zassert(pthread_setspecific(hdrbufferkey, hdrbuffer));
	}
	return hdrbuffer;
#endif  // SAUNAFS_HAVE_THREAD_LOCAL
}

size_t CmrChunk::getHeaderSize() const {
	auto chunkType = type();

	if (slice_traits::isStandard(chunkType)) {
		return kMaxSignatureBlockSize + kCrcSize * maxBlocksInFile();
	}

	assert(slice_traits::isXor(chunkType) || slice_traits::isEC(chunkType));

	const uint32_t requiredHeaderSize =
	    kMaxSignatureBlockSize + kCrcSize * maxBlocksInFile();
	// header size is equal to the requiredHeaderSize rounded up to typical
	// disk block size
	const uint32_t diskBlockSize = kDiskBlockSize;
	const off_t dataOffset = (requiredHeaderSize + diskBlockSize - 1) /
	                         diskBlockSize * diskBlockSize;
	return dataOffset;
}

off_t CmrChunk::getCrcOffset() const { return kMaxSignatureBlockSize; }

void CmrChunk::shrinkToBlocks(uint16_t newBlocks) { (void)newBlocks; }

bool CmrChunk::isDirty() { return false; }

std::string CmrChunk::toString() const {
	std::stringstream result;

	result << "{id: " << id() << ", version: " << version()
	       << ", type: " << type().toString() << ", blocks: " << blocks()
	       << "}";

	return result.str();
}
