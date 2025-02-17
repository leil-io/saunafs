/*
   Copyright 2024      Leil Storage OÜ

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

#include "common/pcqueue.h"
#include <gtest/gtest.h>
#include <cstdint>
#include <thread>
#include <vector>

// Custom deleter for testing
void customDeleter(uint8_t *data) { delete[] data; }

const int kMaxSize = 10;
const uint32_t kMaxLength = 10;
const uint32_t kMaxQueueSize = 100;

// Test adding and removing a single element
TEST(ProducerConsumerQueueTests, SingleElement) {
	ProducerConsumerQueue queue(kMaxSize, customDeleter);
	auto *data = new uint8_t[kMaxLength];
	EXPECT_TRUE(queue.put(1, 1, data, kMaxLength));

	uint32_t jobId = 0;
	uint32_t jobType = 0;
	uint32_t length = 0;
	uint8_t *retrievedData = nullptr;

	EXPECT_TRUE(queue.get(&jobId, &jobType, &retrievedData, &length));
	EXPECT_EQ(jobId, 1U);
	EXPECT_EQ(jobType, 1U);
	EXPECT_EQ(length, kMaxLength);
	EXPECT_EQ(retrievedData, data);
}

// Test adding and removing multiple elements
TEST(ProducerConsumerQueueTests, MultipleElements) {
	ProducerConsumerQueue queue(kMaxQueueSize, customDeleter);
	for (int i = 0; i < kMaxSize; ++i) {
		auto *data = new uint8_t[kMaxLength];
		EXPECT_TRUE(queue.put(i, i, data, kMaxLength));
	}

	for (uint32_t i = 0; i < kMaxSize; ++i) {
		uint32_t jobId = 0;
		uint32_t jobType = 0;
		uint32_t length = 0;
		uint8_t *retrievedData = nullptr;

		EXPECT_TRUE(queue.get(&jobId, &jobType, &retrievedData, &length));
		EXPECT_EQ(jobId, i);
		EXPECT_EQ(jobType, i);
		EXPECT_EQ(length, kMaxLength);
	}
}

// Test behavior when the queue is full
TEST(ProducerConsumerQueueTests, QueueFull) {
	ProducerConsumerQueue queue(2, customDeleter);
	auto *data1 = new uint8_t[kMaxLength];
	auto *data2 = new uint8_t[kMaxLength];
	auto *data3 = new uint8_t[kMaxLength];
	EXPECT_TRUE(queue.put(1, 1, data1, 1));
	EXPECT_TRUE(queue.put(2, 2, data2, 1));
	EXPECT_FALSE(queue.tryPut(3, 3, data3, kMaxLength));
	delete[] data3;
}

// Test behavior when the queue is empty
TEST(ProducerConsumerQueueTests, QueueEmpty) {
	ProducerConsumerQueue queue(kMaxSize, customDeleter);
	uint32_t jobId = 0;
	uint32_t jobType = 0;
	uint32_t length = 0;
	uint8_t *retrievedData = nullptr;
	EXPECT_FALSE(queue.tryGet(&jobId, &jobType, &retrievedData, &length));
}

// Test concurrent access by multiple producer threads
TEST(ProducerConsumerQueueTests, MultipleProducers) {
	const int kMaxProducersSize = 10;
	const int kMaxInsertionsPerThread = 10;

	ProducerConsumerQueue queue(kMaxQueueSize, customDeleter);
	std::vector<std::thread> producers;
	producers.reserve(kMaxProducersSize);

	for (int i = 0; i < kMaxProducersSize; ++i) {
		producers.emplace_back([&queue, i]() {
			for (int j = 0; j < kMaxInsertionsPerThread; ++j) {
				auto *data = new uint8_t[kMaxLength];
				queue.put(i * kMaxSize + j, i, data, 1);
			}
		});
	}

	for (auto &producer : producers) { producer.join(); }
	EXPECT_EQ(queue.elements(), kMaxQueueSize);
}

// Test concurrent access by multiple consumer threads
TEST(ProducerConsumerQueueTests, MultipleConsumers) {
	const int kMaxConsumersSize = 10;
	const int kMaxRemovalsPerThread = 10;

	ProducerConsumerQueue queue(kMaxQueueSize, customDeleter);
	for (uint32_t i = 0; i < kMaxQueueSize; ++i) {
		auto *data = new uint8_t[kMaxLength];
		queue.put(i, i, data, 1);
	}

	std::vector<std::thread> consumers;
	consumers.reserve(kMaxConsumersSize);

	for (int i = 0; i < kMaxConsumersSize; ++i) {
		consumers.emplace_back([&queue]() {
			for (int j = 0; j < kMaxRemovalsPerThread; ++j) {
				uint32_t jobId = 0;
				uint32_t jobType = 0;
				uint32_t length = 0;
				uint8_t *retrievedData = nullptr;
				queue.get(&jobId, &jobType, &retrievedData, &length);
			}
		});
	}

	for (auto &consumer : consumers) { consumer.join(); }
	EXPECT_TRUE(queue.isEmpty());
}

// Test concurrent access by both producer and consumer threads
TEST(ProducerConsumerQueueTests, ProducersAndConsumers) {
	const int kMaxProducersSize = 10;
	const int kMaxConsumersSize = 10;
	const int kMaxInsertionsPerThread = 10;
	const int kMaxRemovalsPerThread = 10;

	ProducerConsumerQueue queue(kMaxQueueSize, customDeleter);
	std::vector<std::thread> producers;
	producers.reserve(kMaxProducersSize);

	std::vector<std::thread> consumers;
	consumers.reserve(kMaxConsumersSize);

	for (int i = 0; i < kMaxProducersSize; ++i) {
		producers.emplace_back([&queue, i]() {
			for (int j = 0; j < kMaxInsertionsPerThread; ++j) {
				auto *data = new uint8_t[kMaxLength];
				queue.put(i * kMaxProducersSize + j, i, data, kMaxLength);
			}
		});
	}

	for (int i = 0; i < kMaxConsumersSize; ++i) {
		consumers.emplace_back([&queue]() {
			for (int j = 0; j < kMaxRemovalsPerThread; ++j) {
				uint32_t jobId = 0;
				uint32_t jobType = 0;
				uint32_t length = 0;
				uint8_t *retrievedData = nullptr;
				queue.get(&jobId, &jobType, &retrievedData, &length);
			}
		});
	}

	for (auto &producer : producers) { producer.join(); }
	for (auto &consumer : consumers) { consumer.join(); }

	EXPECT_TRUE(queue.isEmpty());
}

// Test using a custom deleter
TEST(ProducerConsumerQueueTests, CustomDeleter) {
	bool deleterCalled = false;
	auto customDeleter = [&deleterCalled](uint8_t *data) {
		deleterCalled = true;
		delete[] data;
	};

	{
		ProducerConsumerQueue queue(kMaxSize, customDeleter);
		auto *data = new uint8_t[kMaxLength];
		queue.put(1, 1, data, kMaxLength);
	}

	EXPECT_TRUE(deleterCalled);
}
