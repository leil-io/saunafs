#  Copyright 2015-2017 Skytechnology sp. z o.o.
#  Copyright 2023      Leil Storage OÜ
#
#  This file is part of SaunaFS.
#
#  SaunaFS is free software: you can redistribute it and/or modify
#  it under the terms of the GNU General Public License as published by
#  the Free Software Foundation, version 3.
#
#  SaunaFS is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
#  You should have received a copy of the GNU General Public License
#  along with SaunaFS  If not, see <http://www.gnu.org/licenses/>.

include_directories(${CMAKE_CURRENT_SOURCE_DIR})
if (ENABLE_PROMETHEUS AND NOT SYSTEM_PROMETHEUS)
    include(FetchContent)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(ENABLE_PULL ON CACHE BOOL "" FORCE)
    set(ENABLE_PUSH OFF CACHE BOOL "" FORCE)
    set(ENABLE_COMPRESSION ON CACHE BOOL "" FORCE)

    FetchContent_Declare(
      prometheus-cpp
      GIT_REPOSITORY https://github.com/jupp0r/prometheus-cpp.git
      GIT_TAG        e5fada43131d251e9c4786b04263ce98b6767ba5 # v.1.3.0
    )
    FetchContent_MakeAvailable(prometheus-cpp)

    unset(BUILD_SHARED_LIBS)
    unset(ENABLE_PULL)
    unset(ENABLE_PUSH)
    unset(ENABLE_COMPRESSION)

endif()

collect_sources(METRICS)

add_library(metrics ${METRICS_SOURCES})

if (ENABLE_PROMETHEUS)
  target_link_libraries(metrics slogger prometheus-cpp::pull)
endif()
