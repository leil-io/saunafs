/**
 * @file   safs_fsal_types.h
 * @author Rubén Alcolea Núñez <ruben.crash@gmail.com>
 * @date Tue Aug 29 19:01 2022
 *
 * @brief File System Abstraction Layer types and constants.
 *
 * This file includes declarations of data types, variables and constants
 * for SaunaFS FSAL.
 */

#pragma once

#include "fsal_api.h"

#include "mount/client/saunafs_c_api.h"
#include "fileinfo_cache.h"

/*
 * In this file, you must define your own FSAL internal types.
 */

#define SAUNAFS_VERSION(major, minor, micro) \
                (0x010000 * major + 0x0100 * minor + micro)
#define kDisconnectedChunkServerVersion SAUNAFS_VERSION(256, 0, 0)

#define SFS_NAME_MAX 255

#ifndef SFSBLOCKSIZE
#define SFSBLOCKSIZE 65536
#endif

#define SPECIAL_INODE_BASE 0xFFFFFFF0U
#define SPECIAL_INODE_ROOT 0x01U
#define MAX_REGULAR_INODE  (SPECIAL_INODE_BASE - 0x01U)

#define SAFS_SUPPORTED_ATTRS \
       (ATTR_TYPE     | ATTR_SIZE   | ATTR_FSID   | ATTR_FILEID | ATTR_MODE  | \
        ATTR_NUMLINKS | ATTR_OWNER  | ATTR_GROUP  | ATTR_ATIME  | ATTR_CTIME | \
        ATTR_MTIME    | ATTR_CHANGE | ATTR_SPACEUSED | ATTR_RAWDEV |           \
        ATTR_ACL      | ATTR4_XATTR)

#define SAFS_BIGGEST_STRIPE_COUNT 4096
#define SAFS_STD_CHUNK_PART_TYPE  0
#define SAFS_EXPECTED_BACKUP_DS_COUNT 3
#define TCP_PROTO_NUMBER 6

typedef sau_fileinfo_t fileinfo_t;

/**
 * @struct FSModule safs_fsal_types.h [safs_fsal_types.h]
 *
 * @brief SaunaFS Main global module object.
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

extern struct FSModule SaunaFS;

// forward reference
struct FSHandle;

/**
 * @struct FSExport safs_fsal_types.h [safs_fsal_types.h]
 *
 * @brief SaunaFS private export object.
 *
 * FSExport contains information related with the export,
 * the filesystem operations, the parameters used to connect
 * to the master server, the cache used and the pNFS support.
 */
struct FSExport {
	struct fsal_export export; /// Export object
	struct FSHandle *rootHandle; /// root handle of export

	sau_t *fsInstance; /// Filesystem instance
	sau_init_params_t initialParameters; /// Initial parameters
	FileInfoCache_t *fileinfoCache; /// Cache of export

	bool isMDSEnabled; /// pNFS Metadata Server enabled
	bool isDSEnabled;  /// pNFS Data Server enabled

	uint32_t cacheTimeout; /// Timeout for entries at cache
	uint32_t cacheMaximumSize; /// Maximum size of cache
};

/**
 * @struct FSFileDescriptor safs_fsal_types.h [safs_fsal_types.h]
 *
 * @brief SaunaFS FSAL file descriptor.
 *
 * FSFileDescriptor works as a container to handle the information of a
 * file descriptor and its flags associated like open and share mode.
 */
struct FSFileDescriptor {
	fsal_openflags_t openFlags; /// The open and share mode
	struct sau_fileinfo *fileDescriptor; /// File descriptor instance
};

/**
 * @struct FSFileDescriptorState safs_fsal_types.h [safs_fsal_types.h]
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
	sau_inode_t inode; /// inode
};

/**
 * @struct FSHandle safs_fsal_types.h [safs_fsal_types.h]
 *
 * @brief SaunaFS FSAL handle.
 *
 * FSHandle is one of the most important concepts of SaunaFS FSAL.
 *
 * It contains information related with the public structure of the
 * filesystem and its operations.
 */
struct FSHandle {
	struct fsal_obj_handle fileHandle; /// Public structure for filesystem objects.
	struct FSFileDescriptor fileDescriptor; /// File descriptor instance
	sau_inode_t inode; /// inode of file
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
