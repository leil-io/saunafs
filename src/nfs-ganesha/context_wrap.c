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

#include "context_wrap.h"
#include "lzfs_fsal_methods.h"

int fs_lookup(liz_t *instance, struct user_cred *cred, liz_inode_t parent,
              const char *path, struct liz_entry *entry) {
	liz_context_t *context __attribute__((cleanup(liz_destroy_context))) = NULL;
	context = createFSALContext(instance, cred);

	if (context == NULL) {
		return -1;
	}

	return liz_lookup(instance, context, parent, path, entry);
}

int fs_mknode(liz_t *instance, struct user_cred *cred, liz_inode_t parent,
              const char *path, mode_t mode, dev_t rdev,
              struct liz_entry *entry) {
	liz_context_t *context __attribute__((cleanup(liz_destroy_context))) = NULL;
	context = createFSALContext(instance, cred);

	if (context == NULL) {
		return -1;
	}

	return liz_mknod(instance, context, parent, path, mode, rdev, entry);
}

fileinfo_t *fs_open(liz_t *instance, struct user_cred *cred,
                    liz_inode_t inode, int flags) {
	liz_context_t *context __attribute__((cleanup(liz_destroy_context))) = NULL;
	context = createFSALContext(instance, cred);

	if (context == NULL) {
		return NULL;
	}

	return liz_open(instance, context, inode, flags);
}

ssize_t fs_read(liz_t *instance, struct user_cred *cred,
                fileinfo_t *fileinfo, off_t offset,
                size_t size, char *buffer) {
	liz_context_t *context __attribute__((cleanup(liz_destroy_context))) = NULL;
	context = createFSALContext(instance, cred);

	if (context == NULL) {
		return -1;
	}

	return liz_read(instance, context, fileinfo, offset, size, buffer);
}

ssize_t fs_write(liz_t *instance, struct user_cred *cred, fileinfo_t *fileinfo,
                 off_t offset, size_t size, const char *buffer) {
	liz_context_t *context __attribute__((cleanup(liz_destroy_context))) = NULL;
	context = createFSALContext(instance, cred);

	if (context == NULL) {
		return -1;
	}

	return liz_write(instance, context, fileinfo, offset, size, buffer);
}

int fs_flush(liz_t *instance, struct user_cred *cred, fileinfo_t *fileinfo) {
	liz_context_t *context __attribute__((cleanup(liz_destroy_context))) = NULL;
	context = createFSALContext(instance, cred);

	if (context == NULL) {
		return -1;
	}

	return liz_flush(instance, context, fileinfo);
}

int fs_getattr(liz_t *instance, struct user_cred *cred,
               liz_inode_t inode, struct liz_attr_reply *reply) {
	liz_context_t *context __attribute__((cleanup(liz_destroy_context))) = NULL;
	context = createFSALContext(instance, cred);

	if (context == NULL) {
		return -1;
	}

	return liz_getattr(instance, context, inode, reply);
}

fileinfo_t *fs_opendir(liz_t *instance, struct user_cred *cred,
                       liz_inode_t inode) {
	liz_context_t *context __attribute__((cleanup(liz_destroy_context))) = NULL;
	context = createFSALContext(instance, cred);

	if (context == NULL) {
		return NULL;
	}

	return liz_opendir(instance, context, inode);
}

int fs_readdir(liz_t *instance, struct user_cred *cred,
               struct liz_fileinfo *fileinfo, off_t offset,
               size_t max_entries, struct liz_direntry *buf,
               size_t *num_entries) {
	liz_context_t *context __attribute__((cleanup(liz_destroy_context))) = NULL;
	context = createFSALContext(instance, cred);

	if (context == NULL) {
		return -1;
	}

	return liz_readdir(instance, context, fileinfo, offset, max_entries, buf,
	                   num_entries);
}

int fs_mkdir(liz_t *instance, struct user_cred *cred, liz_inode_t parent,
             const char *name, mode_t mode, struct liz_entry *out_entry) {
	liz_context_t *context __attribute__((cleanup(liz_destroy_context))) = NULL;
	context = createFSALContext(instance, cred);

	if (context == NULL) {
		return -1;
	}

	return liz_mkdir(instance, context, parent, name, mode, out_entry);
}

int fs_rmdir(liz_t *instance, struct user_cred *cred, liz_inode_t parent,
             const char *name) {
	liz_context_t *context __attribute__((cleanup(liz_destroy_context))) = NULL;
	context = createFSALContext(instance, cred);

	if (context == NULL) {
		return -1;
	}

	return liz_rmdir(instance, context, parent, name);
}

int fs_unlink(liz_t *instance, struct user_cred *cred,
              liz_inode_t parent, const char *name) {
	liz_context_t *context __attribute__((cleanup(liz_destroy_context))) = NULL;
	context = createFSALContext(instance, cred);

	if (context == NULL) {
		return -1;
	}

	return liz_unlink(instance, context, parent, name);
}

int fs_setattr(liz_t *instance, struct user_cred *cred,
               liz_inode_t inode, struct stat *stbuf, int to_set,
               struct liz_attr_reply *reply) {
	liz_context_t *context __attribute__((cleanup(liz_destroy_context))) = NULL;
	context = createFSALContext(instance, cred);

	if (context == NULL) {
		return -1;
	}

	return liz_setattr(instance, context, inode, stbuf, to_set, reply);
}

int fs_fsync(liz_t *instance, struct user_cred *cred,
             struct liz_fileinfo *fileinfo) {
	liz_context_t *context __attribute__((cleanup(liz_destroy_context))) = NULL;
	context = createFSALContext(instance, cred);

	if (context == NULL) {
		return -1;
	}

	return liz_fsync(instance, context, fileinfo);
}

int fs_rename(liz_t *instance, struct user_cred *cred,
              liz_inode_t parent, const char *name,
              liz_inode_t new_parent, const char *new_name) {
	liz_context_t *context __attribute__((cleanup(liz_destroy_context))) = NULL;
	context = createFSALContext(instance, cred);

	if (context == NULL) {
		return -1;
	}

	return liz_rename(instance, context, parent, name, new_parent, new_name);
}

int fs_symlink(liz_t *instance, struct user_cred *cred, const char *link,
               liz_inode_t parent, const char *name,
               struct liz_entry *entry) {
	liz_context_t *context __attribute__((cleanup(liz_destroy_context))) = NULL;
	context = createFSALContext(instance, cred);

	if (context == NULL) {
		return -1;
	}

	return liz_symlink(instance, context, link, parent, name, entry);
}

int fs_readlink(liz_t *instance, struct user_cred *cred,
                liz_inode_t inode, char *buf, size_t size) {
	liz_context_t *context __attribute__((cleanup(liz_destroy_context))) = NULL;
	context = createFSALContext(instance, cred);

	if (context == NULL) {
		return -1;
	}

	return liz_readlink(instance, context, inode, buf, size);
}

int fs_link(liz_t *instance, struct user_cred *cred, liz_inode_t inode,
            liz_inode_t parent, const char *name,
            struct liz_entry *entry) {
	liz_context_t *context __attribute__((cleanup(liz_destroy_context))) = NULL;
	context = createFSALContext(instance, cred);

	if (context == NULL) {
		return -1;
	}

	return liz_link(instance, context, inode, parent, name, entry);
}

int fs_get_chunks_info(liz_t *instance, struct user_cred *cred,
                       liz_inode_t inode, uint32_t chunk_index,
                       liz_chunk_info_t *buffer, uint32_t buffer_size,
                       uint32_t *reply_size) {
	liz_context_t *context __attribute__((cleanup(liz_destroy_context))) = NULL;
	context = createFSALContext(instance, cred);

	if (context == NULL) {
		return -1;
	}
	return liz_get_chunks_info(instance, context, inode, chunk_index, buffer,
	                           buffer_size, reply_size);
}

int fs_setacl(liz_t *instance, struct user_cred *cred,
              liz_inode_t inode, liz_acl_t *acl) {
	liz_context_t *context __attribute__((cleanup(liz_destroy_context))) = NULL;
	context = createFSALContext(instance, cred);

	if (context == NULL) {
		return -1;
	}

	return liz_setacl(instance, context, inode, acl);
}

int fs_getacl(liz_t *instance, struct user_cred *cred,
              liz_inode_t inode, liz_acl_t **acl) {
	liz_context_t *context __attribute__((cleanup(liz_destroy_context))) = NULL;
	context = createFSALContext(instance, cred);

	if (context == NULL) {
		return -1;
	}

	return liz_getacl(instance, context, inode, acl);
}

int fs_setlk(liz_t *instance, struct user_cred *cred,
             fileinfo_t *fileinfo, const liz_lock_info_t *lock) {
	liz_context_t *context __attribute__((cleanup(liz_destroy_context))) = NULL;
	context = createFSALContext(instance, cred);

	if (context == NULL) {
		return -1;
	}

	return liz_setlk(instance, context, fileinfo, lock, NULL, NULL);
}

int fs_getlk(liz_t *instance, struct user_cred *cred,
             fileinfo_t *fileinfo, liz_lock_info_t *lock) {
	liz_context_t *context = createFSALContext(instance, cred);

	if (context == NULL) {
		return -1;
	}

	return liz_getlk(instance, context, fileinfo, lock);
}

int fs_getxattr(liz_t *instance, struct user_cred *cred,
                liz_inode_t ino, const char *name, size_t size,
                size_t *out_size, uint8_t *buf) {
	liz_context_t *context __attribute__((cleanup(liz_destroy_context))) = NULL;
	context = createFSALContext(instance, cred);

	if (context == NULL) {
		return -1;
	}

	return liz_getxattr(instance, context, ino, name, size, out_size, buf);
}

int fs_setxattr(liz_t *instance, struct user_cred *cred,
                liz_inode_t ino, const char *name,
                const uint8_t *value, size_t size, int flags) {
	liz_context_t *context __attribute__((cleanup(liz_destroy_context))) = NULL;
	context = createFSALContext(instance, cred);

	if (context == NULL) {
		return -1;
	}

	return liz_setxattr(instance, context, ino, name, value, size, flags);
}

int fs_listxattr(liz_t *instance, struct user_cred *cred,
                 liz_inode_t ino, size_t size,
                 size_t *out_size, char *buf) {
	liz_context_t *context __attribute__((cleanup(liz_destroy_context))) = NULL;
	context = createFSALContext(instance, cred);

	if (context == NULL) {
		return -1;
	}

	return liz_listxattr(instance, context, ino, size, out_size, buf);
}

int fs_removexattr(liz_t *instance, struct user_cred *cred,
                   liz_inode_t ino, const char *name) {
	liz_context_t *context __attribute__((cleanup(liz_destroy_context))) = NULL;
	context = createFSALContext(instance, cred);

	if (context == NULL) {
		return -1;
	}

	return liz_removexattr(instance, context, ino, name);
}
