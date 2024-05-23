/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA
   Copyright 2013-2014 EditShare
   Copyright 2013-2017 Skytechnology sp. z o.o.
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

#include "common/platform.h"
#include "mount/readdata.h"

#include <cerrno>
#include <cinttypes>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <limits>
#include <map>
#include <mutex>
#include <thread>

#include "common/connection_pool.h"
#include "common/datapack.h"
#include "common/exceptions.h"
#include "common/sfserr/sfserr.h"
#include "common/read_plan_executor.h"
#include "common/slogger.h"
#include "common/sockets.h"
#include "common/time_utils.h"
#include "mount/chunk_locator.h"
#include "mount/chunk_reader.h"
#include "mount/mastercomm.h"
#include "mount/readahead_adviser.h"
#include "mount/readdata_cache.h"
#include "mount/tweaks.h"
#include "protocol/SFSCommunication.h"

#define USECTICK 333333
#define REFRESHTICKS 15

#define EMPTY_REQUEST nullptr

inline std::condition_variable readOperationsAvailable;

RequestConditionVariablePair
ReadaheadRequests::append(ReadaheadRequestPtr readaheadRequestPtr) {
	return pendingRequests_.emplace_back(readaheadRequestPtr);
}

void ReadaheadRequests::tryNotify() {
	while (!pendingRequests_.empty() &&
	       pendingRequests_.front().requestPtr->state ==
	           ReadaheadRequestState::kFinished) {
		pendingRequests_.front().notify();
		pendingRequests_.pop_front();
	}
}

void ReadaheadRequests::clearAndNotify(const ReadaheadRequestPtr &reqPtr) {
	// find the request
	auto it = pendingRequests_.begin();
	uint32_t index = 0;
	while (it != pendingRequests_.end() && it->requestPtr != reqPtr) {
		it++;
		index++;
	}

	// this request already is not there
	if (it == pendingRequests_.end()) {
		return;
	}

	// remove all the requests that were considering this request successful
	while (pendingRequests_.size() > index) {
		pendingRequests_.back().requestPtr->error_code = reqPtr->error_code;
		pendingRequests_.back().notify();
		pendingRequests_.pop_back();
	}
}

uint64_t ReadaheadRequests::continuousOffsetRequested(
    uint64_t offset, uint64_t endOffset, ReadCache::Result &result,
    ConditionVariablePtr &waitingCVPtr, ReadaheadRequestPtr &requestPtr) {
	if (pendingRequests_.empty()) {
		return offset;
	}

	auto it = pendingRequests_.begin();
	while (it != pendingRequests_.end() && offset < endOffset) {
		if (pendingRequests_.front().requestPtr->request_offset() <= offset &&
		    pendingRequests_.front().requestPtr->endOffset() > offset) {
			result.add(*(it->requestPtr->entry));
			offset = it->requestPtr->endOffset();
		}
		it++;
	}
	if (offset >= endOffset) {
		// the process finalized in the next one after achieving the desired
		// offset
		it--;
		offset = endOffset;
		waitingCVPtr = it->cvPtr;
		requestPtr = it->requestPtr;
	}
	return offset;
}

std::stringstream ReadaheadRequests::toString() {
	std::stringstream text;
	int index = 0;
	for (const auto &req : pendingRequests_) {
		text << (std::to_string(index) + ": (" +
		         std::to_string(req.requestPtr->request_offset()) + "," +
		         std::to_string(req.requestPtr->bytes_to_read_left()) + "," +
		         std::to_string(req.requestPtr->endOffset()) + ")" + "\n");
		index++;
	}
	return text;
}

void ReadaheadRequests::discardAllPendingRequests() {
	for (const auto &req : pendingRequests_) {
		if (req.requestPtr->state == ReadaheadRequestState::kInqueued &&
		    req.requestPtr->entry->refcount <= 1) {
			req.requestPtr->state = ReadaheadRequestState::kDiscarded;
		}
	}
	pendingRequests_.clear();
}

bool ReadaheadOperationsManager::request(ReadRecord *rrec, off_t fuseOffset,
                                         size_t fuseSize, uint64_t offset,
                                         uint32_t size,
                                         ReadCache::Result &result,
                                         ConditionVariablePtr &waitingCVPtr,
                                         ReadaheadRequestPtr &requestPtr) {
	bool isSequential = true;
	// Feed the adviser with original FUSE offset and size (before alignment)
	rrec->readahead_adviser.feed(fuseOffset, fuseSize, isSequential);

	rrec->cache.query(offset, size, result, false);

	uint64_t recommendedSize = round_up_to_blocksize(
	    std::max<uint64_t>(size, rrec->readahead_adviser.window()));

	if (!result.empty() && result.frontOffset() <= offset &&
	    offset + size <= result.endOffset()) {
		// this query can be directly served from cache
		addExtraRequests_(rrec, offset, recommendedSize, result.endOffset());
		return false;
	}

	if (!isSequential) {
		rrec->resetSuggestedReadaheadReqs();
	} else {
		rrec->increaseSuggestedReadaheadReqs();
	}

	uint64_t cachedOffset = result.empty() ? offset : result.endOffset();

	// check if this data is already in process
	uint64_t maximumRequestedOffset =
	    rrec->readaheadRequests.continuousOffsetRequested(
	        cachedOffset, offset + size, result, waitingCVPtr, requestPtr);

	if (maximumRequestedOffset == offset + size) {
		// this query can be directly served after succeeding at some
		// pending read requests
		addExtraRequests_(rrec, offset, recommendedSize,
		                  maximumRequestedOffset);
		return true;
	}

	if (!isSequential && maximumRequestedOffset == cachedOffset) {
		rrec->readaheadRequests.discardAllPendingRequests();
	}

	// this query cannot be directly served after succeeding at some pending
	// read requests, it is necessary to add specific request for it
	auto remainingSize = recommendedSize - (maximumRequestedOffset - offset);

	rrec->cache.query(maximumRequestedOffset, remainingSize, result, true);

	uint64_t requestOffset = result.remainingOffset();
	uint64_t bytesToReadLeft = round_up_to_blocksize(
	    remainingSize - (requestOffset - maximumRequestedOffset));

	auto requestConditionVariablePair = addRequest_(rrec, result.back());

	waitingCVPtr = requestConditionVariablePair.cvPtr;
	requestPtr = requestConditionVariablePair.requestPtr;

	addExtraRequests_(rrec, offset, recommendedSize,
	                  requestOffset + bytesToReadLeft);

	return true;
}

Request ReadaheadOperationsManager::nextRequest() {
	Request request = readaheadRequestContainer_.front();
	readaheadRequestContainer_.pop();
	return request;
}

void ReadaheadOperationsManager::putTerminateRequest() {
	readaheadRequestContainer_.push(
	    std::make_pair(EMPTY_REQUEST, EMPTY_REQUEST));
	readOperationsAvailable.notify_one();
}

RequestConditionVariablePair
ReadaheadOperationsManager::addRequest_(ReadRecord *rrec,
                                        ReadCache::Entry *entry) {
	ReadaheadRequestPtr readaheadRequestPtr =
	    ReadaheadRequestPtr(new ReadaheadRequestEntry(entry));
	readaheadRequestContainer_.push(std::make_pair(rrec, readaheadRequestPtr));
	entry->acquire();

	readOperationsAvailable.notify_one();

	return rrec->readaheadRequests.append(readaheadRequestPtr);
}

void ReadaheadOperationsManager::addExtraRequests_(
    ReadRecord *rrec, uint64_t currentOffset, uint64_t satisfyingSize,
    uint64_t maximumRequestedOffset) {
	if (!rrec->readaheadRequests.empty()) {
		maximumRequestedOffset = rrec->readaheadRequests.lastPendingRequest()
		                             .requestPtr->endOffset();
	}

	uint64_t throughputWindow = rrec->readahead_adviser.throughputWindow();
	uint64_t readaheadSize = std::min<uint64_t>(
	    gMaxReadaheadRequests * satisfyingSize, throughputWindow);

	while (rrec->readaheadRequests.size() < rrec->suggestedReadaheadReqs() &&
	       maximumRequestedOffset < currentOffset + readaheadSize) {

		auto it = rrec->cache.find(maximumRequestedOffset);
		if (it != MISSING_OFFSET_PTR) {
			maximumRequestedOffset += it->requested_size;
			continue;
		}

		ReadCache::Entry *entry =
		    rrec->cache.forceInsert(maximumRequestedOffset, satisfyingSize);

		// we are not going to use the return value from the addRequest_ call
		RequestConditionVariablePair _ = addRequest_(rrec, entry);

		maximumRequestedOffset += satisfyingSize;
	}
}

using ReadRecords = std::unordered_multimap<uint32_t, ReadRecord *>;
using ReadRecordRange = std::pair<ReadRecords::iterator, ReadRecords::iterator>;

inline ConnectionPool gReadConnectionPool;
inline ChunkConnectorUsingPool gChunkConnector(gReadConnectionPool);
inline ReadaheadOperationsManager gReadaheadOperationsManager;
inline std::mutex gMutex;
inline std::mutex gReadaheadOperationsManagerMutex;
inline ReadRecords gActiveReadRecords;
inline pthread_t delayedOpsThread;
inline std::vector<pthread_t> readOpsThreads;
inline std::atomic<uint32_t> gChunkserverConnectTimeout_ms;
inline std::atomic<uint32_t> gChunkserverWaveReadTimeout_ms;
inline std::atomic<uint32_t> gChunkserverTotalReadTimeout_ms;
inline uint32_t gReadWorkers;
inline std::atomic<bool> gPrefetchXorStripes;
inline bool readDataTerminate;
inline std::atomic<uint32_t> maxRetries;
inline double gBandwidthOveruse;

const unsigned ReadaheadAdviser::kInitWindowSize;
const int ReadaheadAdviser::kRandomThreshold;
const int ReadaheadAdviser::kHistoryEntryLifespan_ns;
const int ReadaheadAdviser::kHistoryCapacity;
const unsigned ReadaheadAdviser::kHistoryValidityThreshold;

inline uint64_t round_up_to_blocksize(uint64_t bytes) {
	return (bytes + SFSBLOCKSIZE - 1) / SFSBLOCKSIZE * SFSBLOCKSIZE;
}

uint32_t read_data_get_wave_read_timeout_ms() {
	return gChunkserverWaveReadTimeout_ms;
}

uint32_t read_data_get_connect_timeout_ms() {
	return gChunkserverConnectTimeout_ms;
}

uint32_t read_data_get_total_read_timeout_ms() {
	return gChunkserverTotalReadTimeout_ms;
}

bool read_data_get_prefetchxorstripes() {
	return gPrefetchXorStripes;
}

inline void clear_active_read_records()
{
	std::unique_lock lock(gMutex);

	for (ReadRecords::value_type& readRecord : gActiveReadRecords) {
		delete readRecord.second;
	}

	gActiveReadRecords.clear();
}

void* read_data_delayed_ops(void *arg) {
	(void)arg;

	pthread_setname_np(pthread_self(), "readDelayedOps");

	for (;;) {
		gReadConnectionPool.cleanup();
		std::unique_lock lock(gMutex);
		if (readDataTerminate) {
			return EMPTY_REQUEST;
		}
		auto readRecordIt = gActiveReadRecords.begin();
		while (readRecordIt != gActiveReadRecords.end()) {
			if (readRecordIt->second->refreshCounter < REFRESHTICKS) {
				++(readRecordIt->second->refreshCounter);
			}

			if (readRecordIt->second->expired &&
			    readRecordIt->second->requestsInProcess == 0 &&
			    readRecordIt->second->readaheadRequests.empty()) {
				delete readRecordIt->second;
				readRecordIt = gActiveReadRecords.erase(readRecordIt);
			} else {
				++readRecordIt;
			}
		}
		lock.unlock();
		usleep(USECTICK);
	}
}

void* read_worker(void *arg) {
	(void)arg;

	static std::atomic_uint16_t readWorkersCounter(0);
	std::string threadName = "readWorker " + std::to_string(readWorkersCounter++);
	pthread_setname_np(pthread_self(), threadName.c_str());

	for (;;) {
		std::unique_lock operationsLock(gReadaheadOperationsManagerMutex);
		if (gReadaheadOperationsManager.empty()) {
			readOperationsAvailable.wait(operationsLock, [] {
				return !gReadaheadOperationsManager.empty();
			});
		}

		auto [readRecord, request] = gReadaheadOperationsManager.nextRequest();

		if (!request) {
			return EMPTY_REQUEST;
		}

		ReadCache::Entry *entry = request->entry;

		// request no longer valid or no longer needed
		if (request->error_code != SAUNAFS_STATUS_OK
		    || request->state == ReadaheadRequestState::kDiscarded) {
			entry->release();
			entry->done = true;
			entry->requested_size = 0;
			continue;
		}

		ChunkReader reader(gChunkConnector, readRecord->locator,
		                   gBandwidthOveruse);
		request->state = ReadaheadRequestState::kProcessing;
		readRecord->requestsInProcess++;
		operationsLock.unlock();

		uint64_t bytes_read = 0;
		int error_code = read_to_buffer(readRecord, request->request_offset(),
		                                request->bytes_to_read_left(),
		                                entry->buffer, &bytes_read, reader);

		operationsLock.lock();
		entry->release();
		entry->done = true;
		entry->reset_timer();

		if (error_code != SAUNAFS_STATUS_OK
		    || request->error_code != SAUNAFS_STATUS_OK) {
			// discard any leftover bytes from incorrect read
			entry->buffer.clear();
			entry->requested_size = 0;

			// clear the list of read requests for this inode and notify waiting
			// threads of this error
			if (request->error_code == SAUNAFS_STATUS_OK) {
				request->error_code = error_code;
				readRecord->readaheadRequests.clearAndNotify(request);
			}

			readRecord->requestsInProcess--;
			continue;
		}

		entry->requested_size = entry->buffer.size();
		request->state = ReadaheadRequestState::kFinished;
		readRecord->readaheadRequests.tryNotify();
		readRecord->requestsInProcess--;
	}

	return EMPTY_REQUEST;
}

ReadRecord *read_data_new(uint32_t inode) {
	ReadRecord *rrec = new ReadRecord(inode);
	std::unique_lock lock(gMutex);

	gActiveReadRecords.emplace(inode, rrec);

	return rrec;
}

void read_data_end(ReadRecord *rrec) {
	std::unique_lock lock(gMutex);
	rrec->expired = true;
	lock.unlock();
	std::unique_lock managerLock(gReadaheadOperationsManagerMutex);
	rrec->readaheadRequests.discardAllPendingRequests();
}

void read_data_init(uint32_t retries,
		uint32_t chunkserverRoundTripTime_ms,
		uint32_t chunkserverConnectTimeout_ms,
		uint32_t chunkServerWaveReadTimeout_ms,
		uint32_t chunkserverTotalReadTimeout_ms,
		uint32_t cache_expiration_time_ms,
		uint32_t readahead_max_window_size_kB,
		uint32_t read_workers,
		uint32_t max_readahead_requests,
		bool prefetchXorStripes,
		double bandwidth_overuse) {
	pthread_attr_t thattr;

	readDataTerminate = false;

	clear_active_read_records();

	maxRetries = retries;
	gChunkserverConnectTimeout_ms = chunkserverConnectTimeout_ms;
	gChunkserverWaveReadTimeout_ms = chunkServerWaveReadTimeout_ms;
	gChunkserverTotalReadTimeout_ms = chunkserverTotalReadTimeout_ms;
	gCacheExpirationTime_ms = cache_expiration_time_ms;
	gReadaheadMaxWindowSize = readahead_max_window_size_kB * 1024;
	gReadWorkers = read_workers;
	gMaxReadaheadRequests = max_readahead_requests;
	gPrefetchXorStripes = prefetchXorStripes;
	gBandwidthOveruse = bandwidth_overuse;
	gTweaks.registerVariable("PrefetchXorStripes", gPrefetchXorStripes);
	gChunkConnector.setRoundTripTime(chunkserverRoundTripTime_ms);
	gChunkConnector.setSourceIp(fs_getsrcip());
	pthread_attr_init(&thattr);
	pthread_attr_setstacksize(&thattr,0x100000);
	pthread_create(&delayedOpsThread,&thattr,read_data_delayed_ops,NULL);
	readOpsThreads.resize(gReadWorkers);
	for (auto &th : readOpsThreads)
		pthread_create(&th, &thattr, read_worker, NULL);
	pthread_attr_destroy(&thattr);

	gTweaks.registerVariable("ReadMaxRetries", maxRetries);
	gTweaks.registerVariable("ReadConnectTimeout", gChunkserverConnectTimeout_ms);
	gTweaks.registerVariable("ReadWaveTimeout", gChunkserverWaveReadTimeout_ms);
	gTweaks.registerVariable("ReadTotalTimeout", gChunkserverTotalReadTimeout_ms);
	gTweaks.registerVariable("CacheExpirationTime", gCacheExpirationTime_ms);
	gTweaks.registerVariable("ReadaheadMaxWindowSize", gReadaheadMaxWindowSize);
	gTweaks.registerVariable("MaxReadaheadRequests", gMaxReadaheadRequests);
	gTweaks.registerVariable("ReadChunkPrepare", ChunkReader::preparations);
	gTweaks.registerVariable("ReqExecutedTotal", ReadPlanExecutor::executions_total_);
	gTweaks.registerVariable("ReqExecutedUsingAll", ReadPlanExecutor::executions_with_additional_operations_);
	gTweaks.registerVariable("ReqFinishedUsingAll", ReadPlanExecutor::executions_finished_by_additional_operations_);
}

void read_data_term(void) {
	{
		std::unique_lock lock(gMutex);
		readDataTerminate = true;
	}

	pthread_join(delayedOpsThread, NULL);
	for (uint32_t i = 0; i < gReadWorkers; i++) {
		gReadaheadOperationsManager.putTerminateRequest();
	}

	for (auto &thread : readOpsThreads) {
		pthread_join(thread, NULL);
	}

	clear_active_read_records();
}

void read_inode_ops(uint32_t inode) { // attributes of inode have been changed - force reconnect and clear cache
	std::unique_lock lock(gMutex);

	ReadRecordRange range = gActiveReadRecords.equal_range(inode);

	for (auto it = range.first; it != range.second; ++it) {
		it->second->refreshCounter = REFRESHTICKS; // force reconnect on forthcoming access
	}
}

int read_data_sleep_time_ms(int tryCounter) {
	if (tryCounter <= 13) {            // 2^13 = 8192
		return (1 << tryCounter);  // 2^tryCounter milliseconds
	} else {
		return 1000 * 10;          // 10 seconds
	}
}

static void print_error_msg(ChunkReader& reader, uint32_t try_counter, const Exception &ex) {
	if (reader.isChunkLocated()) {
		safs_pretty_syslog(LOG_WARNING,
		                   "read file error, inode: %u, index: %u, chunk: %" PRIu64 ", version: %u - %s "
		                   "(try counter: %u)", reader.inode(), reader.index(),
		                   reader.chunkId(), reader.version(), ex.what(), try_counter);
	} else {
		safs_pretty_syslog(LOG_WARNING,
		                   "read file error, inode: %u, index: %u, chunk: failed to locate - %s "
		                   "(try counter: %u)", reader.inode(), reader.index(),
		                   ex.what(), try_counter);
	}
}

int read_to_buffer(ReadRecord *rrec, uint64_t current_offset,
                   uint64_t bytes_to_read, std::vector<uint8_t> &read_buffer,
                   uint64_t *bytes_read, ChunkReader &reader) {
	uint32_t try_counter = 0;
	uint32_t prepared_inode = 0; // this is always different than any real inode
	uint32_t prepared_chunk_id = 0;
	assert(*bytes_read == 0);

	// forced sleep between retries caused by recoverable failures
	uint32_t sleep_time_ms = 0;

	bool force_prepare = (rrec->refreshCounter == REFRESHTICKS);

	while (bytes_to_read > 0) {
		Timeout sleep_timeout = Timeout(std::chrono::milliseconds(sleep_time_ms));
		// Increase communicationTimeout to sleepTime; longer poll() can't be worse
		// than short poll() followed by nonproductive usleep().
		uint32_t timeout_ms = std::max(gChunkserverTotalReadTimeout_ms.load(), sleep_time_ms);
		Timeout communication_timeout = Timeout(std::chrono::milliseconds(timeout_ms));
		sleep_time_ms = 0;
		try {
			uint32_t chunk_id = current_offset / SFSCHUNKSIZE;
			if (force_prepare || prepared_inode != rrec->inode || prepared_chunk_id != chunk_id) {
				reader.prepareReadingChunk(rrec->inode, chunk_id, force_prepare);
				prepared_chunk_id = chunk_id;
				prepared_inode = rrec->inode;
				force_prepare = false;
				rrec->refreshCounter = 0;
			}

			uint64_t offset_of_chunk = static_cast<uint64_t>(chunk_id) * SFSCHUNKSIZE;
			uint32_t offset_in_chunk = current_offset - offset_of_chunk;
			uint32_t size_in_chunk = SFSCHUNKSIZE - offset_in_chunk;
			if (size_in_chunk > bytes_to_read) {
				size_in_chunk = bytes_to_read;
			}
			uint32_t bytes_read_from_chunk = reader.readData(
					read_buffer, offset_in_chunk, size_in_chunk,
					gChunkserverConnectTimeout_ms, gChunkserverWaveReadTimeout_ms,
					communication_timeout, gPrefetchXorStripes);
			// No exceptions thrown. We can increase the counters and go to the next chunk
			*bytes_read += bytes_read_from_chunk;
			current_offset += bytes_read_from_chunk;
			bytes_to_read -= bytes_read_from_chunk;
			if (bytes_read_from_chunk < size_in_chunk) {
				// end of file
				break;
			}
			try_counter = 0;
		} catch (UnrecoverableReadException &ex) {
			print_error_msg(reader, try_counter, ex);
			if (ex.status() == SAUNAFS_ERROR_ENOENT) {
				return SAUNAFS_ERROR_EBADF; // stale handle
			} else {
				return SAUNAFS_ERROR_IO;
			}
		} catch (Exception &ex) {
			if (try_counter > 0) {
				print_error_msg(reader, try_counter, ex);
			}
			force_prepare = true;
			if (try_counter > maxRetries) {
				return SAUNAFS_ERROR_IO;
			} else {
				usleep(sleep_timeout.remaining_us());
				sleep_time_ms = read_data_sleep_time_ms(try_counter);
			}
			try_counter++;
		}
	}
	return SAUNAFS_STATUS_OK;
}

int read_data(ReadRecord *rrec, off_t fuseOffset, size_t fuseSize,
              uint64_t offset, uint32_t size, ReadCache::Result &ret) {
	assert(size % SFSBLOCKSIZE == 0);
	assert(offset % SFSBLOCKSIZE == 0);

	if (size == 0) {
		return SAUNAFS_STATUS_OK;
	}

	ReadCache::Result result;

	if (gCacheExpirationTime_ms == 0
	    || !rrec->readahead_adviser.shouldUseReadahead()) {
		// no cache case
		rrec->readahead_adviser.feed(fuseOffset, fuseSize);

		rrec->cache.query(offset, size, result, true);

		uint64_t endOffset = result.back()->done ? result.endOffset()
		                                         : result.remainingOffset();

		if (result.frontOffset() > offset || offset + size > endOffset) {
			uint64_t requestOffset = result.remainingOffset();
			uint64_t bytesToReadLeft = round_up_to_blocksize(
			    size - (requestOffset - offset));

			ChunkReader reader(gChunkConnector, rrec->locator,
			                   gBandwidthOveruse);
			uint64_t bytesRead = 0;
			int errorCode = read_to_buffer(rrec,
			                               requestOffset,
			                               bytesToReadLeft,
			                               result.inputBuffer(),
			                               &bytesRead,
			                               reader);
			result.back()->done = true;

			if (errorCode != SAUNAFS_STATUS_OK) {
				result.inputBuffer().clear();
				return errorCode;
			}
		}
	} else {
		// use the read operations manager to process the request
		ConditionVariablePtr waitingCVPtr;
		ReadaheadRequestPtr requestPtr;

		std::unique_lock lock(gReadaheadOperationsManagerMutex);
		bool mustWait = gReadaheadOperationsManager.request(
		    rrec, fuseOffset, fuseSize, offset, size, result, waitingCVPtr,
		    requestPtr);

		if (mustWait) {
			waitingCVPtr->wait(lock);

			int error_code = requestPtr->error_code;
			if (error_code != SAUNAFS_STATUS_OK) {
				return error_code;
			}
		}
		lock.unlock();
	}

	ret = std::move(result);
	return SAUNAFS_STATUS_OK;
}
