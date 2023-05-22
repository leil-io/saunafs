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

#include "fileinfo_cache.h"
#include "lzfs_fsal_types.h"

#include <time.h>
#include <abstract_mem.h>
#include <avltree.h>

struct FileInfoEntry {
	struct glist_head list_hook;
	struct avltree_node tree_hook;

	liz_inode_t inode;
	fileinfo_t *fileinfo;
	uint64_t timestamp;
	bool is_used;
	bool lookup;
};

struct FileInfoCache {
	struct glist_head lru_list;
	struct glist_head used_list;
	struct avltree entry_lookup;

	int entry_count;

	unsigned max_entries;
	int min_timeout_ms;

	pthread_mutex_t lock;
};

static uint64_t get_time_ms() {
	struct timespec ts;
	timespec_get(&ts, TIME_UTC);

	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000;
}

static int cache_entry_cmp_function(const struct avltree_node *a,
                                    const struct avltree_node *b) {
	FileInfoEntry_t *a_entry = avltree_container_of(a, FileInfoEntry_t,
	                                                tree_hook);
	FileInfoEntry_t *b_entry = avltree_container_of(b, FileInfoEntry_t,
	                                                tree_hook);

	if (a_entry->inode < b_entry->inode) {
		return -1;
	}
	if (a_entry->inode > b_entry->inode) {
		return 1;
	}
	if (a_entry->lookup || b_entry->lookup) {
		return 0;
	}
	if (a_entry < b_entry) {
		return -1;
	}
	if (a_entry > b_entry) {
		return 1;
	}
	return 0;
}

FileInfoCache_t *createFileInfoCache(unsigned max_entries, int min_timeout_ms) {
	FileInfoCache_t *cache;

	cache = gsh_calloc(1, sizeof(FileInfoCache_t));
	cache->max_entries = max_entries;
	cache->min_timeout_ms = min_timeout_ms;
	PTHREAD_MUTEX_init(&cache->lock, NULL);

	glist_init(&cache->lru_list);
	glist_init(&cache->used_list);
	avltree_init(&cache->entry_lookup, &cache_entry_cmp_function, 0);

	return cache;
}

void resetFileInfoCacheParameters(FileInfoCache_t *cache, unsigned max_entries,
                                  int min_timeout_ms) {
	PTHREAD_MUTEX_lock(&cache->lock);
	cache->max_entries = max_entries;
	cache->min_timeout_ms = min_timeout_ms;
	PTHREAD_MUTEX_unlock(&cache->lock);
}

void destroyFileInfoCache(FileInfoCache_t *cache) {
	FileInfoEntry_t *entry;

	if (!cache) {
		return;
	}

	while ((entry = glist_first_entry(&cache->used_list, FileInfoEntry_t,
	                                  list_hook))) {
		glist_del(&entry->list_hook);
		gsh_free(entry);
	}

	while ((entry = glist_first_entry(&cache->lru_list, FileInfoEntry_t,
	                                  list_hook))) {
		glist_del(&entry->list_hook);
		gsh_free(entry);
	}

	gsh_free(cache);
}

FileInfoEntry_t *acquireFileInfoCache(FileInfoCache_t *cache, liz_inode_t inode) {
	FileInfoEntry_t key, *entry;
	struct avltree_node *node;

	key.inode = inode;
	key.lookup = true;
	PTHREAD_MUTEX_lock(&cache->lock);

	node = avltree_lookup(&key.tree_hook, &cache->entry_lookup);
	if (node) {
		entry = avltree_container_of(node, FileInfoEntry_t, tree_hook);
		assert(!entry->is_used);

		glist_del(&entry->list_hook);
		glist_add(&cache->used_list, &entry->list_hook);
		avltree_remove(node, &cache->entry_lookup);
	}
	else {
		entry = gsh_calloc(1, sizeof(FileInfoEntry_t));
		glist_add(&cache->used_list, &entry->list_hook);
		cache->entry_count++;
	}

	entry->is_used = true;
	entry->inode = inode;
	entry->timestamp = get_time_ms();

	PTHREAD_MUTEX_unlock(&cache->lock);

	return entry;
}

void releaseFileInfoCache(FileInfoCache_t *cache, FileInfoEntry_t *entry) {
	PTHREAD_MUTEX_lock(&cache->lock);
	assert(entry->is_used);
	entry->is_used = false;
	entry->timestamp = get_time_ms();

	glist_del(&entry->list_hook);
	glist_add_tail(&cache->lru_list, &entry->list_hook);
	avltree_insert(&entry->tree_hook, &cache->entry_lookup);
	PTHREAD_MUTEX_unlock(&cache->lock);
}

void eraseFileInfoCache(FileInfoCache_t *cache, FileInfoEntry_t *entry) {
	PTHREAD_MUTEX_lock(&cache->lock);
	assert(entry->is_used);
	glist_del(&entry->list_hook);
	cache->entry_count--;
	PTHREAD_MUTEX_unlock(&cache->lock);
	gsh_free(entry);
}

FileInfoEntry_t *popExpiredFileInfoCache(FileInfoCache_t *cache) {
	PTHREAD_MUTEX_lock(&cache->lock);

	FileInfoEntry_t *entry =
	    glist_first_entry(&cache->lru_list, FileInfoEntry_t, list_hook);

	if (!entry) {
		PTHREAD_MUTEX_unlock(&cache->lock);
		return NULL;
	}

	bool is_full = cache->entry_count > cache->max_entries;
	int timeout = is_full ? 0 : cache->min_timeout_ms;
	uint64_t current_time = get_time_ms();

	if ((current_time - entry->timestamp) >= timeout) {
		glist_del(&entry->list_hook);
		avltree_remove(&entry->tree_hook, &cache->entry_lookup);
		cache->entry_count--;
	}
	else {
		entry = NULL;
	}

	PTHREAD_MUTEX_unlock(&cache->lock);
	return entry;
}

void fileInfoEntryFree(FileInfoEntry_t *entry) {
	assert(!entry->is_used);
	gsh_free(entry);
}

fileinfo_t *extractFileInfo(FileInfoEntry_t *entry) {
	return entry->fileinfo;
}

void attachFileInfo(FileInfoEntry_t *entry, fileinfo_t *fileinfo) {
	entry->fileinfo = fileinfo;
}
