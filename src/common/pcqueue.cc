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

#include "common/platform.h"
#include "pcqueue.h"

#include <cerrno>
#include <cstdlib>
#include "devtools/TracePrinter.h"
#include <pthread.h>

ProducerConsumerQueue::ProducerConsumerQueue(uint32_t maxSize, Deleter deleter)
    : maxSize_(maxSize), currentSize_(0), deleter_(deleter) {
	TRACETHIS();
}

ProducerConsumerQueue::~ProducerConsumerQueue() {
	TRACETHIS();
	std::lock_guard<std::mutex> lock(mutex_);
	while (!queue_.empty()) {
		auto &entry = queue_.front();
		deleter_(entry.data);
		queue_.pop();
	}
}

bool ProducerConsumerQueue::isEmpty() const {
	TRACETHIS();
	std::lock_guard<std::mutex> lock(mutex_);
	return queue_.empty();
}

bool ProducerConsumerQueue::isFull() const {
	TRACETHIS();
	std::lock_guard<std::mutex> lock(mutex_);
	return maxSize_ > 0 && currentSize_ >= maxSize_;
}

uint32_t ProducerConsumerQueue::sizeLeft() const {
	TRACETHIS();
	std::lock_guard<std::mutex> lock(mutex_);
	return maxSize_ > 0 ? maxSize_ - currentSize_ : UINT32_MAX;
}

uint32_t ProducerConsumerQueue::elements() const {
	TRACETHIS();
	std::lock_guard<std::mutex> lock(mutex_);
	return queue_.size();
}

bool ProducerConsumerQueue::put(uint32_t jobId, uint32_t jobType, uint8_t *data,
                                uint32_t length) {
	TRACETHIS();
	std::unique_lock<std::mutex> lock(mutex_);
	notFull_.wait(lock, [this, length] {
		return maxSize_ == 0 || currentSize_ + length <= maxSize_;
	});

	if (maxSize_ > 0 && length > maxSize_) {
		errno = EDEADLK;
		return false;
	}

	queue_.emplace(jobId, jobType, data, length);
	currentSize_ += length;

	notEmpty_.notify_one();
	return true;
}

bool ProducerConsumerQueue::tryPut(uint32_t jobId, uint32_t jobType,
                                   uint8_t *data, uint32_t length) {
	TRACETHIS();
	std::lock_guard<std::mutex> lock(mutex_);
	if (maxSize_ > 0) {
		if (length > maxSize_) {
			errno = EDEADLK;
			return false;
		}

		if (currentSize_ + length > maxSize_) {
			errno = EBUSY;
			return false;
		}
	}

	queue_.emplace(jobId, jobType, data, length);
	currentSize_ += length;

	notEmpty_.notify_one();
	return true;
}

bool ProducerConsumerQueue::get(uint32_t *jobId, uint32_t *jobType,
                                uint8_t **data, uint32_t *length) {
	TRACETHIS();
	std::unique_lock<std::mutex> lock(mutex_);
	notEmpty_.wait(lock, [this] { return !queue_.empty(); });

	auto &entry = queue_.front();
	currentSize_ -= entry.length;

	if (jobId) { *jobId = entry.jobId; }
	if (jobType) { *jobType = entry.jobType; }
	if (data) { *data = entry.data; }
	if (length) { *length = entry.length; }

	queue_.pop();

	notFull_.notify_one();
	return true;
}

bool ProducerConsumerQueue::tryGet(uint32_t *jobId, uint32_t *jobType,
                                   uint8_t **data, uint32_t *length) {
	TRACETHIS();
	std::lock_guard<std::mutex> lock(mutex_);
	if (queue_.empty()) {
		if (jobId) { *jobId = 0; }
		if (jobType) { *jobType = 0; }
		if (data) { *data = nullptr; }
		if (length) { *length = 0; }
		errno = EBUSY;
		return false;
	}

	auto &entry = queue_.front();
	currentSize_ -= entry.length;

	if (jobId) { *jobId = entry.jobId; }
	if (jobType) { *jobType = entry.jobType; }
	if (data) { *data = entry.data; }
	if (length) { *length = entry.length; }

	queue_.pop();
	notFull_.notify_one();
	return true;
}
