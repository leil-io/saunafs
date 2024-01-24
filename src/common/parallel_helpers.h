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

#pragma once

#include "common/platform.h"

#include <functional>
#include <mutex>

namespace parallel {

    class CriticalZone {
    public:
        static void run(std::mutex &mutex, std::function<void()> callback);
    };

} // parallel
