/*
   Copyright 2013-2015 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ

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
#include "chunkserver/network_worker_thread.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <atomic>
#include <cassert>
#include <memory>
#include <mutex>
#include <set>

#include "chunkserver/bgjobs.h"
#include "chunkserver/hdd_readahead.h"
#include "chunkserver/hddspacemgr.h"
#include "chunkserver/network_stats.h"
#include "common/cfg.h"
#include "common/charts.h"
#include "protocol/cltocs.h"
#include "protocol/cstocl.h"
#include "protocol/cstocs.h"
#include "common/datapack.h"
#include "common/event_loop.h"
#include "common/saunafs_version.h"
#include "common/massert.h"
#include "protocol/SFSCommunication.h"
#include "common/legacy_vector.h"
#include "protocol/packet.h"
#include "slogger/slogger.h"
#include "common/sockets.h"
#include "devtools/request_log.h"
#include "devtools/TracePrinter.h"

#define MaxPacketSize (100000 + SFSBLOCKSIZE)

// connection timeout in seconds
#define CSSERV_TIMEOUT 10

#define CONNECT_RETRIES 10
#define CONNECT_TIMEOUT(cnt) (((cnt)%2)?(300000*(1<<((cnt)>>1))):(200000*(1<<((cnt)>>1))))

class MessageSerializer {
public:
	static MessageSerializer* getSerializer(PacketHeader::Type type);

	virtual void serializePrefixOfCstoclReadData(std::vector<uint8_t>& buffer,
			uint64_t chunkId, uint32_t offset, uint32_t size) = 0;
	virtual void serializeCstoclReadStatus(std::vector<uint8_t>& buffer,
			uint64_t chunkId, uint8_t status) = 0;
	virtual void serializeCstoclWriteStatus(std::vector<uint8_t>& buffer,
			uint64_t chunkId, uint32_t writeId, uint8_t status) = 0;
	virtual ~MessageSerializer() {}
};

class LegacyMessageSerializer : public MessageSerializer {
public:
	void serializePrefixOfCstoclReadData(std::vector<uint8_t>& buffer,
			uint64_t chunkId, uint32_t offset, uint32_t size) {
		// This prefix requires CRC (uint32_t) and data (size * uint8_t) to be appended
		uint32_t extraSpace = sizeof(uint32_t) + size;
		serializeLegacyPacketPrefix(buffer, extraSpace, CSTOCL_READ_DATA, chunkId, offset, size);
	}

	void serializeCstoclReadStatus(std::vector<uint8_t>& buffer,
			uint64_t chunkId, uint8_t status) {
		serializeLegacyPacket(buffer, CSTOCL_READ_STATUS, chunkId, status);
	}

	void serializeCstoclWriteStatus(std::vector<uint8_t>& buffer,
			uint64_t chunkId, uint32_t writeId, uint8_t status) {
		serializeLegacyPacket(buffer, CSTOCL_WRITE_STATUS, chunkId, writeId, status);
	}
};

class SaunaFsMessageSerializer : public MessageSerializer {
public:
	void serializePrefixOfCstoclReadData(std::vector<uint8_t>& buffer,
			uint64_t chunkId, uint32_t offset, uint32_t size) {
		cstocl::readData::serializePrefix(buffer, chunkId, offset, size);
	}

	void serializeCstoclReadStatus(std::vector<uint8_t>& buffer,
			uint64_t chunkId, uint8_t status) {
		cstocl::readStatus::serialize(buffer, chunkId, status);
	}

	void serializeCstoclWriteStatus(std::vector<uint8_t>& buffer,
			uint64_t chunkId, uint32_t writeId, uint8_t status) {
		cstocl::writeStatus::serialize(buffer, chunkId, writeId, status);
	}
};

MessageSerializer* MessageSerializer::getSerializer(PacketHeader::Type type) {
	sassert((type >= PacketHeader::kMinSauPacketType && type <= PacketHeader::kMaxSauPacketType)
			|| type <= PacketHeader::kMaxOldPacketType);
	if (type <= PacketHeader::kMaxOldPacketType) {
		static LegacyMessageSerializer singleton;
		return &singleton;
	} else {
		static SaunaFsMessageSerializer singleton;
		return &singleton;
	}
}

std::unique_ptr<PacketStruct> worker_create_detached_packet_with_output_buffer(
    const std::vector<uint8_t> &packetPrefix) {
	TRACETHIS();
	PacketHeader header;
	deserializePacketHeader(packetPrefix, header);
	uint32_t sizeOfWholePacket = PacketHeader::kSize + header.length;
	std::unique_ptr<PacketStruct> outPacket = std::make_unique<PacketStruct>();
	passert(outPacket);
	outPacket->outputBuffer = getReadOutputBufferPool().get(sizeOfWholePacket);

	if (outPacket->outputBuffer->copyIntoBuffer(packetPrefix) !=
	    static_cast<ssize_t>(packetPrefix.size())) {
		if (outPacket->outputBuffer) {
			getReadOutputBufferPool().put(std::move(outPacket->outputBuffer));
		}
		return nullptr;
	}

	return outPacket;
}

void ChunkserverEntry::releasePacketResources(
    std::unique_ptr<PacketStruct> &packet) {
	TRACETHIS();
	free(packet->packet);
	packet->packet = nullptr;
}

void ChunkserverEntry::attachPacket(std::unique_ptr<PacketStruct> &&packet) {
	outputPackets.push_back(std::move(packet));
}

void ChunkserverEntry::preserveInputPacket() {
	TRACETHIS();
	writePacket->packet = inputPacket.packet;
	inputPacket.packet = nullptr;
}

void ChunkserverEntry::deletePreservedPacket(
    std::unique_ptr<PacketStruct> &packet) {
	TRACETHIS();
	if (packet) {
		free(packet->packet);
		packet->packet = nullptr;
	}
}

void worker_create_attached_packet(ChunkserverEntry *eptr,
                                   const std::vector<uint8_t> &packet) {
	TRACETHIS();
	std::unique_ptr<PacketStruct> outpacket = std::make_unique<PacketStruct>();
	passert(outpacket);
	outpacket->packet = (uint8_t*) malloc(packet.size());
	passert(outpacket->packet);
	memcpy(outpacket->packet, packet.data(), packet.size());
	outpacket->bytesLeft = packet.size();
	outpacket->startPtr = outpacket->packet;
	eptr->attachPacket(std::move(outpacket));
}

uint8_t *worker_create_attached_packet(ChunkserverEntry *eptr, uint32_t type,
                                       uint32_t size) {
	TRACETHIS();
	uint8_t *ptr;
	uint32_t packetSize;

	std::unique_ptr<PacketStruct> outPacket = std::make_unique<PacketStruct>();
	passert(outPacket);
	packetSize = size + PacketHeader::kSize;
	outPacket->packet = (uint8_t*) malloc(packetSize);
	passert(outPacket->packet);
	outPacket->bytesLeft = packetSize;
	ptr = outPacket->packet;
	put32bit(&ptr, type);
	put32bit(&ptr, size);
	outPacket->startPtr = outPacket->packet;
	outPacket->next = nullptr;
	eptr->attachPacket(std::move(outPacket));

	return ptr;
}

void worker_fwderror(ChunkserverEntry *eptr) {
	TRACETHIS();
	sassert(eptr->messageSerializer != NULL);
	std::vector<uint8_t> buffer;
	uint8_t status = (eptr->state == ChunkserverEntry::State::Connecting
	                      ? SAUNAFS_ERROR_CANTCONNECT
	                      : SAUNAFS_ERROR_DISCONNECTED);
	eptr->messageSerializer->serializeCstoclWriteStatus(buffer, eptr->chunkId,
	                                                    0, status);
	worker_create_attached_packet(eptr, buffer);
	eptr->state = ChunkserverEntry::State::WriteFinish;
}

// initialize connection to another CS
int worker_initconnect(ChunkserverEntry *eptr) {
	TRACETHIS();
	int status;
	// TODO(msulikowski) If we want to use a ConnectionPool, this is the right place
	// to get a connection from it
	eptr->fwdSocket = tcpsocket();
	if (eptr->fwdSocket < 0) {
		safs_pretty_errlog(LOG_WARNING, "create socket, error");
		return -1;
	}
	if (tcpnonblock(eptr->fwdSocket) < 0) {
		safs_pretty_errlog(LOG_WARNING, "set nonblock, error");
		tcpclose(eptr->fwdSocket);
		eptr->fwdSocket = -1;
		return -1;
	}
	status = tcpnumconnect(eptr->fwdSocket, eptr->fwdServer.ip,
	                       eptr->fwdServer.port);
	if (status < 0) {
		safs_pretty_errlog(LOG_WARNING, "connect failed, error");
		tcpclose(eptr->fwdSocket);
		eptr->fwdSocket = -1;
		return -1;
	}
	if (status == 0) { // connected immediately
		tcpnodelay(eptr->fwdSocket);
		eptr->state = ChunkserverEntry::State::WriteInit;
	} else {
		eptr->state = ChunkserverEntry::State::Connecting;
		eptr->connectStartTimeUSec = eventloop_utime();
	}
	return 0;
}

void worker_retryconnect(ChunkserverEntry *eptr) {
	TRACETHIS();
	tcpclose(eptr->fwdSocket);
	eptr->fwdSocket = -1;
	eptr->connectRetryCounter++;
	if (eptr->connectRetryCounter < CONNECT_RETRIES) {
		if (worker_initconnect(eptr) < 0) {
			worker_fwderror(eptr);
			return;
		}
	} else {
		worker_fwderror(eptr);
		return;
	}
}

void worker_check_nextpacket(ChunkserverEntry *eptr);

// common - delayed close
void worker_delayed_close(uint8_t status, void *e) {
	TRACETHIS();
	auto *eptr = static_cast<ChunkserverEntry*>(e);
	if (eptr->writeJobId > 0 && eptr->writeJobWriteId == 0 &&
	    status == SAUNAFS_STATUS_OK) {  // this was job_open
		eptr->isChunkOpen = 1;
	} else if (eptr->readJobId > 0 &&
	           status == SAUNAFS_STATUS_OK) {  // this could be job_open
		eptr->isChunkOpen = 1;
	}
	if (eptr->isChunkOpen) {
		job_close(eptr->workerJobPool, nullptr, nullptr, eptr->chunkId,
		          eptr->chunkType);
		eptr->isChunkOpen = 0;
	}
	eptr->state = ChunkserverEntry::State::Closed;
}

// bg reading

void worker_read_continue(ChunkserverEntry *eptr);

void worker_read_finished(uint8_t status, void *e) {
	TRACETHIS();
	auto *eptr = static_cast<ChunkserverEntry*>(e);
	eptr->readJobId = 0;
	if (status == SAUNAFS_STATUS_OK) {
		eptr->todoReadCounter--;
		eptr->isChunkOpen = 1;
		if (eptr->todoReadCounter == 0) {
			worker_read_continue(eptr);
		}
	} else {
		if (eptr->readPacket) {
			eptr->releasePacketResources(eptr->readPacket);
			eptr->readPacket.reset();
		}
		std::vector<uint8_t> buffer;
		eptr->messageSerializer->serializeCstoclReadStatus(
		    buffer, eptr->chunkId, status);
		worker_create_attached_packet(eptr, buffer);
		if (eptr->isChunkOpen) {
			job_close(eptr->workerJobPool, nullptr, nullptr, eptr->chunkId,
			          eptr->chunkType);
			eptr->isChunkOpen = 0;
		}
		// after sending status even if there was an error it's possible to
		eptr->state = ChunkserverEntry::State::Idle;
		// receive new requests on the same connection
		LOG_AVG_STOP(eptr->readOperationTimer);
	}
}

void worker_send_finished(ChunkserverEntry *eptr) {
	TRACETHIS();
	eptr->todoReadCounter--;
	if (eptr->todoReadCounter == 0) {
		worker_read_continue(eptr);
	}
}

void worker_read_continue(ChunkserverEntry *eptr) {
	TRACETHIS2(eptr->offset, eptr->size);

	if (eptr->readPacket) {
		eptr->attachPacket(std::move(eptr->readPacket));
		eptr->readPacket.reset();
		eptr->todoReadCounter++;
	}
	if (eptr->size == 0) { // everything has been read
		std::vector<uint8_t> buffer;
		eptr->messageSerializer->serializeCstoclReadStatus(
		    buffer, eptr->chunkId, SAUNAFS_STATUS_OK);
		worker_create_attached_packet(eptr, buffer);
		sassert(eptr->isChunkOpen);
		job_close(eptr->workerJobPool, nullptr, nullptr, eptr->chunkId,
		          eptr->chunkType);
		eptr->isChunkOpen = 0;
		// no error - do not disconnect - go direct to the IDLE state, ready for
		// requests on the same connection
		eptr->state = ChunkserverEntry::State::Idle;
		LOG_AVG_STOP(eptr->readOperationTimer);
	} else {
		const uint32_t totalRequestSize = eptr->size;
		const uint32_t thisPartOffset = eptr->offset % SFSBLOCKSIZE;
		const uint32_t thisPartSize = std::min<uint32_t>(
				totalRequestSize, SFSBLOCKSIZE - thisPartOffset);
		const uint16_t totalRequestBlocks =
				(totalRequestSize + thisPartOffset + SFSBLOCKSIZE - 1) / SFSBLOCKSIZE;
		std::vector<uint8_t> readDataPrefix;
		eptr->messageSerializer->serializePrefixOfCstoclReadData(
		    readDataPrefix, eptr->chunkId, eptr->offset, thisPartSize);
		auto packet =
		    worker_create_detached_packet_with_output_buffer(readDataPrefix);
		if (packet == nullptr) {
			eptr->state = ChunkserverEntry::State::Close;
			return;
		}
		eptr->readPacket = std::move(packet);
		uint32_t readAheadBlocks = 0;
		uint32_t maxReadBehindBlocks = 0;
		if (!eptr->isChunkOpen) {
			if (gHDDReadAhead.blocksToBeReadAhead() > 0) {
				readAheadBlocks = totalRequestBlocks + gHDDReadAhead.blocksToBeReadAhead();
			}
			// Try not to influence slow streams to much:
			maxReadBehindBlocks = std::min(totalRequestBlocks,
					gHDDReadAhead.maxBlocksToBeReadBehind());
		}
		eptr->readJobId = job_read(
		    eptr->workerJobPool, worker_read_finished, eptr, eptr->chunkId,
		    eptr->chunkVersion, eptr->chunkType, eptr->offset, thisPartSize,
		    maxReadBehindBlocks, readAheadBlocks,
		    eptr->readPacket->outputBuffer.get(), !eptr->isChunkOpen);
		if (eptr->readJobId == 0) {
			eptr->state = ChunkserverEntry::State::Close;
			return;
		}
		eptr->todoReadCounter++;
		eptr->offset += thisPartSize;
		eptr->size -= thisPartSize;
	}
}

void worker_ping(ChunkserverEntry *eptr, const uint8_t *data,
                 PacketHeader::Length length) {
	if (length != 4) {
		eptr->state = ChunkserverEntry::State::Close;
		return;
	}

	uint32_t size;
	deserialize(data, length, size);
	worker_create_attached_packet(eptr, ANTOAN_PING_REPLY, size);
}

void worker_read_init(ChunkserverEntry *eptr, const uint8_t *data,
                      PacketHeader::Type type, PacketHeader::Length length) {
	TRACETHIS2(type, length);

	// Deserialize request
	sassert(type == SAU_CLTOCS_READ || type == CLTOCS_READ);
	try {
		if (type == SAU_CLTOCS_READ) {
			PacketVersion v;
			deserializePacketVersionNoHeader(data, length, v);
			if (v == cltocs::read::kECChunks) {
				cltocs::read::deserialize(data, length,
						eptr->chunkId,
						eptr->chunkVersion,
						eptr->chunkType,
						eptr->offset,
						eptr->size);
			} else {
				legacy::ChunkPartType legacy_type;
				cltocs::read::deserialize(data, length,
						eptr->chunkId,
						eptr->chunkVersion,
						legacy_type,
						eptr->offset,
						eptr->size);
				eptr->chunkType = legacy_type;
			}
		} else {
			deserializeAllLegacyPacketDataNoHeader(data, length,
					eptr->chunkId,
					eptr->chunkVersion,
					eptr->offset,
					eptr->size);
			eptr->chunkType = slice_traits::standard::ChunkPartType();
		}
		eptr->messageSerializer = MessageSerializer::getSerializer(type);
	} catch (IncorrectDeserializationException&) {
		safs_pretty_syslog(LOG_NOTICE, "read_init: Cannot deserialize READ message (type:%"
				PRIX32 ", length:%" PRIu32 ")", type, length);
		eptr->state = ChunkserverEntry::State::Close;
		return;
	}
	// Check if the request is valid
	std::vector<uint8_t> instantResponseBuffer;
	if (eptr->size == 0) {
		eptr->messageSerializer->serializeCstoclReadStatus(
		    instantResponseBuffer, eptr->chunkId, SAUNAFS_STATUS_OK);
	} else if (eptr->size > SFSCHUNKSIZE) {
		eptr->messageSerializer->serializeCstoclReadStatus(
		    instantResponseBuffer, eptr->chunkId, SAUNAFS_ERROR_WRONGSIZE);
	} else if (eptr->offset >= SFSCHUNKSIZE || eptr->offset + eptr->size > SFSCHUNKSIZE) {
		eptr->messageSerializer->serializeCstoclReadStatus(
		    instantResponseBuffer, eptr->chunkId, SAUNAFS_ERROR_WRONGOFFSET);
	}
	if (!instantResponseBuffer.empty()) {
		worker_create_attached_packet(eptr, instantResponseBuffer);
		return;
	}
	// Process the request
	stats_hlopr++;
	eptr->state = ChunkserverEntry::State::Read;
	eptr->todoReadCounter = 0;
	eptr->readJobId = 0;
	LOG_AVG_START0(eptr->readOperationTimer, "csserv_read");
	worker_read_continue(eptr);
}

void worker_prefetch(ChunkserverEntry *eptr, const uint8_t *data,
                     PacketHeader::Type type, PacketHeader::Length length) {
	sassert(type == SAU_CLTOCS_PREFETCH);
	PacketVersion v;
	try {
		deserializePacketVersionNoHeader(data, length, v);
		if (v == cltocs::prefetch::kECChunks) {
			cltocs::prefetch::deserialize(data, length,
				eptr->chunkId,
				eptr->chunkVersion,
				eptr->chunkType,
				eptr->offset,
				eptr->size);
		} else {
			legacy::ChunkPartType legacy_type;
			cltocs::prefetch::deserialize(data, length,
				eptr->chunkId,
				eptr->chunkVersion,
				legacy_type,
				eptr->offset,
				eptr->size);
			eptr->chunkType = legacy_type;
		}
	} catch (IncorrectDeserializationException&) {
		safs_pretty_syslog(LOG_NOTICE, "prefetch: Cannot deserialize PREFETCH message (type:%"
				PRIX32 ", length:%" PRIu32 ")", type, length);
		eptr->state = ChunkserverEntry::State::Close;
		return;
	}
	// Start prefetching in background, don't wait for it to complete
	auto firstBlock = eptr->offset / SFSBLOCKSIZE;
	auto lastByte = eptr->offset + eptr->size - 1;
	auto lastBlock = lastByte / SFSBLOCKSIZE;
	auto nrOfBlocks = lastBlock - firstBlock + 1;
	job_prefetch(eptr->workerJobPool, eptr->chunkId, eptr->chunkVersion,
	             eptr->chunkType, firstBlock, nrOfBlocks);
}

// bg writing

void worker_write_finished(uint8_t status, void *e) {
	TRACETHIS();
	auto *eptr = static_cast<ChunkserverEntry*>(e);
	eptr->writeJobId = 0;
	sassert(eptr->messageSerializer != NULL);
	if (status != SAUNAFS_STATUS_OK) {
		std::vector<uint8_t> buffer;
		eptr->messageSerializer->serializeCstoclWriteStatus(
		    buffer, eptr->chunkId, eptr->writeJobWriteId, status);
		worker_create_attached_packet(eptr, buffer);
		eptr->state = ChunkserverEntry::State::WriteFinish;
		return;
	}
	if (eptr->writeJobWriteId == 0) {
		eptr->isChunkOpen = 1;
	}
	if (eptr->state == ChunkserverEntry::State::WriteLast) {
		std::vector<uint8_t> buffer;
		eptr->messageSerializer->serializeCstoclWriteStatus(
		    buffer, eptr->chunkId, eptr->writeJobWriteId, status);
		worker_create_attached_packet(eptr, buffer);
	} else {
		if (eptr->partiallyCompletedWrites.count(eptr->writeJobWriteId) > 0) {
			// found - it means that it was added by status_receive, ie. next chunkserver from
			// a chain finished writing before our worker
			sassert(eptr->messageSerializer != NULL);
			std::vector<uint8_t> buffer;
			eptr->messageSerializer->serializeCstoclWriteStatus(
			    buffer, eptr->chunkId, eptr->writeJobWriteId,
			    SAUNAFS_STATUS_OK);
			worker_create_attached_packet(eptr, buffer);
			eptr->partiallyCompletedWrites.erase(eptr->writeJobWriteId);
		} else {
			// not found - so add it
			eptr->partiallyCompletedWrites.insert(eptr->writeJobWriteId);
		}
	}
	worker_check_nextpacket(eptr);
}

void serializeCltocsWriteInit(std::vector<uint8_t>& buffer,
		uint64_t chunkId, uint32_t chunkVersion, ChunkPartType chunkType,
		const std::vector<ChunkTypeWithAddress>& chain, uint32_t target_version) {

	if (target_version >= kFirstECVersion) {
		cltocs::writeInit::serialize(buffer, chunkId, chunkVersion, chunkType, chain);
	} else if (target_version >= kFirstXorVersion) {
		assert((int)chunkType.getSliceType() < Goal::Slice::Type::kECFirst);
		std::vector<NetworkAddress> legacy_chain;
		legacy_chain.reserve(chain.size());
		for (const auto &entry : chain) {
			legacy_chain.push_back(entry.address);
		}
		cltocs::writeInit::serialize(buffer, chunkId, chunkVersion,
			(legacy::ChunkPartType)chunkType, legacy_chain);
	} else {
		assert(slice_traits::isStandard(chunkType));
		LegacyVector<NetworkAddress> legacy_chain;
		legacy_chain.reserve(chain.size());
		for (const auto &entry : chain) {
			legacy_chain.push_back(entry.address);
		}
		serializeLegacyPacket(buffer, CLTOCS_WRITE, chunkId, chunkVersion, legacy_chain);
	}
}

void worker_write_init(ChunkserverEntry *eptr, const uint8_t *data,
                       PacketHeader::Type type, PacketHeader::Length length) {
	TRACETHIS();
	std::vector<ChunkTypeWithAddress> chain;

	sassert(type == SAU_CLTOCS_WRITE_INIT || type == CLTOCS_WRITE);
	try {
		if (type == SAU_CLTOCS_WRITE_INIT) {
			PacketVersion v;
			deserializePacketVersionNoHeader(data, length, v);
			if (v == cltocs::writeInit::kECChunks) {
				cltocs::writeInit::deserialize(data, length,
					eptr->chunkId, eptr->chunkVersion, eptr->chunkType, chain);
			} else {
				std::vector<NetworkAddress> legacy_chain;
				legacy::ChunkPartType legacy_type;
				cltocs::writeInit::deserialize(data, length, eptr->chunkId,
				                               eptr->chunkVersion, legacy_type,
				                               legacy_chain);
				eptr->chunkType = legacy_type;
				for (const auto &address : legacy_chain) {
					chain.push_back(ChunkTypeWithAddress(address, eptr->chunkType, kFirstXorVersion));
				}
			}
		} else {
			LegacyVector<NetworkAddress> legacyChain;
			deserializeAllLegacyPacketDataNoHeader(data, length,
				eptr->chunkId, eptr->chunkVersion, legacyChain);
			for (const auto &address : legacyChain) {
				chain.push_back(ChunkTypeWithAddress(address, slice_traits::standard::ChunkPartType(), kStdVersion));
			}
			eptr->chunkType = slice_traits::standard::ChunkPartType();
		}
		eptr->messageSerializer = MessageSerializer::getSerializer(type);
	} catch (IncorrectDeserializationException& ex) {
		safs_pretty_syslog(LOG_NOTICE, "Received malformed WRITE_INIT message (length: %" PRIu32 ")", length);
		eptr->state = ChunkserverEntry::State::Close;
		return;
	}

	if (!chain.empty()) {
		// Create a chain -- connect to the next chunkserver
		eptr->fwdServer = chain[0].address;
		uint32_t target_version = chain[0].chunkserver_version;
		chain.erase(chain.begin());
		serializeCltocsWriteInit(eptr->fwdInitPacket, eptr->chunkId,
		                         eptr->chunkVersion, eptr->chunkType, chain,
		                         target_version);
		eptr->fwdStartPtr = eptr->fwdInitPacket.data();
		eptr->fwdBytesLeft = eptr->fwdInitPacket.size();
		eptr->connectRetryCounter = 0;
		if (worker_initconnect(eptr) < 0) {
			std::vector<uint8_t> buffer;
			eptr->messageSerializer->serializeCstoclWriteStatus(
			    buffer, eptr->chunkId, 0, SAUNAFS_ERROR_CANTCONNECT);
			worker_create_attached_packet(eptr, buffer);
			eptr->state = ChunkserverEntry::State::WriteFinish;
			return;
		}
	} else {
		eptr->state = ChunkserverEntry::State::WriteLast;
	}
	stats_hlopw++;
	eptr->writeJobWriteId = 0;
	eptr->writeJobId = job_open(eptr->workerJobPool, worker_write_finished,
	                            eptr, eptr->chunkId, eptr->chunkType);
}

void worker_write_data(ChunkserverEntry *eptr, const uint8_t *data,
                       PacketHeader::Type type, PacketHeader::Length length) {
	TRACETHIS();
	uint64_t chunkId;
	uint32_t writeId;
	uint16_t blocknum;
	uint32_t offset;
	uint32_t size;
	uint32_t crc;
	const uint8_t* dataToWrite;

	sassert(type == SAU_CLTOCS_WRITE_DATA || type == CLTOCS_WRITE_DATA);
	try {
		const MessageSerializer *serializer = MessageSerializer::getSerializer(type);
		if (eptr->messageSerializer != serializer) {
			safs_pretty_syslog(LOG_NOTICE, "Received WRITE_DATA message incompatible with WRITE_INIT");
			eptr->state = ChunkserverEntry::State::Close;
			return;
		}
		if (type == SAU_CLTOCS_WRITE_DATA) {
			cltocs::writeData::deserializePrefix(data, length,
					chunkId, writeId, blocknum, offset, size, crc);
			dataToWrite = data + cltocs::writeData::kPrefixSize;
		} else {
			uint16_t offset16;
			deserializeAllLegacyPacketDataNoHeader(data, length,
				chunkId, writeId, blocknum, offset16, size, crc, dataToWrite);
			offset = offset16;
			sassert(eptr->chunkType == slice_traits::standard::ChunkPartType());
		}
	} catch (IncorrectDeserializationException&) {
		safs_pretty_syslog(LOG_NOTICE, "Received malformed WRITE_DATA message (length: %" PRIu32 ")", length);
		eptr->state = ChunkserverEntry::State::Close;
		return;
	}

	uint8_t status = SAUNAFS_STATUS_OK;
	uint32_t dataSize = data + length - dataToWrite;
	if (dataSize != size) {
		status = SAUNAFS_ERROR_WRONGSIZE;
	} else if (chunkId != eptr->chunkId) {
		status = SAUNAFS_ERROR_WRONGCHUNKID;
	}

	if (status != SAUNAFS_STATUS_OK) {
		std::vector<uint8_t> buffer;
		eptr->messageSerializer->serializeCstoclWriteStatus(buffer, chunkId, writeId, status);
		worker_create_attached_packet(eptr, buffer);
		eptr->state = ChunkserverEntry::State::WriteFinish;
		return;
	}
	if (eptr->writePacket) {
		ChunkserverEntry::deletePreservedPacket(eptr->writePacket);
	}
	eptr->preserveInputPacket();
	eptr->writeJobWriteId = writeId;
	eptr->writeJobId = job_write(eptr->workerJobPool, worker_write_finished,
	                             eptr, chunkId, eptr->chunkVersion, eptr->chunkType,
	                             blocknum, offset, size, crc, dataToWrite);
}

void worker_write_status(ChunkserverEntry *eptr, const uint8_t *data,
                         PacketHeader::Type type, PacketHeader::Length length) {
	TRACETHIS();
	uint64_t chunkId;
	uint32_t writeId;
	uint8_t status;

	sassert(type == SAU_CSTOCL_WRITE_STATUS || type == CSTOCL_WRITE_STATUS);
	sassert(eptr->messageSerializer != NULL);
	try {
		const MessageSerializer *serializer = MessageSerializer::getSerializer(type);
		if (eptr->messageSerializer != serializer) {
			safs_pretty_syslog(LOG_NOTICE, "Received WRITE_DATA message incompatible with WRITE_INIT");
			eptr->state = ChunkserverEntry::State::Close;
			return;
		}
		if (type == SAU_CSTOCL_WRITE_STATUS) {
			std::vector<uint8_t> message(data, data + length);
			cstocl::writeStatus::deserialize(message, chunkId, writeId, status);
		} else {
			deserializeAllLegacyPacketDataNoHeader(data, length, chunkId, writeId, status);
			sassert(eptr->chunkType == slice_traits::standard::ChunkPartType());
		}
	} catch (IncorrectDeserializationException&) {
		safs_pretty_syslog(LOG_NOTICE, "Received malformed WRITE_STATUS message (length: %" PRIu32 ")", length);
		eptr->state = ChunkserverEntry::State::Close;
		return;
	}

	if (eptr->chunkId != chunkId) {
		status = SAUNAFS_ERROR_WRONGCHUNKID;
		writeId = 0;
	}

	if (status != SAUNAFS_STATUS_OK) {
		std::vector<uint8_t> buffer;
		eptr->messageSerializer->serializeCstoclWriteStatus(buffer, chunkId, writeId, status);
		worker_create_attached_packet(eptr, buffer);
		eptr->state = ChunkserverEntry::State::WriteFinish;
		return;
	}

	if (eptr->partiallyCompletedWrites.count(writeId) > 0) {
		// found - means it was added by write_finished
		std::vector<uint8_t> buffer;
		eptr->messageSerializer->serializeCstoclWriteStatus(buffer, chunkId, writeId, SAUNAFS_STATUS_OK);
		worker_create_attached_packet(eptr, buffer);
		eptr->partiallyCompletedWrites.erase(writeId);
	} else {
		// if not found then add record
		eptr->partiallyCompletedWrites.insert(writeId);
	}
}

void worker_write_end(ChunkserverEntry *eptr, const uint8_t *data,
                      uint32_t length) {
	uint64_t chunkId;
	eptr->messageSerializer = nullptr;
	try {
		cltocs::writeEnd::deserialize(data, length, chunkId);
	} catch (IncorrectDeserializationException&) {
		safs_pretty_syslog(LOG_NOTICE,"Received malformed WRITE_END message (length: %" PRIu32 ")", length);
		eptr->state = ChunkserverEntry::State::WriteFinish;
		return;
	}
	if (chunkId != eptr->chunkId) {
		safs_pretty_syslog(LOG_NOTICE,"Received malformed WRITE_END message "
				"(got chunkId=%016" PRIX64 ", expected %016" PRIX64 ")",
				chunkId, eptr->chunkId);
		eptr->state = ChunkserverEntry::State::WriteFinish;
		return;
	}
	if (eptr->writeJobId > 0 || !eptr->partiallyCompletedWrites.empty() ||
	    !eptr->outputPackets.empty()) {
		/*
		 * WRITE_END received too early:
		 * eptr->wjobid > 0 -- hdd worker is working (writing some data)
		 * !eptr->partiallyCompletedWrites.empty() -- there are write tasks which have not been
		 *         acked by our hdd worker EX-or next chunkserver from a chain
		 * eptr->outputhead != nullptr -- there is a status being send
		 */
		// TODO(msulikowski) temporary syslog message. May be useful until this code is fully tested
		safs_pretty_syslog(LOG_NOTICE, "Received WRITE_END message too early");
		eptr->state = ChunkserverEntry::State::WriteFinish;
		return;
	}
	if (eptr->isChunkOpen) {
		job_close(eptr->workerJobPool, NULL, NULL, eptr->chunkId,
		          eptr->chunkType);
		eptr->isChunkOpen = 0;
	}
	if (eptr->fwdSocket > 0) {
		// TODO(msulikowski) if we want to use a ConnectionPool, this the right place to put the
		// connection to the pool.
		tcpclose(eptr->fwdSocket);
		eptr->fwdSocket = -1;
	}
	eptr->state = ChunkserverEntry::State::Idle;
}

void worker_sau_get_chunk_blocks_finished_legacy(uint8_t status, void *extra) {
	TRACETHIS();
	auto *eptr = static_cast<ChunkserverEntry*>(extra);
	eptr->getBlocksJobId = 0;
	std::vector<uint8_t> buffer;
	cstocs::getChunkBlocksStatus::serialize(
	    buffer, eptr->chunkId, eptr->chunkVersion,
	    (legacy::ChunkPartType)eptr->chunkType, eptr->getBlocksJobResult,
	    status);
	worker_create_attached_packet(eptr, buffer);
	eptr->state = ChunkserverEntry::State::Idle;
}

void worker_sau_get_chunk_blocks_finished(uint8_t status, void *extra) {
	TRACETHIS();
	auto *eptr = static_cast<ChunkserverEntry*>(extra);
	eptr->getBlocksJobId = 0;
	std::vector<uint8_t> buffer;
	cstocs::getChunkBlocksStatus::serialize(buffer, eptr->chunkId,
	                                        eptr->chunkVersion, eptr->chunkType,
	                                        eptr->getBlocksJobResult, status);
	worker_create_attached_packet(eptr, buffer);
	eptr->state = ChunkserverEntry::State::Idle;
}

void worker_get_chunk_blocks_finished(uint8_t status, void *extra) {
	TRACETHIS();
	auto *eptr = static_cast<ChunkserverEntry*>(extra);
	eptr->getBlocksJobId = 0;
	std::vector<uint8_t> buffer;
	serializeLegacyPacket(buffer, CSTOCS_GET_CHUNK_BLOCKS_STATUS, eptr->chunkId,
	                      eptr->chunkVersion, eptr->getBlocksJobResult, status);
	worker_create_attached_packet(eptr, buffer);
	eptr->state = ChunkserverEntry::State::Idle;
}

void worker_sau_get_chunk_blocks(ChunkserverEntry *eptr, const uint8_t *data,
                                 uint32_t length) {
	PacketVersion v;
	deserializePacketVersionNoHeader(data, length, v);
	if (v == cstocs::getChunkBlocks::kECChunks) {
		cstocs::getChunkBlocks::deserialize(
		    data, length, eptr->chunkId, eptr->chunkVersion, eptr->chunkType);

		eptr->getBlocksJobId = job_get_blocks(eptr->workerJobPool,
			worker_sau_get_chunk_blocks_finished, eptr, eptr->chunkId, eptr->chunkVersion,
			eptr->chunkType, &(eptr->getBlocksJobResult));

	} else {
		legacy::ChunkPartType legacy_type;
		cstocs::getChunkBlocks::deserialize(data, length, eptr->chunkId,
		                                    eptr->chunkVersion, legacy_type);
		eptr->chunkType = legacy_type;

		eptr->getBlocksJobId = job_get_blocks(
		    eptr->workerJobPool, worker_sau_get_chunk_blocks_finished_legacy,
		    eptr, eptr->chunkId, eptr->chunkVersion, eptr->chunkType,
		    &(eptr->getBlocksJobResult));
	}
	eptr->state = ChunkserverEntry::State::GetBlock;
}

void worker_get_chunk_blocks(ChunkserverEntry *eptr, const uint8_t *data,
		uint32_t length) {
	deserializeAllLegacyPacketDataNoHeader(data, length, eptr->chunkId,
	                                       eptr->chunkVersion);
	eptr->chunkType = slice_traits::standard::ChunkPartType();
	eptr->getBlocksJobId =
	    job_get_blocks(eptr->workerJobPool, worker_get_chunk_blocks_finished,
	                   eptr, eptr->chunkId, eptr->chunkVersion, eptr->chunkType,
	                   &(eptr->getBlocksJobResult));
	eptr->state = ChunkserverEntry::State::GetBlock;
}

/* IDLE operations */

void worker_hdd_list_v2(ChunkserverEntry *eptr, const uint8_t *data,
		uint32_t length) {
	TRACETHIS();
	uint32_t size;
	uint8_t *ptr;

	(void) data;
	if (length != 0) {
		safs_pretty_syslog(LOG_NOTICE,"CLTOCS_HDD_LIST_V2 - wrong size (%" PRIu32 "/0)",length);
		eptr->state = ChunkserverEntry::State::Close;
		return;
	}
	size = hddGetSerializedSizeOfAllDiskInfosV2(); // lock
	ptr = worker_create_attached_packet(eptr, CSTOCL_HDD_LIST_V2, size);
	hddSerializeAllDiskInfosV2(ptr); // unlock
}

void worker_list_disk_groups(ChunkserverEntry *eptr,
                             [[maybe_unused]] const uint8_t *data,
                             [[maybe_unused]] uint32_t length) {
	TRACETHIS();

	std::string diskGroups = hddGetDiskGroups();

	// 4 bytes for the size of the string + 1 byte for the null character
	static constexpr uint8_t kSerializedSizePlusNullChar = 5;

	uint8_t *ptr = worker_create_attached_packet(
	    eptr, CSTOCL_ADMIN_LIST_DISK_GROUPS,
	    diskGroups.size() + kSerializedSizePlusNullChar);
	serialize(&ptr, diskGroups);
}

void worker_chart(ChunkserverEntry *eptr, const uint8_t *data,
                  uint32_t length) {
	TRACETHIS();
	uint32_t chartid;
	uint8_t *ptr;
	uint32_t l;

	if (length != 4) {
		safs_pretty_syslog(LOG_NOTICE,"CLTOAN_CHART - wrong size (%" PRIu32 "/4)",length);
		eptr->state = ChunkserverEntry::State::Close;
		return;
	}
	chartid = get32bit(&data);
	if(chartid <= CHARTS_CSV_CHARTID_BASE) {
		l = charts_make_png(chartid);
		ptr = worker_create_attached_packet(eptr, ANTOCL_CHART, l);
		if (l > 0) {
			charts_get_png(ptr);
		}
	} else {
		l = charts_make_csv(chartid % CHARTS_CSV_CHARTID_BASE);
		ptr = worker_create_attached_packet(eptr,ANTOCL_CHART,l);
		if (l>0) {
			charts_get_csv(ptr);
		}
	}
}

void worker_chart_data(ChunkserverEntry *eptr, const uint8_t *data,
                       uint32_t length) {
	TRACETHIS();
	uint32_t chartid;
	uint8_t *ptr;
	uint32_t l;

	if (length != 4) {
		safs_pretty_syslog(LOG_NOTICE,"CLTOAN_CHART_DATA - wrong size (%" PRIu32 "/4)",length);
		eptr->state = ChunkserverEntry::State::Close;
		return;
	}
	chartid = get32bit(&data);
	l = charts_datasize(chartid);
	ptr = worker_create_attached_packet(eptr, ANTOCL_CHART_DATA, l);
	if (l > 0) {
		charts_makedata(ptr, chartid);
	}
}

void worker_test_chunk(ChunkserverEntry *eptr, const uint8_t *data,
                       uint32_t length) {
	try {
		PacketVersion v;
		deserializePacketVersionNoHeader(data, length, v);
		ChunkWithVersionAndType chunk;
		if (v == cltocs::testChunk::kECChunks) {
			cltocs::testChunk::deserialize(data, length, chunk.id, chunk.version, chunk.type);
		} else {
			legacy::ChunkPartType legacy_type;
			cltocs::testChunk::deserialize(data, length, chunk.id, chunk.version, legacy_type);
			chunk.type = legacy_type;
		}
		hddAddChunkToTestQueue(chunk);
	} catch (IncorrectDeserializationException &e) {
		safs_pretty_syslog(LOG_NOTICE, "SAU_CLTOCS_TEST_CHUNK - bad packet: %s (length: %" PRIu32 ")",
				e.what(), length);
		eptr->state = ChunkserverEntry::State::Close;
		return;
	}
}

void worker_outputcheck(ChunkserverEntry *eptr) {
	TRACETHIS();
	if (eptr->state == ChunkserverEntry::State::Read) {
		worker_send_finished(eptr);
	}
}

void worker_close(ChunkserverEntry *eptr) {
	TRACETHIS();
	if (eptr->readJobId > 0) {
		job_pool_disable_job(eptr->workerJobPool, eptr->readJobId);
		job_pool_change_callback(eptr->workerJobPool, eptr->readJobId,
		                         worker_delayed_close, eptr);
		eptr->state = ChunkserverEntry::State::CloseWait;
	} else if (eptr->writeJobId > 0) {
		job_pool_disable_job(eptr->workerJobPool, eptr->writeJobId);
		job_pool_change_callback(eptr->workerJobPool, eptr->writeJobId,
		                         worker_delayed_close, eptr);
		eptr->state = ChunkserverEntry::State::CloseWait;
	} else if (eptr->getBlocksJobId > 0) {
		job_pool_disable_job(eptr->workerJobPool, eptr->getBlocksJobId);
		job_pool_change_callback(eptr->workerJobPool, eptr->getBlocksJobId,
		                         worker_delayed_close, eptr);
		eptr->state = ChunkserverEntry::State::CloseWait;
	} else {
		if (eptr->isChunkOpen) {
			job_close(eptr->workerJobPool, nullptr, nullptr, eptr->chunkId,
			          eptr->chunkType);
			eptr->isChunkOpen = 0;
		}
		eptr->state = ChunkserverEntry::State::Closed;
	}
}

void worker_gotpacket(ChunkserverEntry *eptr, uint32_t type,
                      const uint8_t *data, uint32_t length) {
	TRACETHIS();

	if (type == ANTOAN_NOP) {
		return;
	}
	if (type == ANTOAN_UNKNOWN_COMMAND) { // for future use
		return;
	}
	if (type == ANTOAN_BAD_COMMAND_SIZE) { // for future use
		return;
	}
	if (eptr->state == ChunkserverEntry::State::Idle) {
		switch (type) {
		case ANTOAN_PING:
			worker_ping(eptr, data, length);
			break;
		case CLTOCS_READ:
		case SAU_CLTOCS_READ:
			worker_read_init(eptr, data, type, length);
			break;
		case SAU_CLTOCS_PREFETCH:
			worker_prefetch(eptr, data, type, length);
			break;
		case CLTOCS_WRITE:
		case SAU_CLTOCS_WRITE_INIT:
			worker_write_init(eptr, data, type, length);
			break;
		case CSTOCS_GET_CHUNK_BLOCKS:
			worker_get_chunk_blocks(eptr, data, length);
			break;
		case SAU_CSTOCS_GET_CHUNK_BLOCKS:
			worker_sau_get_chunk_blocks(eptr, data, length);
			break;
		case CLTOCS_HDD_LIST_V2:
			worker_hdd_list_v2(eptr, data, length);
			break;
		case CLTOCS_ADMIN_LIST_DISK_GROUPS:
			worker_list_disk_groups(eptr, data, length);
			break;
		case CLTOAN_CHART:
			worker_chart(eptr, data, length);
			break;
		case CLTOAN_CHART_DATA:
			worker_chart_data(eptr, data, length);
			break;
		case SAU_CLTOCS_TEST_CHUNK:
			worker_test_chunk(eptr, data, length);
			break;
		default:
			safs_pretty_syslog(
			    LOG_NOTICE,
			    "Got invalid message in Idle state (type:%" PRIu32 ")", type);
			eptr->state = ChunkserverEntry::State::Close;
			break;
		}
	} else if (eptr->state == ChunkserverEntry::State::WriteLast) {
		switch (type) {
		case CLTOCS_WRITE_DATA:
		case SAU_CLTOCS_WRITE_DATA:
			worker_write_data(eptr, data, type, length);
			break;
		case SAU_CLTOCS_WRITE_END:
			worker_write_end(eptr, data, length);
			break;
		default:
			safs_pretty_syslog(
			    LOG_NOTICE,
			    "Got invalid message in WriteLast state (type:%" PRIu32 ")",
			    type);
			eptr->state = ChunkserverEntry::State::Close;
			break;
		}
	} else if (eptr->state == ChunkserverEntry::State::WriteForward) {
		switch (type) {
		case CLTOCS_WRITE_DATA:
		case SAU_CLTOCS_WRITE_DATA:
			worker_write_data(eptr, data, type, length);
			break;
		case CSTOCL_WRITE_STATUS:
		case SAU_CSTOCL_WRITE_STATUS:
			worker_write_status(eptr, data, type, length);
			break;
		case SAU_CLTOCS_WRITE_END:
			worker_write_end(eptr, data, length);
			break;
		default:
			safs_pretty_syslog(
			    LOG_NOTICE,
			    "Got invalid message in WriteForward state (type:%" PRIu32 ")",
			    type);
			eptr->state = ChunkserverEntry::State::Close;
			break;
		}
	} else if (eptr->state == ChunkserverEntry::State::WriteFinish) {
		switch (type) {
		case CLTOCS_WRITE_DATA:
		case SAU_CLTOCS_WRITE_DATA:
		case SAU_CLTOCS_WRITE_END:
			return;
		default:
			safs_pretty_syslog(
			    LOG_NOTICE,
			    "Got invalid message in WriteFinish state (type:%" PRIu32 ")",
			    type);
			eptr->state = ChunkserverEntry::State::Close;
		}
	} else {
		safs_pretty_syslog(LOG_NOTICE, "Got invalid message (type:%" PRIu32 ")",type);
		eptr->state = ChunkserverEntry::State::Close;
	}
}

void worker_check_nextpacket(ChunkserverEntry *eptr) {
	TRACETHIS();
	uint32_t type, size;
	const uint8_t *ptr;
	if (eptr->state == ChunkserverEntry::State::WriteForward) {
		if (eptr->mode == ChunkserverEntry::Mode::Data &&
		    eptr->inputPacket.bytesLeft == 0 && eptr->fwdBytesLeft == 0) {
			ptr = eptr->headerBuffer;
			type = get32bit(&ptr);
			size = get32bit(&ptr);

			eptr->mode = ChunkserverEntry::Mode::Header;
			eptr->inputPacket.bytesLeft = PacketHeader::kSize;
			eptr->inputPacket.startPtr = eptr->headerBuffer;

			worker_gotpacket(eptr, type,
			                 eptr->inputPacket.packet + PacketHeader::kSize,
			                 size);

			if (eptr->inputPacket.packet) {
				free(eptr->inputPacket.packet);
			}
			eptr->inputPacket.packet = nullptr;
		}
	} else {
		if (eptr->mode == ChunkserverEntry::Mode::Data &&
		    eptr->inputPacket.bytesLeft == 0) {
			ptr = eptr->headerBuffer;
			type = get32bit(&ptr);
			size = get32bit(&ptr);

			eptr->mode = ChunkserverEntry::Mode::Header;
			eptr->inputPacket.bytesLeft = PacketHeader::kSize;
			eptr->inputPacket.startPtr = eptr->headerBuffer;

			worker_gotpacket(eptr, type, eptr->inputPacket.packet, size);

			if (eptr->inputPacket.packet) {
				free(eptr->inputPacket.packet);
			}
			eptr->inputPacket.packet = nullptr;
		}
	}
}

void worker_fwdconnected(ChunkserverEntry *eptr) {
	TRACETHIS();
	int status;
	status = tcpgetstatus(eptr->fwdSocket);
	if (status) {
		safs_silent_errlog(LOG_WARNING, "connection failed, error");
		worker_fwderror(eptr);
		return;
	}
	tcpnodelay(eptr->fwdSocket);
	eptr->state = ChunkserverEntry::State::WriteInit;
}

void worker_fwdread(ChunkserverEntry *eptr) {
	TRACETHIS();
	int32_t i;
	uint32_t type, size;
	const uint8_t *ptr;
	if (eptr->fwdMode == ChunkserverEntry::Mode::Header) {
		i = read(eptr->fwdSocket, eptr->fwdInputPacket.startPtr,
		         eptr->fwdInputPacket.bytesLeft);
		if (i == 0) {
			worker_fwderror(eptr);
			return;
		}
		if (i < 0) {
			if (errno != EAGAIN) {
				safs_silent_errlog(LOG_NOTICE, "(fwdread) read error");
				worker_fwderror(eptr);
			}
			return;
		}
		stats_bytesin += i;
		eptr->fwdInputPacket.startPtr += i;
		eptr->fwdInputPacket.bytesLeft -= i;
		if (eptr->fwdInputPacket.bytesLeft > 0) {
			return;
		}
		ptr = eptr->fwdHeaderBuffer + 4;
		size = get32bit(&ptr);
		if (size > MaxPacketSize) {
			safs_pretty_syslog(LOG_WARNING,"(fwdread) packet too long (%" PRIu32 "/%u)",size,MaxPacketSize);
			worker_fwderror(eptr);
			return;
		}
		if (size > 0) {
			eptr->fwdInputPacket.packet = (uint8_t*) malloc(size);
			passert(eptr->fwdInputPacket.packet);
			eptr->fwdInputPacket.startPtr = eptr->fwdInputPacket.packet;
		}
		eptr->fwdInputPacket.bytesLeft = size;
		eptr->fwdMode = ChunkserverEntry::Mode::Data;
	}
	if (eptr->fwdMode == ChunkserverEntry::Mode::Data) {
		if (eptr->fwdInputPacket.bytesLeft > 0) {
			i = read(eptr->fwdSocket, eptr->fwdInputPacket.startPtr,
			         eptr->fwdInputPacket.bytesLeft);
			if (i == 0) {
				worker_fwderror(eptr);
				return;
			}
			if (i < 0) {
				if (errno != EAGAIN) {
					safs_silent_errlog(LOG_NOTICE, "(fwdread) read error");
					worker_fwderror(eptr);
				}
				return;
			}
			stats_bytesin += i;
			eptr->fwdInputPacket.startPtr += i;
			eptr->fwdInputPacket.bytesLeft -= i;
			if (eptr->fwdInputPacket.bytesLeft > 0) {
				return;
			}
		}
		ptr = eptr->fwdHeaderBuffer;
		type = get32bit(&ptr);
		size = get32bit(&ptr);

		eptr->fwdMode = ChunkserverEntry::Mode::Header;
		eptr->fwdInputPacket.bytesLeft = PacketHeader::kSize;
		eptr->fwdInputPacket.startPtr = eptr->fwdHeaderBuffer;

		worker_gotpacket(eptr, type, eptr->fwdInputPacket.packet, size);

		if (eptr->fwdInputPacket.packet) {
			free(eptr->fwdInputPacket.packet);
		}
		eptr->fwdInputPacket.packet = nullptr;
	}
}

void worker_fwdwrite(ChunkserverEntry *eptr) {
	TRACETHIS();
	int32_t i;
	if (eptr->fwdBytesLeft > 0) {
		i = write(eptr->fwdSocket, eptr->fwdStartPtr, eptr->fwdBytesLeft);
		if (i == 0) {
			worker_fwderror(eptr);
			return;
		}
		if (i < 0) {
			if (errno != EAGAIN) {
				safs_silent_errlog(LOG_NOTICE, "(fwdwrite) write error");
				worker_fwderror(eptr);
			}
			return;
		}
		stats_bytesout += i;
		eptr->fwdStartPtr += i;
		eptr->fwdBytesLeft -= i;
	}
	if (eptr->fwdBytesLeft == 0) {
		eptr->fwdInitPacket.clear();
		eptr->fwdStartPtr = nullptr;
		eptr->fwdMode = ChunkserverEntry::Mode::Header;
		eptr->fwdInputPacket.bytesLeft = PacketHeader::kSize;
		eptr->fwdInputPacket.startPtr = eptr->fwdHeaderBuffer;
		eptr->fwdInputPacket.packet = nullptr;
		eptr->state = ChunkserverEntry::State::WriteForward;
	}
}

void worker_forward(ChunkserverEntry *eptr) {
	TRACETHIS();
	int32_t i;
	if (eptr->mode == ChunkserverEntry::Mode::Header) {
		i = read(eptr->sock, eptr->inputPacket.startPtr,
		         eptr->inputPacket.bytesLeft);
		if (i == 0) {
			eptr->state = ChunkserverEntry::State::Close;
			return;
		}
		if (i < 0) {
			if (errno != EAGAIN) {
				safs_silent_errlog(LOG_NOTICE, "(forward) read error");
				eptr->state = ChunkserverEntry::State::Close;
			}
			return;
		}
		stats_bytesin += i;
		eptr->inputPacket.startPtr += i;
		eptr->inputPacket.bytesLeft -= i;
		if (eptr->inputPacket.bytesLeft > 0) {
			return;
		}
		PacketHeader header;
		try {
			deserializePacketHeader(eptr->headerBuffer,
			                        sizeof(eptr->headerBuffer), header);
		} catch (IncorrectDeserializationException&) {
			safs_pretty_syslog(LOG_WARNING, "(forward) Received malformed network packet");
			eptr->state = ChunkserverEntry::State::Close;
			return;
		}
		if (header.length > MaxPacketSize) {
			safs_pretty_syslog(LOG_WARNING,"(forward) packet too long (%" PRIu32 "/%u)",
					header.length, MaxPacketSize);
			eptr->state = ChunkserverEntry::State::Close;
			return;
		}
		uint32_t totalPacketLength = PacketHeader::kSize + header.length;
		if (eptr->inputPacket.packet) {
			free(eptr->inputPacket.packet);
		}
		eptr->inputPacket.packet = static_cast<uint8_t*>(malloc(totalPacketLength));
		passert(eptr->inputPacket.packet);
		memcpy(eptr->inputPacket.packet, eptr->headerBuffer,
		       PacketHeader::kSize);
		eptr->inputPacket.bytesLeft = header.length;
		eptr->inputPacket.startPtr = eptr->inputPacket.packet + PacketHeader::kSize;
		if (header.type == CLTOCS_WRITE_DATA
				|| header.type == SAU_CLTOCS_WRITE_DATA
				|| header.type == SAU_CLTOCS_WRITE_END) {
			eptr->fwdBytesLeft = PacketHeader::kSize;
			eptr->fwdStartPtr = eptr->inputPacket.packet;
		}
		eptr->mode = ChunkserverEntry::Mode::Data;
	}
	if (eptr->inputPacket.bytesLeft > 0) {
		i = read(eptr->sock, eptr->inputPacket.startPtr,
		         eptr->inputPacket.bytesLeft);
		if (i == 0) {
			eptr->state = ChunkserverEntry::State::Close;
			return;
		}
		if (i < 0) {
			if (errno != EAGAIN) {
				safs_silent_errlog(LOG_NOTICE, "(forward) read error");
				eptr->state = ChunkserverEntry::State::Close;
			}
			return;
		}
		stats_bytesin += i;
		eptr->inputPacket.startPtr += i;
		eptr->inputPacket.bytesLeft -= i;
		if (eptr->fwdStartPtr != nullptr) {
			eptr->fwdBytesLeft += i;
		}
	}
	if (eptr->fwdBytesLeft > 0) {
		sassert(eptr->fwdStartPtr != nullptr);
		i = write(eptr->fwdSocket, eptr->fwdStartPtr, eptr->fwdBytesLeft);
		if (i == 0) {
			worker_fwderror(eptr);
			return;
		}
		if (i < 0) {
			if (errno != EAGAIN) {
				safs_silent_errlog(LOG_NOTICE, "(forward) write error");
				worker_fwderror(eptr);
			}
			return;
		}
		stats_bytesout += i;
		eptr->fwdStartPtr += i;
		eptr->fwdBytesLeft -= i;
	}
	if (eptr->inputPacket.bytesLeft == 0 && eptr->fwdBytesLeft == 0 &&
	    eptr->writeJobId == 0) {
		PacketHeader header;
		try {
			deserializePacketHeader(eptr->headerBuffer,
			                        sizeof(eptr->headerBuffer), header);
		} catch (IncorrectDeserializationException&) {
			safs_pretty_syslog(LOG_WARNING, "(forward) Received malformed network packet");
			eptr->state = ChunkserverEntry::State::Close;
			return;
		}
		eptr->mode = ChunkserverEntry::Mode::Header;
		eptr->inputPacket.bytesLeft = PacketHeader::kSize;
		eptr->inputPacket.startPtr = eptr->headerBuffer;

		uint8_t* packetData = eptr->inputPacket.packet + PacketHeader::kSize;
		worker_gotpacket(eptr, header.type, packetData, header.length);
		if (eptr->inputPacket.packet) {
			free(eptr->inputPacket.packet);
		}
		eptr->inputPacket.packet = nullptr;
		eptr->fwdStartPtr = nullptr;
	}
}

void worker_read(ChunkserverEntry *eptr) {
	TRACETHIS();
	int32_t i;
	uint32_t type, size;
	const uint8_t *ptr;

	if (eptr->mode == ChunkserverEntry::Mode::Header) {
		sassert(eptr->inputPacket.startPtr + eptr->inputPacket.bytesLeft ==
		        eptr->headerBuffer + PacketHeader::kSize);
		i = read(eptr->sock, eptr->inputPacket.startPtr,
		         eptr->inputPacket.bytesLeft);
		if (i == 0) {
			eptr->state = ChunkserverEntry::State::Close;
			return;
		}
		if (i < 0) {
			if (errno != EAGAIN) {
				safs_silent_errlog(LOG_NOTICE, "(read) read error");
				eptr->state = ChunkserverEntry::State::Close;
			}
			return;
		}
		stats_bytesin += i;
		eptr->inputPacket.startPtr += i;
		eptr->inputPacket.bytesLeft -= i;

		if (eptr->inputPacket.bytesLeft > 0) {
			return;
		}

		ptr = eptr->headerBuffer + 4;
		size = get32bit(&ptr);

		if (size > 0) {
			if (size > MaxPacketSize) {
				safs_pretty_syslog(LOG_WARNING,"(read) packet too long (%" PRIu32 "/%u)",size,MaxPacketSize);
				eptr->state = ChunkserverEntry::State::Close;
				return;
			}
			if (eptr->inputPacket.packet) {
				free(eptr->inputPacket.packet);
			}
			eptr->inputPacket.packet = (uint8_t*) malloc(size);
			passert(eptr->inputPacket.packet);
			eptr->inputPacket.startPtr = eptr->inputPacket.packet;
		}
		eptr->inputPacket.bytesLeft = size;
		eptr->mode = ChunkserverEntry::Mode::Data;
	}
	if (eptr->mode == ChunkserverEntry::Mode::Data) {
		if (eptr->inputPacket.bytesLeft > 0) {
			i = read(eptr->sock, eptr->inputPacket.startPtr,
			         eptr->inputPacket.bytesLeft);
			if (i == 0) {
				eptr->state = ChunkserverEntry::State::Close;
				return;
			}
			if (i < 0) {
				if (errno != EAGAIN) {
					safs_silent_errlog(LOG_NOTICE, "(read) read error");
					eptr->state = ChunkserverEntry::State::Close;
				}
				return;
			}
			stats_bytesin += i;
			eptr->inputPacket.startPtr += i;
			eptr->inputPacket.bytesLeft -= i;

			if (eptr->inputPacket.bytesLeft > 0) {
				return;
			}
		}
		if (eptr->writeJobId == 0) {
			ptr = eptr->headerBuffer;
			type = get32bit(&ptr);
			size = get32bit(&ptr);

			eptr->mode = ChunkserverEntry::Mode::Header;
			eptr->inputPacket.bytesLeft = PacketHeader::kSize;
			eptr->inputPacket.startPtr = eptr->headerBuffer;

			worker_gotpacket(eptr, type, eptr->inputPacket.packet, size);

			if (eptr->inputPacket.packet) {
				free(eptr->inputPacket.packet);
			}
			eptr->inputPacket.packet = nullptr;
		}
	}
}

void worker_write(ChunkserverEntry *eptr) {
	TRACETHIS();
	PacketStruct *pack = nullptr;
	int32_t i;
	for (;;) {
		pack = eptr->outputPackets.front().get();
		if (pack == nullptr) {
			return;
		}
		if (pack->outputBuffer) {
			size_t bytesInBufferBefore = pack->outputBuffer->bytesInABuffer();
			OutputBuffer::WriteStatus ret = pack->outputBuffer->writeOutToAFileDescriptor(eptr->sock);
			size_t bytesInBufferAfter = pack->outputBuffer->bytesInABuffer();
			massert(bytesInBufferAfter <= bytesInBufferBefore,
					"New bytes in pack->outputBuffer after sending some data");
			stats_bytesout += (bytesInBufferBefore - bytesInBufferAfter);
			if (ret == OutputBuffer::WRITE_ERROR) {
				safs_silent_errlog(LOG_NOTICE, "(write) write error");
				eptr->state = ChunkserverEntry::State::Close;
				return;
			} else if (ret == OutputBuffer::WRITE_AGAIN) {
				return;
			}
		} else {
			i = write(eptr->sock, pack->startPtr, pack->bytesLeft);
			if (i == 0) {
				eptr->state = ChunkserverEntry::State::Close;
				return;
			}
			if (i < 0) {
				if (errno != EAGAIN) {
					safs_silent_errlog(LOG_NOTICE, "(write) write error");
					eptr->state = ChunkserverEntry::State::Close;
				}
				return;
			}
			stats_bytesout += i;
			pack->startPtr += i;
			pack->bytesLeft -= i;
			if (pack->bytesLeft > 0) {
				return;
			}
		}
		// packet has been sent
		if (pack->outputBuffer) {
			getReadOutputBufferPool().put(std::move(pack->outputBuffer));
		}
		free(pack->packet);
		eptr->outputPackets.pop_front();
		worker_outputcheck(eptr);
	}
}

NetworkWorkerThread::NetworkWorkerThread(uint32_t nrOfBgjobsWorkers, uint32_t bgjobsCount)
		: doTerminate(false) {
	TRACETHIS();
	eassert(pipe(notify_pipe) != -1);
#ifdef F_SETPIPE_SZ
	eassert(fcntl(notify_pipe[1], F_SETPIPE_SZ, 4096*32));
#endif
	bgJobPool_ = job_pool_new(nrOfBgjobsWorkers, bgjobsCount, &bgJobPoolWakeUpFd_);
}

void NetworkWorkerThread::operator()() {
	TRACETHIS();

	static std::atomic_uint16_t threadCounter(0);
	std::string threadName = "networkWorker " + std::to_string(threadCounter++);
	pthread_setname_np(pthread_self(), threadName.c_str());

	while (!doTerminate) {
		preparePollFds();
		int i = poll(pdesc.data(), pdesc.size(), 50);
		if (i < 0) {
			if (errno == EAGAIN) {
				safs_pretty_syslog(LOG_WARNING, "poll returned EAGAIN");
				usleep(100000);
				continue;
			}
			if (errno != EINTR) {
				safs_pretty_syslog(LOG_WARNING, "poll error: %s", strerr(errno));
				break;
			}
		} else {
			if ((pdesc[0].revents) & POLLIN) {
				uint8_t notifyByte;
				eassert(read(pdesc[0].fd, &notifyByte, 1) == 1);
			}
		}
		servePoll();
	}
	this->terminate();
}

void NetworkWorkerThread::terminate() {
	TRACETHIS();
	job_pool_delete(bgJobPool_);
	std::unique_lock lock(csservheadLock);
	while (!csservEntries.empty()) {
		auto& entry = csservEntries.back();
		if (entry.isChunkOpen) {
			hddClose(entry.chunkId, entry.chunkType);
		}
		tcpclose(entry.sock);
		if (entry.fwdSocket >= 0) {
			tcpclose(entry.fwdSocket);
		}
		if (entry.inputPacket.packet) {
			free(entry.inputPacket.packet);
		}
		if (entry.writePacket) {
			ChunkserverEntry::deletePreservedPacket(entry.writePacket);
		}
		if (entry.fwdInputPacket.packet) {
			free(entry.fwdInputPacket.packet);
		}
		for (auto& outputPacket : entry.outputPackets) {
			if (outputPacket->packet) {
				free(outputPacket->packet);
			}
		}
		entry.outputPackets.clear();
		csservEntries.pop_back();
	}
}

void NetworkWorkerThread::preparePollFds() {
	LOG_AVG_TILL_END_OF_SCOPE0("preparePollFds");
	TRACETHIS();
	pdesc.clear();
	pdesc.emplace_back();
	pdesc.back().fd = notify_pipe[0];
	pdesc.back().events = POLLIN;
	pdesc.emplace_back();
	pdesc.back().fd = bgJobPoolWakeUpFd_;
	pdesc.back().events = POLLIN;
	sassert(JOB_FD_PDESC_POS == (pdesc.size() - 1));

	std::unique_lock lock(csservheadLock);
	for (auto& entry : csservEntries) {
		entry.pDescPos = -1;
		entry.fwdPDescPos = -1;
		switch (entry.state) {
			case ChunkserverEntry::State::Idle:
			case ChunkserverEntry::State::Read:
			case ChunkserverEntry::State::GetBlock:
			case ChunkserverEntry::State::WriteLast:
				pdesc.emplace_back();
				pdesc.back().fd = entry.sock;
				pdesc.back().events = 0;
				entry.pDescPos = pdesc.size() - 1;
				if (entry.inputPacket.bytesLeft > 0) {
					pdesc.back().events |= POLLIN;
				}
				if (!entry.outputPackets.empty()) {
					pdesc.back().events |= POLLOUT;
				}
				break;
			case ChunkserverEntry::State::Connecting:
				pdesc.emplace_back();
				pdesc.back().fd = entry.fwdSocket;
				pdesc.back().events = POLLOUT;
				entry.fwdPDescPos = pdesc.size() - 1;
				break;
			case ChunkserverEntry::State::WriteInit:
				if (entry.fwdBytesLeft > 0) {
					pdesc.emplace_back();
					pdesc.back().fd = entry.fwdSocket;
					pdesc.back().events = POLLOUT;
					entry.fwdPDescPos = pdesc.size() - 1;
				}
				break;
			case ChunkserverEntry::State::WriteForward:
				pdesc.emplace_back();
				pdesc.back().fd = entry.fwdSocket;
				pdesc.back().events = POLLIN;
				entry.fwdPDescPos = pdesc.size() - 1;
				if (entry.fwdBytesLeft > 0) {
					pdesc.back().events |= POLLOUT;
				}

				pdesc.emplace_back();
				pdesc.back().fd = entry.sock;
				pdesc.back().events = 0;
				entry.pDescPos = pdesc.size() - 1;
				if (entry.inputPacket.bytesLeft > 0) {
					pdesc.back().events |= POLLIN;
				}
				if (!entry.outputPackets.empty()) {
					pdesc.back().events |= POLLOUT;
				}
				break;
			case ChunkserverEntry::State::WriteFinish:
				if (!entry.outputPackets.empty()) {
					pdesc.emplace_back();
					pdesc.back().fd = entry.sock;
					pdesc.back().events = POLLOUT;
					entry.pDescPos = pdesc.size() - 1;
				}
				break;
			default:
				break;
		}
	}
}

void NetworkWorkerThread::servePoll() {
	LOG_AVG_TILL_END_OF_SCOPE0("servePoll");
	TRACETHIS();
	uint32_t now = eventloop_time();
	uint64_t usecnow = eventloop_utime();
	uint32_t jobscnt;
	ChunkserverEntry::State lstate;

	if (pdesc[JOB_FD_PDESC_POS].revents & POLLIN) {
		job_pool_check_jobs(bgJobPool_);
	}
	std::unique_lock lock(csservheadLock);
	for (auto& entry : csservEntries) {
		ChunkserverEntry* eptr = &entry;
		if (entry.pDescPos >= 0
				&& (pdesc[entry.pDescPos].revents & (POLLERR | POLLHUP))) {
			entry.state = ChunkserverEntry::State::Close;
		} else if (entry.fwdPDescPos >= 0
				&& (pdesc[entry.fwdPDescPos].revents & (POLLERR | POLLHUP))) {
			worker_fwderror(eptr);
		}
		lstate = entry.state;
		if (lstate == ChunkserverEntry::State::Idle ||
		    lstate == ChunkserverEntry::State::Read ||
		    lstate == ChunkserverEntry::State::WriteLast ||
		    lstate == ChunkserverEntry::State::WriteFinish ||
		    lstate == ChunkserverEntry::State::GetBlock) {
			if (entry.pDescPos >= 0 &&
			    (pdesc[entry.pDescPos].revents & POLLIN)) {
				entry.lastActivity = now;
				worker_read(eptr);
			}
			if (entry.pDescPos >= 0 &&
			    (pdesc[entry.pDescPos].revents & POLLOUT) &&
			    entry.state == lstate) {
				entry.lastActivity = now;
				worker_write(eptr);
			}
		} else if (lstate == ChunkserverEntry::State::Connecting &&
		           entry.fwdPDescPos >= 0 &&
		           (pdesc[entry.fwdPDescPos].revents &
		            POLLOUT)) {  // FD_ISSET(entry.fwdsock,wset)) {
			entry.lastActivity = now;
			worker_fwdconnected(eptr);
			if (entry.state == ChunkserverEntry::State::WriteInit) {
				worker_fwdwrite(eptr); // after connect likely some data can be send
			}
			if (entry.state == ChunkserverEntry::State::WriteForward) {
				worker_forward(eptr); // and also some data can be forwarded
			}
		} else if (entry.state == ChunkserverEntry::State::WriteInit &&
		           entry.fwdPDescPos >= 0 &&
		           (pdesc[entry.fwdPDescPos].revents &
		            POLLOUT)) {  // FD_ISSET(entry.fwdsock,wset)) {
			entry.lastActivity = now;
			worker_fwdwrite(eptr); // after sending init packet
			if (entry.state == ChunkserverEntry::State::WriteForward) {
				worker_forward(eptr); // likely some data can be forwarded
			}
		} else if (entry.state == ChunkserverEntry::State::WriteForward) {
			if ((entry.pDescPos >= 0 &&
			     (pdesc[entry.pDescPos].revents & POLLIN)) ||
			    (entry.fwdPDescPos >= 0 &&
			     (pdesc[entry.fwdPDescPos].revents & POLLOUT))) {
				entry.lastActivity = now;
				worker_forward(eptr);
			}
			if (entry.fwdPDescPos >= 0 &&
			    (pdesc[entry.fwdPDescPos].revents & POLLIN) &&
			    entry.state == lstate) {
				entry.lastActivity = now;
				worker_fwdread(eptr);
			}
			if (entry.pDescPos >= 0 &&
			    (pdesc[entry.pDescPos].revents & POLLOUT) &&
			    entry.state == lstate) {
				entry.lastActivity = now;
				worker_write(eptr);
			}
		}
		if (entry.state == ChunkserverEntry::State::WriteFinish &&
		    entry.outputPackets.empty()) {
			entry.state = ChunkserverEntry::State::Close;
		}
		if (entry.state == ChunkserverEntry::State::Connecting &&
		    entry.connectStartTimeUSec +
		            CONNECT_TIMEOUT(entry.connectRetryCounter) <
		        usecnow) {
			worker_retryconnect(eptr);
		}
		if (entry.state != ChunkserverEntry::State::Close &&
		    entry.state != ChunkserverEntry::State::CloseWait &&
		    entry.state != ChunkserverEntry::State::Closed &&
		    entry.lastActivity + CSSERV_TIMEOUT < now) {
			// Close connection if inactive for more than CSSERV_TIMEOUT seconds
			entry.state = ChunkserverEntry::State::Close;
		}
		if (entry.state == ChunkserverEntry::State::Close) {
			worker_close(eptr);
		}
	}

	jobscnt = job_pool_jobs_count(bgJobPool_);
//      // Lock free stats_maxjobscnt = max(stats_maxjobscnt, jobscnt), but I don't trust myself :(...
//      uint32_t expected_value = stats_maxjobscnt;
//      while (jobscnt > expected_value
//                      && !stats_maxjobscnt.compare_exchange_strong(expected_value, jobscnt)) {
//              expected_value = stats_maxjobscnt;
//      }
// // .. Will end up with a racy code instead :(
	if (jobscnt > stats_maxjobscnt) {
		// A race is possible here, but it won't lead to any serious consequences, in a worst
		// (and unlikely) case stats_maxjobscnt will be slightly lower than it actually should be
		stats_maxjobscnt = jobscnt;
	}

	auto eptr = csservEntries.begin();
	while (eptr != csservEntries.end()) {
		if (eptr->state == ChunkserverEntry::State::Closed) {
			tcpclose(eptr->sock);
			if (eptr->readPacket) {
				eptr->releasePacketResources(eptr->readPacket);
				eptr->readPacket.reset();
			}
			if (eptr->writePacket) {
				ChunkserverEntry::deletePreservedPacket(eptr->writePacket);
			}
			if (eptr->fwdSocket >= 0) {
				tcpclose(eptr->fwdSocket);
			}
			if (eptr->inputPacket.packet) {
				free(eptr->inputPacket.packet);
			}
			if (eptr->fwdInputPacket.packet) {
				free(eptr->fwdInputPacket.packet);
			}
			for (auto& outputPacket : eptr->outputPackets) {
				if (outputPacket->packet) {
					free(outputPacket->packet);
				}
			}
			eptr->outputPackets.clear();
			eptr = csservEntries.erase(eptr);
		} else {
			++eptr;
		}
	}
}

void NetworkWorkerThread::askForTermination() {
	TRACETHIS();
	doTerminate = true;
}

void NetworkWorkerThread::addConnection(int newSocketFD) {
	TRACETHIS();
	tcpnonblock(newSocketFD);
	tcpnodelay(newSocketFD);

	std::unique_lock lock(csservheadLock);
	csservEntries.emplace_front(newSocketFD, bgJobPool_);
	csservEntries.front().lastActivity = eventloop_time();

	eassert(write(notify_pipe[1], "9", 1) == 1);
}
