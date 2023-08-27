/*
   Copyright 2017 Skytechnology sp. z o.o.

   This file is part of LizardFS.

   LizardFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   LizardFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with LizardFS. If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include "mount/client/lizardfs_c_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef liz_fileinfo_t fileinfo_t;

typedef struct FileInfoCache FileInfoCache_t;
typedef struct FileInfoEntry FileInfoEntry_t;

/*!
 * \brief Create fileinfo cache
 * \param max_entries max number of entries to be stored in cache
 * \param min_timeout_ms entries will not be removed until at least min_timeout_ms ms has passed
 * \return pointer to FileInfoCache_t structure
 * \post Destroy with destroyFileInfoCache function
 */
FileInfoCache_t *createFileInfoCache(unsigned maxEntries,
                                     int minTimeoutMilliseconds);

/*!
 * \brief Reset cache parameters
 * \param cache cache to be modified
 * \param max_entries max number of entries to be stored in cache
 * \param min_timeout_ms entries will not be removed until at least min_timeout_ms ms has passed
 */
void resetFileInfoCacheParameters(FileInfoCache_t *cache, unsigned maxEntries,
                                  int minTimeoutMilliseconds);

/*!
 * \brief Destroy fileinfo cache
 * \param cache pointer returned from createFileInfoCache
 */
void destroyFileInfoCache(FileInfoCache_t *cache);

/*!
* \brief Acquire fileinfo from cache
* \param cache cache to be modified
* \param inode Inode of a file
* \return Cache entry if succeeded, NULL if cache is full
* \attention entry->fileinfo will be NULL if file still needs to be open first
* \post Set fileinfo to a valid pointer after opening a file with liz_attach_fileinfo
*/
FileInfoEntry_t *acquireFileInfoCache(FileInfoCache_t *cache, liz_inode_t inode);

/*!
* \brief Release fileinfo from cache
* \param cache cache to be modified
* \param entry pointer returned from previous acquire() call
*/
void releaseFileInfoCache(FileInfoCache_t *cache, FileInfoEntry_t *entry);

/*!
* \brief Erase acquired entry
* \attention This function should be used if entry should not reside in cache
* (i.e. opening a file failed)
* \param cache cache to be modified
* \param entry pointer returned from previous acquire() call
*/
void eraseFileInfoCache(FileInfoCache_t *cache, FileInfoEntry_t *entry);

/*!
* \brief Get expired fileinfo from cache
* \param cache cache to be modified
* \return entry removed from cache
* \post use this entry to call release() on entry->fileinfo and free entry
* afterwards with fileInfoEntryFree
*/
FileInfoEntry_t *popExpiredFileInfoCache(FileInfoCache_t *cache);

/*!
 * \brief Free unused fileinfo cache entry
 * \param entry entry to be freed
 */
void fileInfoEntryFree(FileInfoEntry_t *entry);

/*!
* \brief Get fileinfo from cache entry
* \param cache cache to be modified
* \return fileinfo extracted from entry
*/
fileinfo_t *extractFileInfo(FileInfoEntry_t *entry);

/*!
* \brief Attach fileinfo to an existing cache entry
* \param entry entry to be modified
* \param fileinfo fileinfo to be attached to entry
*/
void attachFileInfo(FileInfoEntry_t *entry, fileinfo_t *fileinfo);

#ifdef __cplusplus
}
#endif
