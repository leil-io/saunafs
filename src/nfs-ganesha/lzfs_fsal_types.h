/**
 * @file   lzfs_fsal_types.h
 * @author Rubén Alcolea Núñez <ruben.crash@gmail.com>
 * @date Tue Aug 29 19:01 2022
 *
 * @brief File System Abstraction Layer types and constants.
 *
 * This file includes declarations of data types, variables, and constants
 * for the CRASH FSAL.
 */

#ifndef LZFS_FSAL_TYPES
#define LZFS_FSAL_TYPES

#include <sys/stat.h>
#include "fsal.h"
#include "fsal_api.h"
#include "fsal_convert.h"
#include "fsal_handle_syscalls.h"
#include <stdbool.h>
#include <uuid/uuid.h>

#include "lizardfs/lizardfs_c_api.h"
#include "fileinfo_cache.h"

/*
 * In this section, you must define your own FSAL internal types.
 */

#define LIZARDFS_VERSION(major, minor, micro) \
                (0x010000 * major + 0x0100 * minor + micro)
#define kDisconnectedChunkserverVersion LIZARDFS_VERSION(256, 0, 0)

#define MFS_NAME_MAX				255
#ifndef MFSBLOCKSIZE
#define MFSBLOCKSIZE				65536
#endif
#define MFSCHUNKSIZE				(65536 * 1024)

#define SPECIAL_INODE_BASE          0xFFFFFFF0U

#define SPECIAL_INODE_ROOT			0x01U

#define MAX_REGULAR_INODE (SPECIAL_INODE_BASE - 0x01U)

#define LZFS_SUPPORTED_ATTRS						\
    (ATTR_TYPE | ATTR_SIZE | ATTR_FSID | ATTR_FILEID | ATTR_MODE |	\
     ATTR_NUMLINKS | ATTR_OWNER | ATTR_GROUP | ATTR_ATIME |		\
     ATTR_CTIME | ATTR_MTIME | ATTR_CHANGE | ATTR_SPACEUSED |	\
     ATTR_RAWDEV | ATTR_ACL  | ATTR4_XATTR)

#define LZFS_BIGGEST_STRIPE_COUNT		4096
#define LZFS_STD_CHUNK_PART_TYPE           0
#define LZFS_EXPECTED_BACKUP_DS_COUNT	   3
#define TCP_PROTO_NUMBER                   6

struct lzfs_fsal_module {
    struct fsal_module module;
    struct fsal_obj_ops handle_ops;
    fsal_staticfsinfo_t fs_info;
    bool only_one_user;
};

extern struct lzfs_fsal_module LizardFS;

/*
 * LizardFS internal export
 */

// forward reference
struct lzfs_fsal_handle;

// internal fsal export
struct lzfs_fsal_export {
    struct fsal_export export;      /*< The public export object */

    liz_t *lzfs_instance;
    struct lzfs_fsal_handle *root;  /*< The root handle */

    liz_fileinfo_cache_t *fileinfo_cache;

    bool pnfs_mds_enabled;
    bool pnfs_ds_enabled;
    uint32_t fileinfo_cache_timeout;
    uint32_t fileinfo_cache_max_size;
    liz_init_params_t lzfs_params;
};

struct lzfs_fd {
    /** The open and share mode etc. */
    fsal_openflags_t openflags;
    struct liz_fileinfo *fd;
};

struct lzfs_state_fd {
    struct state_t state;
    struct lzfs_fd fd;
};

struct lzfs_fsal_key {
    uint16_t module_id;
    uint16_t export_id;
    liz_inode_t inode;
};

/*
 * LizardFS internal object handle
 *
 * It contains a pointer to the fsal_obj_handle used by the subfsal.
 *
 * AF_UNIX sockets are strange ducks. I personally cannot see why they
 * are here except for the ability of a client to see such an animal with
 * an 'ls' or get rid of one with an 'rm'.  You can't open them in the
 * usual file way so open_by_handle_at leads to a deadend.  To work around
 * this, we save the args that were used to mknod or lookup the socket.
 */

struct lzfs_fsal_handle {
    struct fsal_obj_handle handle; /*< The public handle */
    struct lzfs_fd fd;
    liz_inode_t inode;
    struct lzfs_fsal_key unique_key;
    struct lzfs_fsal_export *export;
    struct fsal_share share;
};

struct lzfs_fsal_ds_wire {
    uint32_t inode;
};

struct lzfs_fsal_ds_handle {
    struct fsal_ds_handle ds;
    uint32_t inode;
    liz_fileinfo_entry_t *cache_handle;
};

/*
 * Structure to tell subfunctions whether they should close the
 * returned fd or not
 */
struct closefd {
    int fd;
    int close_fd;
};


#endif // LZFS_FSAL_TYPES
