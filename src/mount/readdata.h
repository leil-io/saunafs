/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA
   Copyright 2013-2014 EditShare
   Copyright 2013-2015 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ


   SaunaFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   SaunaFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with SaunaFS  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "common/platform.h"

#include <cinttypes>
#include <list>
#include <queue>

#include "mount/chunk_locator.h"
#include "mount/chunk_reader.h"
#include "mount/readahead_adviser.h"
#include "mount/readdata_cache.h"

inline std::atomic<uint32_t> gReadaheadMaxWindowSize;
inline std::atomic<uint32_t> gCacheExpirationTime_ms;
inline std::atomic<uint32_t> gMaxReadaheadRequests;
inline std::atomic<uint64_t> gReadCacheMaxSize;

enum class ReadaheadRequestState {
	kInqueued,
	kProcessing,
	kDiscarded,
	kFinished
};

// The ```ReadaheadRequestEntry``` struct contains the data related to a
// readahead request: the cache's entry where the data is stored, current state
// and final error code of the request.
struct ReadaheadRequestEntry {
	ReadCache::Entry *entry;
	ReadaheadRequestState state;
	int error_code;

	ReadaheadRequestEntry(
	    ReadCache::Entry *_entry,
	    ReadaheadRequestState _state = ReadaheadRequestState::kInqueued,
	    int _error_code = SAUNAFS_STATUS_OK)
	    : entry(_entry), state(_state), error_code(_error_code) {}

	inline uint64_t request_offset() const {
		return entry->offset;
	}

	inline uint64_t bytes_to_read_left() const {
		return entry->requested_size;
	}

	inline uint64_t endOffset() const {
		return request_offset() + bytes_to_read_left();
	}
};

using ReadaheadRequestPtr = std::shared_ptr<ReadaheadRequestEntry>;
using ConditionVariablePtr = std::shared_ptr<std::condition_variable>;

// The ```RequestConditionVariablePair``` struct contains the pointer to a
// request and its related conditional variable. The related conditional
// variable will notify waiting threads when the request is done.
struct RequestConditionVariablePair {
	ReadaheadRequestPtr requestPtr;
	ConditionVariablePtr cvPtr;

	RequestConditionVariablePair(ReadaheadRequestPtr _requestPtr)
	    : requestPtr(_requestPtr), cvPtr(new std::condition_variable()) {}

	RequestConditionVariablePair(ReadaheadRequestPtr _requestPtr,
	                             ConditionVariablePtr _cvPtr)
	    : requestPtr(_requestPtr), cvPtr(_cvPtr) {}

	void notify() {
		cvPtr->notify_all();
	}
};

// The ```ReadaheadRequests``` struct contains the data of the readahead
// requests already scheduled for a ReadRecord instance. It implements all
// operations regarding these requests.
struct ReadaheadRequests {

	/** \brief Append a request to the underlying container.
	 * Considering the constructor of ```RequestConditionVariablePair``` used it
	 * also creates the new conditional variable for this request.
	 *
	 * \param readaheadRequestPtr The pointer to the request to append.
	 * \return A copy to the appended instance to the underlying container.
	 */
	RequestConditionVariablePair
	append(ReadaheadRequestPtr readaheadRequestPtr);

	/** \brief Notify the waiting threads for ALL the first already finished
	 * requests. Remove those requests from the underlying container.
	 */
	void tryNotify();

	/** \brief Calculate the finishing offset of continuous interval of data
	 * (starting from the given ```offset```) that can be resolved if the
	 * scheduled readahead operations succeed.
	 *
	 * This finishing offset is capped by the ```endOffset``` parameter, with
	 * the meaning that if it is equal this threshold, then the system request
	 * requerying this value will be satisfied, and the request to wait for is
	 * already scheduled and is returned.
	 *
	 * \param offset The starting offset of the query.
	 * \param endOffset The cap of the finishing request of the query.
	 * \param result The ```Result``` instance to insert all the already
	 * scheduled requests that form the continuous interval.
	 * \param waitingCVPtr If the finishing offset is greater than or equal to
	 * the endOffset, the conditional variable of the last request is returned
	 * here.
	 * \param requestPtr If the finishing offset is greater than or equal
	 * to the endOffset, a pointer to the last request entry is returned here.
	 * \return The finishing offset of continuous interval of data (from the
	 * given ```offset```) that can be resolved if the scheduled readahead
	 * operations succeed.
	 */
	uint64_t continuousOffsetRequested(uint64_t offset, uint64_t endOffset,
	                                   ReadCache::Result &result,
	                                   ConditionVariablePtr &waitingCVPtr,
	                                   ReadaheadRequestPtr &requestPtr);

	inline bool empty() const {
		return pendingRequests_.empty();
	}

	inline size_t size() const {
		return pendingRequests_.size();
	}

	inline RequestConditionVariablePair lastPendingRequest() const {
		return pendingRequests_.back();
	}

	/** \brief Given a non-successfully finishing request, propagate the error
	 * to ALL the following requests, notifying threads waiting for these
	 * requests and remove those following requests.
	 *
	 * \param reqPtr The pointer to a non-successfully finishing request.
	 */
	void clearAndNotify(const ReadaheadRequestPtr &reqPtr);

	/** For debug purposes. */
	std::stringstream toString();

	/** \brief Discard all pending requests. Change ALL inqueued request status
	 * to discarded and clears the underlying container.
	 */
	void discardAllPendingRequests();

private:
	using RequestsContainer = std::list<RequestConditionVariablePair>;

	// List of pending requests.
	//
	// Per request, it contains a pair <requestPtr, cvPtr>, i.e a pointer to the
	// request entry and a pointer to a conditional variable to notify waiting
	// threads for the completion of the related request.
	RequestsContainer pendingRequests_;
};

// The ```ReadRecord``` struct contains the data related to the reading
// operations of a process on our file system. It is specially useful for
// returning data already requested from its inner cache and improving
// sequential reads with the readahead mechanism.
struct ReadRecord {
	ReadCache cache;
	ReadaheadAdviser readahead_adviser;
	ReadChunkLocator locator;
	uint32_t inode;
	ReadaheadRequests readaheadRequests;
	std::atomic<uint8_t> refreshCounter = 0;
	std::atomic<uint16_t> requestsInProcess = 0;
	bool expired = false;                        // gMutex

	ReadRecord(uint32_t inode)
	    : cache(gCacheExpirationTime_ms),
	      readahead_adviser(gCacheExpirationTime_ms, gReadaheadMaxWindowSize),
	      inode(inode) {}

	void resetSuggestedReadaheadReqs() {
		suggestedReadaheadReqs_ = 0;
	}

	void increaseSuggestedReadaheadReqs() {
		suggestedReadaheadReqs_ = std::min<uint32_t>(
		    gMaxReadaheadRequests, suggestedReadaheadReqs_ + 1);
	}

	inline uint32_t suggestedReadaheadReqs() const {
		return suggestedReadaheadReqs_;
	}

private:
	uint32_t suggestedReadaheadReqs_ = 0;
};

using Request = std::pair<ReadRecord *, ReadaheadRequestPtr>;

// The ```ReadaheadOperationsManager``` class is thought as a singleton class
// per mountpoint. It manages all the readahead operations, i.e when the read
// cache is enabled all read operations pass through that instance and wait for
// the read workers to process the requests.
class ReadaheadOperationsManager {
public:
	/**
	 * \brief Request some data from the given inode.
	 * 
	 * Using the provided data, this function:
	 * - searches in cache for the requested data, if present then returns it 
	 * directly.
	 * - if the request could be satisfied after the completion of some of the 
	 * already scheduled ```ReadaheadRequests``` of the given inode, then 
	 * returns the request to wait for.
	 * - if both of the previous conditions are not met, then schedule a specific
	 * request and return it.
	 * 
	 * \note It uses the provided ```Result``` instance to insert the entries that 
	 * would be finally used to satisfy the system request. After waiting for the 
	 * notification of the returned ```ConditionVariablePtr``` the ```Result``` 
	 * instance returned must contain the data to satisfy the system request.
	 * 
	 * \param rrec The pointer to the read data of the given inode. 
	 * \param fuseOffset Real starting offset of the system request.
	 * \param fuseSize Real size of the system request.
	 * \param offset Starting offset of the system request after aligning it to
	 * block size.
	 * \param size Size of the system request after aligning it to block size.
	 * \param result Container of the final entries that will satisfy the given 
	 * system request.
	 * \param waitingCVPtr Pointer to the ```conditional_variable``` to notify the
	 * related request is already finished. If not necessary to wait, this value
	 * is not set.
	 * \param requestPtr Pointer to the readahead request the system request is 
	 * waiting for. If not necessary to wait, this value is not set.
	 * \return Whether the required data is not already in cache and it is 
	 * necessary to wait for a scheduled request to finish.
	 */
	bool request(ReadRecord *rrec, off_t fuseOffset, size_t fuseSize,
	             uint64_t offset, uint32_t size, ReadCache::Result &result,
	             ConditionVariablePtr &waitingCVPtr,
	             ReadaheadRequestPtr &requestPtr);

	/** \brief Check if the underlying container of scheduled requests is empty. */
	inline bool empty() const {
		return readaheadRequestContainer_.empty();
	}

	/** \brief Return next scheduled request and remove it from the underlying
	 * container.
	 *
	 * \returns The next scheduled request.
	 */
	Request nextRequest();

	/** \brief Insert a request signaling one read worker thread to stop. */
	void putTerminateRequest();

private:
	/**
	 * \brief Add a readahead request to the underlying container and to the 
	 * provided ```ReadRecord```'s ```ReadaheadRequests``` member.
	 * 
	 * \param rrec The related ```ReadRecord```.
	 * \param entry Pointer to the ```Entry``` in the read cache of the given 
	 * ```ReadRecord```.
	 * \return ```RequestConditionVariablePair``` - A pair of pointers to the 
	 * request inserted and the ```std::conditional_variable``` to wait for.
	 */
	RequestConditionVariablePair addRequest_(ReadRecord *rrec,
	                                         ReadCache::Entry *entry);

	/**
	 * \brief Add extra requests to increase the cached data from a given offset.
	 * 
	 * This increase is bounded by:
	 * - the number readahead requests should not be higher than the suggested
	 * readahead requests for the ```ReadRecord```.
	 * - should not add more requests if the total readahead exceeds 
	 * ```gMaxReadaheadRequests```*```satisfying_size```.
	 * - should not add more requests if the total readahead exceeds the 
	 * throughput estimation done the ```ReadaheadAdviser```.
	 * 
	 * \param rrec The related ```ReadRecord```.
	 * \param currentOffset Offset of the last system request.
	 * \param satisfyingSize Size satisfying both system request size and
	 * ```ReadaheadAdviser``` suggestion.
	 * \param maximumRequestedOffset If the ```ReadRecord``` contains some
	 * scheduled requests, this value is ignored and only takes into account last 
	 * scheduled request. If not, this value provides the starting offset to 
	 * add the extra requests.
	 */
	void addExtraRequests_(ReadRecord *rrec, uint64_t currentOffset,
	                       uint64_t satisfyingSize,
	                       uint64_t maximumRequestedOffset);

	using ReadaheadRequestContainer = std::queue<Request>;

	ReadaheadRequestContainer readaheadRequestContainer_;
};

inline uint64_t round_up_to_blocksize(uint64_t bytes);
uint32_t read_data_get_wave_read_timeout_ms();
uint32_t read_data_get_connect_timeout_ms();
uint32_t read_data_get_total_read_timeout_ms();
bool read_data_get_prefetchxorstripes();

void read_inode_ops(uint32_t inode);
ReadRecord *read_data_new(uint32_t inode);
void read_data_end(ReadRecord *rr);
int read_to_buffer(ReadRecord *rrec, uint64_t current_offset,
                   uint64_t bytes_to_read, std::vector<uint8_t> &read_buffer,
                   uint64_t *bytes_read, ChunkReader &reader);
int read_data(ReadRecord *rr, off_t fuseOffset, size_t fuseSize,
              uint64_t offset, uint32_t size, ReadCache::Result &ret);
void read_data_init(uint32_t retries, uint32_t chunkserverRoundTripTime_ms,
                    uint32_t chunkserverConnectTimeout_ms,
                    uint32_t chunkServerWaveReadTimeout_ms,
                    uint32_t chunkserverTotalReadTimeout_ms,
                    uint32_t cache_expiration_time_ms,
                    uint32_t readahead_max_window_size_kB,
					uint32_t read_cache_max_size_mB,
                    uint32_t read_workers, uint32_t max_readahead_requests,
                    bool prefetchXorStripes, double bandwidth_overuse);
void read_data_term();
