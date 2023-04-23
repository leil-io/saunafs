/*

   Copyright 2017 Skytechnology sp. z o.o.
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

#include "fileinfo_cache.h"

#include <gtest/gtest.h>

TEST(FileInfoCache, Basic) {
	FileInfoCache_t *cache = sau_create_fileinfo_cache(16, 0);

	FileInfoEntry_t *entry7 = sau_fileinfo_cache_acquire(cache, 7);
	FileInfoEntry_t *entry11 = sau_fileinfo_cache_acquire(cache, 11);
	ASSERT_EQ(sau_extract_fileinfo(entry7), nullptr);
	ASSERT_EQ(sau_extract_fileinfo(entry11), nullptr);

	sau_attach_fileinfo(entry7, (fileinfo_t *)0xb00b1e5);
	sau_attach_fileinfo(entry11, (fileinfo_t *)0xface);

	FileInfoEntry_t *expired = sau_fileinfo_cache_pop_expired(cache);
	ASSERT_EQ(expired, nullptr);
	sau_fileinfo_cache_release(cache, entry7);

	FileInfoEntry_t *entry7_2 = sau_fileinfo_cache_acquire(cache, 7);
	ASSERT_NE(sau_extract_fileinfo(entry7_2), nullptr);
	ASSERT_EQ(sau_extract_fileinfo(entry7_2), (fileinfo_t *)0xb00b1e5);
	ASSERT_EQ(sau_extract_fileinfo(entry11), (fileinfo_t *)0xface);

	expired = sau_fileinfo_cache_pop_expired(cache);
	ASSERT_EQ(expired, nullptr);

	sau_fileinfo_cache_release(cache, entry7_2);

	expired = sau_fileinfo_cache_pop_expired(cache);
	ASSERT_NE(expired, nullptr);
	fileInfoEntryFree(expired);

	sau_fileinfo_cache_release(cache, entry11);
	expired = sau_fileinfo_cache_pop_expired(cache);
	while (expired) {
		fileInfoEntryFree(expired);
		expired = sau_fileinfo_cache_pop_expired(cache);
	}

	sau_destroy_fileinfo_cache(cache);
}

TEST(FileInfoCache, Full) {
	FileInfoCache_t *cache = sau_create_fileinfo_cache(3, 0);

	auto a1 = sau_fileinfo_cache_acquire(cache, 1);
	auto a2 = sau_fileinfo_cache_acquire(cache, 1);
	auto a3 = sau_fileinfo_cache_acquire(cache, 1);
	auto a4 = sau_fileinfo_cache_acquire(cache, 1);

	ASSERT_NE(a1, nullptr);
	ASSERT_NE(a2, nullptr);
	ASSERT_NE(a3, nullptr);
	ASSERT_NE(a4, nullptr);

	sau_fileinfo_cache_erase(cache, a1);
	sau_fileinfo_cache_release(cache, a2);
	sau_fileinfo_cache_release(cache, a3);
	sau_fileinfo_cache_release(cache, a4);

	auto expired = sau_fileinfo_cache_pop_expired(cache);
	ASSERT_NE(expired, nullptr);
	fileInfoEntryFree(expired);

	expired = sau_fileinfo_cache_pop_expired(cache);
	ASSERT_NE(expired, nullptr);
	fileInfoEntryFree(expired);

	expired = sau_fileinfo_cache_pop_expired(cache);
	ASSERT_NE(expired, nullptr);
	fileInfoEntryFree(expired);

	expired = sau_fileinfo_cache_pop_expired(cache);
	ASSERT_EQ(expired, nullptr);

	sau_destroy_fileinfo_cache(cache);
}

TEST(FileInfoCache, Reset) {
	FileInfoCache_t *cache = sau_create_fileinfo_cache(100000, 100000);

	auto a1 = sau_fileinfo_cache_acquire(cache, 1);
	sau_fileinfo_cache_release(cache, a1);

	auto expired = sau_fileinfo_cache_pop_expired(cache);
	ASSERT_EQ(expired, nullptr);

	sau_reset_fileinfo_cache_params(cache, 0, 0);

	expired = sau_fileinfo_cache_pop_expired(cache);
	ASSERT_NE(expired, nullptr);
	fileInfoEntryFree(expired);

	sau_destroy_fileinfo_cache(cache);
}
