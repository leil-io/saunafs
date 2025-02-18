/*

   Copyright 2016 Skytechnology sp. z o.o.
   Copyright 2023 Leil Storage OÜ

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

#include "common/small_vector.h"
#include "common/time_utils.h"

#include <atomic>
#include <cassert>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>

#include <boost/intrusive/list.hpp>
#include <boost/intrusive/set.hpp>

#define MISSING_OFFSET_PTR nullptr

inline std::atomic<uint64_t> gReadCacheMaxSize;
inline std::mutex gReadCacheMemoryMutex;
inline uint64_t gUsedReadCacheMemory;
inline std::atomic<bool> gReadCacheMemoryAlmostExceeded = false;
inline std::atomic<uint32_t> gCacheExpirationTime_ms;

constexpr double kReadCacheThreshold = 0.8;

inline void updateReadCacheMemoryAlmostExceeded() {
	gReadCacheMemoryAlmostExceeded =
	    gUsedReadCacheMemory >=
	    static_cast<uint64_t>(kReadCacheThreshold * gReadCacheMaxSize.load());
}

inline void increaseUsedReadCacheMemory(size_t bytesToReadLeft) {
	gUsedReadCacheMemory += bytesToReadLeft;
	updateReadCacheMemoryAlmostExceeded();
}

inline void decreaseUsedReadCacheMemory(size_t bytesToReadLeft) {
	gUsedReadCacheMemory -= bytesToReadLeft;
	updateReadCacheMemoryAlmostExceeded();
}

class ReadCache {
public:
	typedef uint64_t Offset;
	typedef uint32_t Size;

	struct Entry {
		Offset offset;
		std::vector<uint8_t> buffer;
		Timer timer;
		std::atomic<int> refcount = 0;
		std::atomic<Size> requested_size;
		std::atomic<bool> done = false;
		boost::intrusive::set_member_hook<> set_member_hook;
		boost::intrusive::list_member_hook<> lru_member_hook;
		boost::intrusive::list_member_hook<> reserved_member_hook;
		std::mutex mutex;

		struct OffsetComp {
			bool operator()(Offset offset, const Entry &entry) const {
				return offset < entry.offset;
			}
		};

		Entry(Offset offset, Size requested_size)
		    : offset(offset), buffer(), timer(), requested_size(requested_size),
		      set_member_hook(), lru_member_hook() {}

		~Entry() {
			mutex.lock();  // Make helgrind happy
			mutex.unlock();
			pthread_mutex_destroy(mutex.native_handle());
		}

		bool operator<(const Entry &other) const {
			return offset < other.offset;
		}

		bool expired(uint32_t expiration_time) {
			std::unique_lock entryLock(mutex);
			return timer.elapsed_ms() >= expiration_time;
		}

		void reset_timer() {
			timer.reset();
		}

		Offset endOffset() const {
			return offset + buffer.size();
		}

		void acquire() {
			std::unique_lock entryLock(mutex);
			timer.reset();
			refcount++;
		}

		void release() {
			assert(refcount > 0);
			refcount--;
		}
	};

	typedef boost::intrusive::set<Entry,
	        boost::intrusive::member_hook<Entry, boost::intrusive::set_member_hook<>,
	        &Entry::set_member_hook>> EntrySet;
	typedef boost::intrusive::list<Entry,
	        boost::intrusive::member_hook<Entry, boost::intrusive::list_member_hook<>,
	        &Entry::lru_member_hook>> EntryList;
	typedef boost::intrusive::list<Entry,
	        boost::intrusive::member_hook<Entry, boost::intrusive::list_member_hook<>,
	        &Entry::reserved_member_hook>> ReservedEntryList;

	struct Result {
		small_vector<Entry *, 8> entries;
		bool is_fake;

		Result() : entries(), is_fake(false) {}
		Result(Result &&other) noexcept
		       : entries(std::move(other.entries)), is_fake(other.is_fake) {}
		Result &operator=(Result &&other) noexcept {
			entries = std::move(other.entries);
			is_fake = other.is_fake;
			return *this;
		}

		// Wrapper for returning data not really residing in cache
		Result(std::vector<uint8_t> &&data) : entries(), is_fake(true) {
			Entry *entry = new Entry(0, 0);
			entry->buffer = std::move(data);
			entries.push_back(entry);
		}

		~Result() {
			if (is_fake) {
				assert(entries.size() == 1);
				delete entries.front();
			} else {
				release();
			}
		}

		Offset frontOffset() const {
			assert(!entries.empty());
			return entries.front()->offset;
		}

		Offset remainingOffset() const {
			assert(!entries.empty());
			return entries.back()->offset;
		}

		Offset endOffset() const {
			assert(!entries.empty());
			return entries.back()->offset +
			       (entries.back()->done
			            ? entries.back()->buffer.size()
			            : static_cast<size_t>(entries.back()->requested_size));
		}

		/*!
		 * \brief Give access to a buffer which should be filled with data.
		 *
		 * If cache result is incomplete (i.e. some data should be read to fulfill the request),
		 * it should be read into this buffer in order to write it straight to cache.
		 */
		std::vector<uint8_t> &inputBuffer() {
			assert(!entries.empty());
			assert(entries.back()->buffer.empty());
			assert(entries.back()->refcount > 0);
			return entries.back()->buffer;
		}

		/*!
		 * \brief Serialize cache query result to an iovector.
		 *
		 * An iovector can be any structure that accepts pushing back
		 * a pair of {address, length}, which represents a consecutive array
		 * of bytes extracted from cache.
		 *
		 * \return number of bytes added to iovector
		 */
		template<typename IoVec>
		Size toIoVec(IoVec &output, Offset real_offset, Size real_size) const {
			assert(real_offset >= frontOffset());
			uint64_t offset = real_offset;
			Size bytes_left = real_size;
			for (const auto &entry_ptr : entries) {
				const ReadCache::Entry &entry = *entry_ptr;
				if (bytes_left <= 0) {
					break;
				}
				// Special case: Read request was past the end of the file
				if (entry.buffer.empty() || offset >= entry.endOffset()) {
					break;
				}
				assert(offset >= entry.offset && offset < entry.endOffset());
				auto start = entry.buffer.data() + (offset - entry.offset);
				auto end = std::min(start + bytes_left, entry.buffer.data() + entry.buffer.size());
				assert(start < end);
				size_t length = std::distance(start, end);

				output.push_back({(void *)start, length});
				offset += length;
				bytes_left -= length;
			}
			return offset - real_offset;
		}

		Size copyToBuffer(uint8_t *output, Offset real_offset, Size real_size) const {
			assert(real_offset >= frontOffset());
			uint64_t offset = real_offset;
			Size bytes_left = real_size;
			for (const auto &entry_ptr : entries) {
				const ReadCache::Entry &entry = *entry_ptr;
				if (bytes_left <= 0) {
					break;
				}
				// Special case: Read request was past the end of the file
				if (entry.buffer.empty() || offset >= entry.endOffset()) {
					break;
				}
				assert(offset >= entry.offset && offset < entry.endOffset());
				auto start = entry.buffer.data() + (offset - entry.offset);
				auto end = std::min(start + bytes_left, entry.buffer.data() + entry.buffer.size());
				assert(start < end);
				size_t length = std::distance(start, end);
				std::memcpy(output, (void *)start, length);
				output += length;
				offset += length;
				bytes_left -= length;
			}
			return offset - real_offset;
		}

		bool empty() const {
			return entries.empty();
		}

		void release() {
			for (auto &entry : entries) {
				std::unique_lock entryLock(
				    entry->mutex);  // Make helgrind happy
				entry->release();
			}
			entries.clear();
		}

		void add(Entry &entry) {
			entry.acquire();
			assert(entries.empty() || endOffset() >= entry.offset);
			entries.push_back(std::addressof(entry));
		}

		Size requestSize(Offset real_offset, Size real_size) const {
			if (entries.empty()) {
				return 0;
			}
			assert(real_offset >= frontOffset());
			assert(real_offset <= endOffset());
			return std::min<Size>(endOffset() - real_offset, real_size);
		}

		Entry* back() {
			return entries.back();
		}

		std::string toString() const {
			std::string text;
			for(const auto &entry : entries) {
				text += "(" + std::to_string(entry->refcount.load()) + "|"
				+ std::to_string(entry->offset) + ":"
				+ std::to_string(entry->buffer.size()) + "),";
			}
			return text;
		}
	};

	explicit ReadCache(uint32_t expiration_time)
	: entries_(), lru_(), reserved_entries_(), expiration_time_(expiration_time) {}

	~ReadCache() {
		clear();
		clearReserved(std::numeric_limits<unsigned>::max());
		assert(entries_.empty());
		assert(lru_.empty());
		assert(reserved_entries_.empty());
	}

	void collectGarbage(unsigned count = 1000000) {
		unsigned reserved_count = count;
		expiration_time_ = gCacheExpirationTime_ms.load();

		while (!lru_.empty() && count-- > 0) {
			Entry *e = std::addressof(lru_.front());
			if (e->expired(expiration_time_) && e->done) {
				erase(entries_.iterator_to(*e));
			} else {
				break;
			}
		}

		clearReserved(reserved_count);
	}

	/*!
	 * \brief Try to get data from cache.
	 *
	 * If all data is available in cache, it can be obtained from result
	 * as an iovector via result.toIoVec() call.
	 * If some or no data is available, the rest should be read into the result buffer
	 * via result.inputBuffer(). Then, it can be obtain as an iovector via result.toIoVec().
	 *
	 * \return cache query result
	 */
	Entry *query(Offset offset, Size size, ReadCache::Result &result,
	             bool insertPending = true) {
		collectGarbage();

		auto it = entries_.upper_bound(offset, Entry::OffsetComp());
		if (it != entries_.begin()) {
			--it;
		}

		assert(size > 0);

		Size bytes_left = size;
		while (it != entries_.end() && bytes_left > 0) {
			if (offset < it->offset) {
				break;
			}

			if (!it->done) {
				++it;
				continue;
			}

			if (it->expired(expiration_time_) || it->buffer.empty()) {
				it = erase(it);
				continue;
			}

			if (offset < it->endOffset()) {
				Size bytes_from_buffer = std::min<Size>(it->buffer.size() - (offset - it->offset), bytes_left);

				bytes_left -= bytes_from_buffer;
				offset += bytes_from_buffer;
				result.add(*it);
			}
			++it;
		}

		if (bytes_left > 0 && insertPending) {
			it = entries_.upper_bound(offset, Entry::OffsetComp());
			if (it != entries_.begin()) {
				--it;
			}

			if (it != entries_.end() && it->offset == offset) {
				assert(!it->done);
				it = erase(it);
			}
			auto inserted = insert(it, offset, bytes_left);
			result.add(*inserted);
			return &(*inserted);
		}
		return nullptr;
	}

	Entry *forceInsert(Offset offset, Size size) {
		collectGarbage();

		auto it = entries_.upper_bound(offset, Entry::OffsetComp());
		if (it != entries_.begin()) {
			--it;
		}

		auto inserted = insert(it, offset, size);
		return std::addressof(*inserted);
	}

	void clear() {
		auto it = entries_.begin();
		while (it != entries_.end()) {
			it = erase(it);
		}
	}

	Entry *find(uint64_t offset) {
		auto it = entries_.upper_bound(offset, Entry::OffsetComp());
		if (it != entries_.begin()) {
			--it;
		}

		if (it != entries_.end() && it->offset == offset) {
			if (it->requested_size == 0) {
				it = erase(it);
				return MISSING_OFFSET_PTR;
			}
			return std::addressof(*it);
		}
		return MISSING_OFFSET_PTR;
	}

protected:
	EntrySet::iterator insert(EntrySet::iterator it, Offset offset, Size size) {
		it = clearCollisions(it, offset + size);
		Entry *e = new Entry(offset, size);
		lru_.push_back(*e);
		assert(entries_.find(*e) == entries_.end());
		return entries_.insert(it, *e);
	}

	EntrySet::iterator erase(EntrySet::iterator it) {
		assert(it != entries_.end());
		Entry *e = std::addressof(*it);
		auto ret = entries_.erase(it);
		lru_.erase(lru_.iterator_to(*e));
		if (e->refcount > 0) {
			reserved_entries_.push_back(*e);
		} else {
			assert(e->refcount == 0);
			std::unique_lock usedMemoryLock(gReadCacheMemoryMutex);
			decreaseUsedReadCacheMemory(e->buffer.size());
			usedMemoryLock.unlock();
			delete e;
		}
		return ret;
	}

	void clearReserved(unsigned count) {
		std::unique_lock<std::mutex> usedMemoryLock(gReadCacheMemoryMutex,
		                                            std::defer_lock);
		while (!reserved_entries_.empty() && count-- > 0) {
			Entry *e = std::addressof(reserved_entries_.front());
			if (e->refcount == 0) {
				usedMemoryLock.lock();
				decreaseUsedReadCacheMemory(e->buffer.size());
				usedMemoryLock.unlock();
				reserved_entries_.pop_front();
				delete e;
			} else {
				assert(e->refcount >= 0);
				reserved_entries_.splice(reserved_entries_.end(),
				                         reserved_entries_,
				                         reserved_entries_.begin());
			}
		}
	}

	EntrySet::iterator clearCollisions(EntrySet::iterator it, Offset start_offset) {
		while (it != entries_.end() && it->offset < start_offset) {
			if (it->done && it->offset + it->requested_size > start_offset) {
				it = erase(it);
			} else {
				it++;
			}
		}
		return it;
	}

	std::string toString() const {
		std::string text;
		for(const auto &entry : entries_) {
			text += "(" + std::to_string(entry.refcount.load()) + "|"
			+ std::to_string(entry.offset) + ":"
			+ std::to_string(entry.buffer.size()) + "),";
		}
		return text;
	}

	EntrySet entries_;
	EntryList lru_;
	ReservedEntryList reserved_entries_;
	uint32_t expiration_time_;
};
