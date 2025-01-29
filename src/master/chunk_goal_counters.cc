/*
   Copyright 2013-2014 EditShare
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

#include "master/chunk_goal_counters.h"

#include "common/goal.h"

void ChunkGoalCounters::addFile(uint8_t goal) {
	if (!GoalId::isValid(goal)) {
		throw ChunkGoalCounters::InvalidOperation(
				"Trying to add non-existent goal: " + std::to_string(goal));
	}

	/*
	 * For memory saving, max value of a single counter is 255.
	 * If a counter is about to reach 256, a new counter for the same value
	 * is created instead.
	 *
	 * Example:
	 * Counters state: (0, 255), (1, 14), (2, 20)
	 * After adding another 0 it becomes:
	 *                 (0, 1), (0, 255), (1, 14), (2, 20)
	 */
	auto it = std::lower_bound(counters_.begin(), counters_.end(), goal,
			[](const GoalCounter &counter, uint8_t other){
				return counter.goal < other;
			});
	if (it == counters_.end()
			|| it->goal != goal
			|| it->count == std::numeric_limits<uint8_t>::max()) {
		if (counters_.full()) {
			throw ChunkGoalCounters::InvalidOperation("There is no more space for goals");
		}
		counters_.insert(it, GoalCounter{goal, 1});
	} else {
		it->count++;
	}
}

void ChunkGoalCounters::removeFile(uint8_t goal) {
	auto it = std::lower_bound(counters_.begin(), counters_.end(), goal,
			[](const GoalCounter &counter, uint8_t other){
				return counter.goal < other;
			});
	if (it == counters_.end() || it->goal != goal) {
		throw ChunkGoalCounters::InvalidOperation(
				"Trying to remove non-existent goal: " + std::to_string(goal));
	}
	it->count--;
	if (it->count == 0) {
		counters_.erase(it);
	}
}

void ChunkGoalCounters::changeFileGoal(uint8_t prevGoal, uint8_t newGoal) {
	removeFile(prevGoal);
	addFile(newGoal);
}

uint32_t ChunkGoalCounters::fileCount() const {
	uint32_t sum = 0;
	for (auto counter : counters_) {
		sum += counter.count;
	}
	return sum;
}

uint8_t ChunkGoalCounters::highestIdGoal() const {
	if (counters_.empty()) {
		return 0;
	}
	return counters_.back().goal;
}
