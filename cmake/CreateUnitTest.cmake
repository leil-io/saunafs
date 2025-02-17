#  Copyright 2017 Skytechnology sp. z o.o.
#  Copyright 2023 Leil Storage OÜ
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

function(create_unittest TEST_NAME)
  if(NOT BUILD_TESTS OR ARGC EQUAL 1)
    return()
  endif()
  list(REMOVE_AT ARGV 0)
  set(TEST_LIBRARY_NAME ${TEST_NAME}_unittest)

  add_library(${TEST_LIBRARY_NAME} ${ARGV})

  list(FIND UNITTEST_TEST_NAMES ${TEST_NAME} result)
  if (${result} EQUAL -1)
    set(TMP ${UNITTEST_TEST_NAMES})
    list(APPEND TMP ${TEST_NAME})
    set(UNITTEST_TEST_NAMES ${TMP} CACHE INTERNAL "" FORCE)
  endif()
endfunction(create_unittest)

function(link_unittest TEST_NAME)
  if(NOT BUILD_TESTS OR ARGC EQUAL 1)
    return()
  endif()
  list(REMOVE_AT ARGV 0)

  set(${TEST_NAME}_UNITTEST_LINKLIST ${ARGV} CACHE INTERNAL "" FORCE)
endfunction(link_unittest)
