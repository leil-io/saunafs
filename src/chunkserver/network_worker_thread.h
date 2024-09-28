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
#include <list>
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

//entry.mode
enum ChunkserverEntryMode {
	HEADER, DATA
};

struct packetstruct {
	packetstruct *next;
	uint8_t *startptr;
	uint32_t bytesleft;
	uint8_t *packet;
	std::shared_ptr<OutputBuffer> outputBuffer;

	packetstruct() : next(nullptr), startptr(nullptr), bytesleft(0), packet(nullptr) {
	}
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
	uint8_t mode;
	uint8_t fwdmode;

	int sock;
	int fwdsock; // forwarding socket for writing
	uint64_t connstart; // 'connect' start time in usec (for timeout and retry)
	uint8_t connretrycnt; // 'connect' retry counter
	NetworkAddress fwdServer; // the next server in write chain
	int32_t pdescpos;
	int32_t fwdpdescpos;
	uint32_t activity;
	uint8_t hdrbuff[PacketHeader::kSize];
	uint8_t fwdhdrbuff[PacketHeader::kSize];
	packetstruct inputpacket;
	uint8_t *fwdstartptr; // used for forwarding inputpacket data
	uint32_t fwdbytesleft; // used for forwarding inputpacket data
	packetstruct fwdinputpacket; // used for receiving status from fwdsocket
	std::vector<uint8_t> fwdinitpacket; // used only for write initialization
	packetstruct *outputhead, **outputtail;

	/* write */
	uint32_t wjobid;
	uint32_t wjobwriteid;
	std::set<uint32_t> partiallyCompletedWrites; // writeId's which:
	// * have been completed by our worker, but need ack from the next chunkserver from the chain
	// * have been acked by the next chunkserver from the chain, but are still being written by us

	/* read */
	uint32_t rjobid;
	uint8_t todocnt; // R (read finished + send finished)

	/* get blocks */
	uint32_t getBlocksJobId;
	uint16_t getBlocksJobResult;

	/* common for read and write but meaning is different !!! */
	void *rpacket;
	void *wpacket;

	uint8_t chunkisopen;
	uint64_t chunkid; // R+W
	uint32_t version; // R+W
	ChunkPartType chunkType; // R
	uint32_t offset; // R
	uint32_t size; // R
	MessageSerializer* messageSerializer; // R+W

	LOG_AVG_TYPE readOperationTimer;

	ChunkserverEntry(int socket, void* workerJobPool)
			: workerJobPool(workerJobPool),
			  mode(HEADER),
			  fwdmode(HEADER),
			  sock(socket),
			  fwdsock(-1),
			  connstart(0),
			  connretrycnt(0),
			  pdescpos(-1),
			  fwdpdescpos(-1),
			  activity(0),
			  fwdstartptr(NULL),
			  fwdbytesleft(0),
			  outputhead(nullptr),
			  outputtail(&outputhead),
			  wjobid(0),
			  wjobwriteid(0),
			  rjobid(0),
			  todocnt(0),
			  getBlocksJobId(0),
			  getBlocksJobResult(0),
			  rpacket(nullptr),
			  wpacket(nullptr),
			  chunkisopen(0),
			  chunkid(0),
			  version(0),
			  chunkType(slice_traits::standard::ChunkPartType()),
			  offset(0),
			  size(0),
			  messageSerializer(nullptr) {
		inputpacket.bytesleft = 8;
		inputpacket.startptr = hdrbuff;
		inputpacket.packet = NULL;
	}

	ChunkserverEntry(const ChunkserverEntry&) = delete;
	ChunkserverEntry(ChunkserverEntry&&) = default;
	ChunkserverEntry& operator=(const ChunkserverEntry&) = delete;
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

