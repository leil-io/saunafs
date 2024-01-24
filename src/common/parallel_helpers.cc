/*
	Copyright 2023-2024 Leil Storage OÜ

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
#include "parallel_helpers.h"

namespace parallel {

    void CriticalZone::run(std::mutex &mutex, std::function<void()> callback) {
        const std::lock_guard<std::mutex> lock(mutex);
        callback();
    }

} // parallel
