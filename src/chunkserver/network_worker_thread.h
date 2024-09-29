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

#pragma once

#include "common/platform.h"

#include <inttypes.h>
#include <atomic>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <set>
#include <vector>

#include "chunkserver/network_stats.h"
#include "chunkserver/output_buffer.h"
#include "common/chunk_part_type.h"
#include "common/network_address.h"
#include "common/slice_traits.h"
#include "protocol/packet.h"
#include "devtools/request_log.h"

/**
 * @brief Encapsulates the data associated with a packet.
 *
 * Including pointers to the packet data, the number of bytes left to process,
 * and an optional output buffer for writing data.
 */
struct PacketStruct {
	PacketStruct *next = nullptr;
	uint8_t *startPtr = nullptr;
	uint32_t bytesLeft = 0;
	uint8_t *packet = nullptr;
	std::shared_ptr<OutputBuffer> outputBuffer;
};

class MessageSerializer;

/**
 * @brief Represents a single connection to a chunkserver.
 *
 * This struct manages the state and data associated with a connection to a
 * chunkserver. It includes information about the connection's state, mode,
 * sockets, and various buffers used for reading and writing data. It also
 * maintains metadata for managing the connection's lifecycle and handling
 * retries, timeouts, and partial writes.
 *
 * @details
 * The `ChunkserverEntry` struct is used extensively within the
 * `NetworkWorkerThread` to manage connections. It supports both reading and
 * writing operations, including forwarding data to other chunkservers in a
 * write chain. The struct also tracks job IDs and partially completed writes to
 * ensure data consistency and proper error handling.
 */
struct ChunkserverEntry {
	/// The possible modes of a `ChunkserverEntry`.
	enum class Mode : uint8_t {
		Header,  // reading packet header
		Data     // reading packet data
	};

	/// The possible connection states of a `ChunkserverEntry`.
	enum class State : uint8_t {
		Idle,        // idle connection, new or used previously
		Read,        // after CLTOCS_READ, but didn't send all of the
		             // CSTOCL_READ_(DATA|STAUS)
		GetBlock,    // after CSTOCS_GET_CHUNK_BLOCKS, but didn't send response
		WriteLast,   // ready for writing data; data not forwarded to other CSs
		Connecting,  // connecting to other chunkserver to form a writing chain
		WriteInit,   // sending packet forming a chain to the next chunkserver
		WriteForward,  // ready for writing data; will be forwarded to other CSs
		WriteFinish,   // write error, will be closed after sending error status
		Close,         // close request, will change to CloseWait or Closed
		CloseWait,  // waits for a worker to finish a job, then will be Closed
		Closed      // ready to be deleted
	};

	void* workerJobPool; // Job pool assigned to a given network worker thread

	ChunkserverEntry::State state = ChunkserverEntry::State::Idle;
	ChunkserverEntry::Mode mode = ChunkserverEntry::Mode::Header;
	ChunkserverEntry::Mode fwdMode = ChunkserverEntry::Mode::Header;

	int sock;
	int fwdSocket = -1; ///< forwarding socket for writing
	uint64_t connectStartTimeUSec = 0; ///< for timeout and retry (usec)
	uint8_t connectRetryCounter = 0; ///< for timeout and retry
	NetworkAddress fwdServer; // the next server in write chain
	int32_t pDescPos = -1;  ///< Position in the poll descriptors array
	int32_t fwdPDescPos = -1;  ///< Position in poll descriptors for fwdSocket
	uint32_t lastActivity = 0; ///< Last activity time
	uint8_t headerBuffer[PacketHeader::kSize]{};  ///< buffer for packet header
	uint8_t fwdHeaderBuffer[PacketHeader::kSize]{};  ///< fwd packet header buff
	/// Stores the data of the incoming packet for processing
	PacketStruct inputPacket;
	uint8_t *fwdStartPtr = nullptr; ///< used for forwarding inputpacket data
	uint32_t fwdBytesLeft = 0; ///< used for forwarding inputpacket data
	PacketStruct fwdInputPacket; ///< used for receiving status from fwdSocket
	std::vector<uint8_t> fwdInitPacket; ///< used only for write initialization

	/// List of output packets waiting to be sent to the clients
	std::list<std::unique_ptr<PacketStruct>> outputPackets;

	/* write */
	uint32_t writeJobId = 0; ///< ID of the current write job being processed
	uint32_t writeJobWriteId = 0; ///< Specific write operation from client
	/// writeJobWriteId's which:
	/// - have been completed by our worker, but need ack from the next
	///   chunkserver from the chain.
	/// - have been acked by the next chunkserver from the chain, but are still
	///   being written by us.
	std::set<uint32_t> partiallyCompletedWrites;

	/* read */
	uint32_t readJobId = 0; ///< ID of the current read job being processed.
	uint8_t todoReadCounter = 0; ///< R (read finished + send finished)

	/* get blocks */
	uint32_t getBlocksJobId = 0; ///< Current job ID for retrieving chunk blocks
	uint16_t getBlocksJobResult = 0; ///< Result of the get blocks job

	/* common for read and write but meaning is different !!! */
	std::unique_ptr<PacketStruct> readPacket = nullptr;
	std::unique_ptr<PacketStruct> writePacket =
	    std::make_unique<PacketStruct>();

	uint8_t isChunkOpen = 0;
	uint64_t chunkId = 0; // R+W
	uint32_t chunkVersion = 0; // R+W
	ChunkPartType chunkType = slice_traits::standard::ChunkPartType(); // R
	uint32_t offset = 0; ///< R: Offset within the chunk for the operation.
	uint32_t size = 0; ///< R: Size of the current operation.

	/// Pointer to the concrete serializer singleton.
	/// Serializers coud be of type:
	/// - LegacyMessageSerializer: for legacy messages
	/// - SaunaFsMessageSerializer: for new messages
	MessageSerializer* messageSerializer = nullptr; // R+W

	LOG_AVG_TYPE readOperationTimer;

	ChunkserverEntry(int socket, void *workerJobPool)
	    : workerJobPool(workerJobPool), sock(socket) {
		inputPacket.bytesLeft = PacketHeader::kSize;
		inputPacket.startPtr = headerBuffer;
		inputPacket.packet = nullptr;
	}

	// Disallow copying and moving to avoid misuse.
	ChunkserverEntry(const ChunkserverEntry &) = delete;
	ChunkserverEntry &operator=(const ChunkserverEntry &) = delete;
	ChunkserverEntry(ChunkserverEntry &&) = delete;
	ChunkserverEntry &operator=(ChunkserverEntry &&) = delete;

	~ChunkserverEntry() = default;

	/// Attaches a packet to the output packet list (taking ownership).
	inline void attachPacket(std::unique_ptr<PacketStruct> &&packet);
	/// Releases the packet resources, primarily the packet buffer.
	/// This function should be used until the code is refactored to use RAII.
	inline void releasePacketResources(std::unique_ptr<PacketStruct> &packet);
	/// Preserves the inputPacket buffer into writePacket (to avoid copying it).
	/// Used for write operations, where the data comes from the network.
	inline void preserveInputPacket();
	/// Releases the preserved packet resources.
	static inline void deletePreservedPacket(
	    std::unique_ptr<PacketStruct> &packet);
};

class NetworkWorkerThread {
public:
	static constexpr uint32_t kDefaultNumberOfNetworkWorkers = 4;
	static constexpr uint32_t kDefaultNumberOfHddWorkersPerNetworkWorker = 16;
	static constexpr uint32_t kDefaultMaxBackgroundJobsPerNetworkWorker = 1000;

	NetworkWorkerThread(uint32_t nrOfBgjobsWorkers, uint32_t bgjobsCount);
	NetworkWorkerThread(const NetworkWorkerThread&) = delete;

	// main loop
	void operator()();
	void askForTermination();
	void addConnection(int newSocketFD);
	void* bgJobPool() {
		return bgJobPool_;
	}

private:
	void preparePollFds();
	void servePoll() ;
	void terminate();

	std::atomic<bool> doTerminate;
	std::mutex csservheadLock;
	std::list<ChunkserverEntry> csservEntries;

	void *bgJobPool_;
	int bgJobPoolWakeUpFd_;
	static const uint32_t JOB_FD_PDESC_POS = 1;
	std::vector<struct pollfd> pdesc;
	int notify_pipe[2];
};

