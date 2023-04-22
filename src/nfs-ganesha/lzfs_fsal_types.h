/**
 * @file   lzfs_fsal_types.h
 * @author Rubén Alcolea Núñez <ruben.crash@gmail.com>
 * @date Tue Aug 29 19:01 2022
 *
 * @brief File System Abstraction Layer types and constants.
 *
 * This file includes declarations of data types, variables and constants
 * for LizardFS FSAL.
 */

#pragma once

#include "fsal_api.h"

#include "mount/client/lizardfs_c_api.h"
#include "fileinfo_cache.h"

/*
 * In this file, you must define your own FSAL internal types.
 */

#define LIZARDFS_VERSION(major, minor, micro) \
                (0x010000 * major + 0x0100 * minor + micro)
#define kDisconnectedChunkServerVersion LIZARDFS_VERSION(256, 0, 0)

#define MFS_NAME_MAX 255

#ifndef MFSBLOCKSIZE
#define MFSBLOCKSIZE 65536
#endif

#define SPECIAL_INODE_BASE 0xFFFFFFF0U
#define SPECIAL_INODE_ROOT 0x01U
#define MAX_REGULAR_INODE  (SPECIAL_INODE_BASE - 0x01U)

#define LZFS_SUPPORTED_ATTRS \
       (ATTR_TYPE   | ATTR_SIZE  | ATTR_FSID     | \
        ATTR_FILEID | ATTR_MODE  | ATTR_NUMLINKS | ATTR_OWNER  | ATTR_GROUP     | \
        ATTR_ATIME  | ATTR_CTIME | ATTR_MTIME    | ATTR_CHANGE | ATTR_SPACEUSED | \
        ATTR_RAWDEV | ATTR_ACL   | ATTR4_XATTR)

#define LZFS_BIGGEST_STRIPE_COUNT 4096
#define LZFS_STD_CHUNK_PART_TYPE  0
#define LZFS_EXPECTED_BACKUP_DS_COUNT 3
#define TCP_PROTO_NUMBER 6


typedef liz_fileinfo_t fileinfo_t;

/**
 * @struct FSModule lzfs_fsal_types.h [lzfs_fsal_types.h]
 *
 * @brief LizardFS Main global module object.
 *
 * FSModule contains the global module object, FSAL object
 * operations vector and the parameters of the filesystem info.
 */
struct FSModule {
	struct fsal_module module;
	struct fsal_obj_ops operations;
	fsal_staticfsinfo_t filesystemInfo;
	bool onlyOneUser;
};

extern struct FSModule LizardFS;

// forward reference
struct FSHandle;

/**
 * @struct FSExport lzfs_fsal_types.h [lzfs_fsal_types.h]
 *
 * @brief LizardFS private export object.
 *
 * FSExport contains information related with the export,
 * the filesystem operations, the parameters used to connect
 * to the master server, the cache used and the pNFS support.
 */
struct FSExport {
	struct fsal_export export; /// Export object
	struct FSHandle *rootHandle; /// root handle of export

	liz_t *fsInstance; /// Filesystem instance
	liz_init_params_t initialParameters; /// Initial parameters
	FileInfoCache_t *fileinfoCache; /// Cache of export

	bool isMDSEnabled; /// pNFS Metadata Server enabled
	bool isDSEnabled;  /// pNFS Data Server enabled

	uint32_t cacheTimeout; /// Timeout for entries at cache
	uint32_t cacheMaximumSize; /// Maximum size of cache
};

/**
 * @struct FSFileDescriptor lzfs_fsal_types.h [lzfs_fsal_types.h]
 *
 * @brief LizardFS FSAL file descriptor.
 *
 * FSFileDescriptor works as a container to handle the information of a
 * file descriptor and its flags associated like open and share mode.
 */
struct FSFileDescriptor {
	fsal_openflags_t openFlags; /// The open and share mode
	struct liz_fileinfo *fileDescriptor; /// File descriptor instance
};

/**
 * @struct FSFileDescriptorState lzfs_fsal_types.h [lzfs_fsal_types.h]
 *
 * @brief Associates a single NFSv4 state structure with a file descriptor.
 */
struct FSFileDescriptorState {
	struct state_t state; /// Structure representing a single NFSv4 state
	struct FSFileDescriptor fileDescriptor; /// File descriptor instance
};

struct FSALKey {
	uint16_t moduleId; /// module id
	uint16_t exportId; /// export id
	liz_inode_t inode; /// inode
};

/**
 * @struct FSHandle lzfs_fsal_types.h [lzfs_fsal_types.h]
 *
 * @brief LizardFS FSAL handle.
 *
 * FSHandle is one of the most important concepts of LizardFS FSAL.
 *
 * It contains information related with the public structure of the
 * filesystem and its operations.
 */
struct FSHandle {
	struct fsal_obj_handle fileHandle; /// Public structure for filesystem objects.
	struct FSFileDescriptor fileDescriptor; /// File descriptor instance
	liz_inode_t inode; /// inode of file
	struct FSALKey uniqueKey; /// Key of the handle
	struct FSExport *export; /// Export to which the handle belongs
	struct fsal_share share; /// The ref counted share reservation state
};

struct DataServerWire {
	uint32_t inode; /// inode
};

struct DataServerHandle {
	struct fsal_ds_handle dsHandle; /// Public structure for DS file handles
	uint32_t inode; /// inode
	FileInfoEntry_t *cacheHandle; /// Cache entry for inode
};
