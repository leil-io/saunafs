/*
   Copyright 2022 LizardFS sp. z o.o.

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

#include <fsal_types.h>

#include "lzfs_fsal_types.h"

int fs_lookup(liz_t *instance, struct user_cred *cred,
              liz_inode_t parent, const char *path,
              struct liz_entry *entry);

int fs_mknode(liz_t *instance, struct user_cred *cred, liz_inode_t parent,
              const char *path, mode_t mode, dev_t rdev,
              struct liz_entry *entry);

fileinfo_t *fs_open(liz_t *instance, struct user_cred *cred,
                    liz_inode_t inode, int flags);

ssize_t fs_read(liz_t *instance, struct user_cred *cred,
                fileinfo_t *fileinfo, off_t offset,
                size_t size, char *buffer);

ssize_t fs_write(liz_t *instance, struct user_cred *cred,
                 fileinfo_t *fileinfo, off_t offset,
                 size_t size, const char *buffer);

int fs_flush(liz_t *instance, struct user_cred *cred,
             fileinfo_t *fileinfo);

int fs_getattr(liz_t *instance, struct user_cred *cred,
               liz_inode_t inode, struct liz_attr_reply *reply);

fileinfo_t *fs_opendir(liz_t *instance, struct user_cred *cred,
                       liz_inode_t inode);

int fs_readdir(liz_t *instance, struct user_cred *cred,
               struct liz_fileinfo *fileinfo, off_t offset,
               size_t max_entries, struct liz_direntry *buf,
               size_t *num_entries);

int fs_mkdir(liz_t *instance, struct user_cred *cred, liz_inode_t parent,
             const char *name, mode_t mode, struct liz_entry *out_entry);

int fs_rmdir(liz_t *instance, struct user_cred *cred, liz_inode_t parent,
             const char *name);

int fs_unlink(liz_t *instance, struct user_cred *cred,
              liz_inode_t parent, const char *name);

int fs_setattr(liz_t *instance, struct user_cred *cred,
               liz_inode_t inode, struct stat *stbuf, int to_set,
               struct liz_attr_reply *reply);

int fs_fsync(liz_t *instance, struct user_cred *cred,
             struct liz_fileinfo *fileinfo);

int fs_rename(liz_t *instance, struct user_cred *cred,
              liz_inode_t parent, const char *name,
              liz_inode_t new_parent, const char *new_name);

int fs_symlink(liz_t *instance, struct user_cred *cred, const char *link,
               liz_inode_t parent, const char *name,
               struct liz_entry *entry);

int fs_readlink(liz_t *instance, struct user_cred *cred,
                liz_inode_t inode, char *buf, size_t size);

int fs_link(liz_t *instance, struct user_cred *cred, liz_inode_t inode,
            liz_inode_t parent, const char *name,
            struct liz_entry *entry);

int fs_get_chunks_info(liz_t *instance, struct user_cred *cred,
                       liz_inode_t inode, uint32_t chunk_index,
                       liz_chunk_info_t *buffer, uint32_t buffer_size,
                       uint32_t *reply_size);

int fs_setacl(liz_t *instance, struct user_cred *cred,
              liz_inode_t inode, liz_acl_t *acl);

int fs_getacl(liz_t *instance, struct user_cred *cred,
              liz_inode_t inode, liz_acl_t **acl);

int fs_setlk(liz_t *instance, struct user_cred *cred,
             fileinfo_t *fileinfo, const liz_lock_info_t *lock);

int fs_getlk(liz_t *instance, struct user_cred *cred,
             fileinfo_t *fileinfo, liz_lock_info_t *lock);

int fs_getxattr(liz_t *instance, struct user_cred *cred,
                liz_inode_t ino, const char *name,
                size_t size, size_t *out_size, uint8_t *buf);

int fs_setxattr(liz_t *instance, struct user_cred *cred,
                liz_inode_t ino, const char *name,
                const uint8_t *value, size_t size, int flags);

int fs_listxattr(liz_t *instance, struct user_cred *cred,
                 liz_inode_t ino, size_t size,
                 size_t *out_size, char *buf);

int fs_removexattr(liz_t *instance, struct user_cred *cred,
                   liz_inode_t ino, const char *name);
