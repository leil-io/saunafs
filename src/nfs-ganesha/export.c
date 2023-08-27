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

#include "fsal_convert.h"
#include "FSAL/fsal_commonlib.h"
#include "FSAL/fsal_config.h"
#include "nfs_exports.h"
#include "lzfs_fsal_methods.h"
#include "context_wrap.h"

/* Flags to determine if ACLs are supported */
#define NFSv4_ACL_SUPPORT (!op_ctx_export_has_option(EXPORT_OPTION_DISABLE_ACL))

/**
 * @brief Finalize an export
 *
 * This function is called as part of cleanup when the last reference to an
 * export is released and it is no longer part of the list.
 *
 * It should clean up all private resources and destroy the object.
 *
 * @param[in] exportHandle     The export to release
 */
static void release(struct fsal_export *exportHandle) {
	struct FSExport *export = NULL;
	export = container_of(exportHandle, struct FSExport, export);

	deleteHandle(export->rootHandle);
	export->rootHandle = NULL;

	fsal_detach_export(export->export.fsal, &export->export.exports);
	free_export_ops(&export->export);

	if (export->fileinfoCache) {
		resetFileInfoCacheParameters(export->fileinfoCache, 0, 0);

		while (1) {
			FileInfoEntry_t *cacheHandle = NULL;
			fileinfo_t *fileHandle = NULL;

			cacheHandle = popExpiredFileInfoCache(export->fileinfoCache);
			if (!cacheHandle) {
				break;
			}

			fileHandle = extractFileInfo(cacheHandle);
			liz_release(export->fsInstance, fileHandle);
			fileInfoEntryFree(cacheHandle);
		}

		destroyFileInfoCache(export->fileinfoCache);
		export->fileinfoCache = NULL;
	}

	liz_destroy(export->fsInstance);
	export->fsInstance = NULL;

	gsh_free((char *)export->initialParameters.subfolder);
	gsh_free(export);
}

/**
 * @brief Look up a path.
 *
 * Create an object handles within this export.
 *
 * This function looks up a path within the export, it is now exclusively
 * used to get a handle for the root directory of the export.
 *
 * @param [in]     exportHandle     The export in which to look up
 * @param [in]     path             The path to look up
 * @param [out]    handle           The object found
 * @param [in,out] attributes       Optional attributes for newly created object
 *
 * \see fsal_api.h for more information
 *
 * @returns: FSAL status
 */
fsal_status_t lookup_path(struct fsal_export *exportHandle, const char *path,
                          struct fsal_obj_handle **handle,
                          struct fsal_attrlist *attributes) {
	static const char *rootDirPath = "/";

	struct FSExport *export = NULL;
	struct FSHandle *objectHandle = NULL;
	const char *realPath = NULL;

	LogFullDebug(COMPONENT_FSAL, "export_id=%" PRIu16 " path=%s",
	             exportHandle->export_id, path);

	export = container_of(exportHandle, struct FSExport, export);

	*handle = NULL;

	// set the real path to the path without the prefix from ctx_export->fullpath
	if (*path != '/') {
		realPath = strchr(path, ':');
		if (realPath == NULL) {
			return fsalstat(ERR_FSAL_INVAL, 0);
		}
		++realPath;
		if (*realPath != '/') {
			return fsalstat(ERR_FSAL_INVAL, 0);
		}
	}
	else {
		realPath = path;
	}

	if (strstr(realPath, CTX_FULLPATH(op_ctx)) != realPath) {
		return fsalstat(ERR_FSAL_SERVERFAULT, 0);
	}

	realPath += strlen(CTX_FULLPATH(op_ctx));
	if (*realPath == '\0') {
		realPath = rootDirPath;
	}

	LogFullDebug(COMPONENT_FSAL, "real path = %s", realPath);

	// special case the root
	if (strcmp(realPath, "/") == 0) {
		assert(export->rootHandle);
		*handle = &export->rootHandle->fileHandle;

		if (attributes == NULL) {
			return fsalstat(ERR_FSAL_NO_ERROR, 0);
		}
	}

	liz_entry_t entry;
	int status = fs_lookup(export->fsInstance, &op_ctx->creds,
	                       SPECIAL_INODE_ROOT, realPath, &entry);

	if (status < 0) {
		return fsalLastError();
	}

	if (attributes != NULL) {
		posix2fsal_attributes_all(&entry.attr, attributes);
	}

	if (*handle == NULL) {
		objectHandle = allocateNewHandle(&entry.attr, export);
		*handle = &objectHandle->fileHandle;
	}

	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/**
 * @brief Get dynamic filesystem statistics and configuration for this filesystem.
 *
 * This function gets information on inodes and space in use and free for a
 * filesystem. See fsal_dynamicfsinfo_t for details of what to fill out.
 *
 * @param[in]  exportHandle     Export handle to interrogate
 * @param[in]  objectHandle     Directory
 * @param[out] info             Buffer to fill with information
 *
 * @returns: FSAL status.
 */
static fsal_status_t get_dynamic_info(struct fsal_export *exportHandle,
                                      struct fsal_obj_handle *objectHandle,
                                      fsal_dynamicfsinfo_t *info) {
	(void) objectHandle;

	struct FSExport *export = NULL;
	export = container_of(exportHandle, struct FSExport, export);

	liz_stat_t statfsEntry;
	int status = liz_statfs(export->fsInstance, &statfsEntry);

	if (status < 0) {
		return fsalLastError();
	}

	memset(info, 0, sizeof(fsal_dynamicfsinfo_t));
	info->total_bytes = statfsEntry.total_space;
	info->free_bytes  = statfsEntry.avail_space;
	info->avail_bytes = statfsEntry.avail_space;

	info->total_files = MAX_REGULAR_INODE;
	info->free_files  = MAX_REGULAR_INODE - statfsEntry.inodes;
	info->avail_files = MAX_REGULAR_INODE - statfsEntry.inodes;

	info->time_delta.tv_sec  = 0;
	info->time_delta.tv_nsec = FSAL_DEFAULT_TIME_DELTA_NSEC;

	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/**
 * @brief Allocate a state_t structure.
 *
 * Note that this is not expected to fail since memory allocation is
 * expected to abort on failure.
 *
 * @param [in] export           Export state_t will be associated with
 * @param [in] stateType        Type of state to allocate
 * @param [in] relatedState     Related state if appropriate
 *
 * @returns: a state structure.
 */
struct state_t *allocate_state(struct fsal_export *export,
                               enum state_type stateType,
                               struct state_t *relatedState) {
	struct state_t *state = NULL;
	struct FSFileDescriptor *fileDescriptor = NULL;

	state = init_state(gsh_calloc(1, sizeof(struct FSFileDescriptorState)),
	                   export, stateType, relatedState);

	fileDescriptor = &container_of(state, struct FSFileDescriptorState,
	                               state)->fileDescriptor;

	fileDescriptor->fileDescriptor = NULL;
	fileDescriptor->openFlags = FSAL_O_CLOSED;
	return state;
}

/**
 * @brief Free a state_t structure.
 *
 * @param[in] export     Export state_t is associated with
 * @param[in] state      state_t structure to free
 *
 * @returns: NULL on failure otherwise a state structure.
 */
void free_state(struct fsal_export *export, struct state_t *state) {
	(void) export;

	struct FSFileDescriptorState *fdState = NULL;
	fdState = container_of(state, struct FSFileDescriptorState, state);
	gsh_free(fdState);
}

/**
 * @brief Convert a wire handle to a host handle.
 *
 * This function extracts a host handle from a wire handle. That is, when
 * given a handle as passed to a client, this method will extract the handle
 * to create objects.
 *
 * @param[in]     export             Export handle.
 * @param[in]     protocol           Protocol through which buffer was received.
 * @param[in]     flags              Flags to describe the wire handle. Example,
 *                                   if the handle is a big endian handle.
 * @param[in,out] bufferDescriptor   Buffer descriptor. The address of the buffer is
 *                                   given in bufferDescriptor->buf and must not be changed.
 *                                   bufferDescriptor->len is the length of the data contained
 *                                   in the buffer, bufferDescriptor->len must be updated to
 *                                   the correct host handle size.
 *
 * @returns: FSAL type.
 */
static fsal_status_t wire_to_host(struct fsal_export *export,
                                  fsal_digesttype_t protocol,
                                  struct gsh_buffdesc *bufferDescriptor,
                                  int flags) {
	(void) protocol;
	(void) export;

	if (!bufferDescriptor || !bufferDescriptor->addr) {
		return fsalstat(ERR_FSAL_FAULT, 0);
	}

	liz_inode_t *inode = (liz_inode_t *)bufferDescriptor->addr;

	if (flags & FH_FSAL_BIG_ENDIAN) {
#if (BYTE_ORDER != BIG_ENDIAN)
		static_assert(sizeof(liz_inode_t) == 4, "");
		*inode = bswap_32(*inode);
#endif
	}
	else {
#if (BYTE_ORDER == BIG_ENDIAN)
		assert(sizeof(liz_inode_t) == 4);
		*inode = bswap_32(*inode);
#endif
	}

	if (bufferDescriptor->len != sizeof(liz_inode_t)) {
		LogMajor(COMPONENT_FSAL,
		         "Size mismatch for handle. Should be %zu, got %zu",
		         sizeof(liz_inode_t), bufferDescriptor->len);
		return fsalstat(ERR_FSAL_SERVERFAULT, 0);
	}

	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/**
 * @brief Create a FSAL object handle from a host handle.
 *
 * This function creates a FSAL object handle from a host handle
 * (when an object is no longer in cache but the client still
 * remembers the handle).
 *
 * @param[in]     exportHandle         The export in which to create the handle.
 * @param[in]     bufferDescriptor     Buffer descriptor for the host handle.
 * @param[in]     publicHandle         FSAL object handle.
 * @param[in,out] attributes           Optional attributes for newly created object.
 *
 * \see fsal_api.h for more information
 *
 * @returns: FSAL status
 */
fsal_status_t create_handle(struct fsal_export *exportHandle,
                            struct gsh_buffdesc *bufferDescriptor,
                            struct fsal_obj_handle **publicHandle,
                            struct fsal_attrlist *attributes) {
	struct FSExport *export = NULL;
	struct FSHandle *handle = NULL;
	liz_inode_t *inode = NULL;

	export = container_of(exportHandle, struct FSExport, export);
	inode = (liz_inode_t *)bufferDescriptor->addr;

	*publicHandle = NULL;
	if (bufferDescriptor->len != sizeof(liz_inode_t)) {
		return fsalstat(ERR_FSAL_INVAL, 0);
	}

	liz_attr_reply_t result;
	int status = fs_getattr(export->fsInstance, &op_ctx->creds, *inode, &result);

	if (status < 0) {
		return fsalLastError();
	}

	handle = allocateNewHandle(&result.attr, export);

	if (attributes != NULL) {
		posix2fsal_attributes_all(&result.attr, attributes);
	}

	*publicHandle = &handle->fileHandle;
	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/**
 * @brief Get supported ACL types.
 *
 * This function returns a bitmask indicating whether it supports
 * ALLOW, DENY, neither, or both types of ACL.
 *
 * @param[in] export       Filesystem to interrogate.
 *
 * @returns: supported ACL types.
 */
static fsal_aclsupp_t fs_acl_support(struct fsal_export *export) {
	return fsal_acl_support(&export->fsal->fs_info);
}

/**
 * @brief Get supported attributes.
 *
 * This function returns a list of all attributes that this FSAL will support.
 * Be aware that this is specifically the attributes in struct fsal_attrlist,
 * other NFS attributes (fileid and so forth) are supported through other means.
 *
 * @param[in] export       Filesystem to interrogate.
 *
 * @returns: supported attributes.
 */
static attrmask_t fs_supported_attrs(struct fsal_export *export) {
	attrmask_t supported_mask = 0;
	supported_mask = fsal_supported_attrs(&export->fsal->fs_info);

	// Fixup supported_mask to indicate if ACL is actually supported for this export
	if (NFSv4_ACL_SUPPORT) {
		supported_mask |=  (attrmask_t)ATTR_ACL;
	}
	else {
		supported_mask &= ~(attrmask_t)ATTR_ACL;
	}
	return supported_mask;
}

/**
 * @brief Set operations for exports
 *
 * This function overrides operations that we've implemented, leaving
 * the rest for the default.
 *
 * @param[in,out] ops     Operations vector
 */
void initializeExportOperations(struct export_ops *ops) {
	ops->release = release;
	ops->lookup_path = lookup_path;
	ops->wire_to_host = wire_to_host;
	ops->create_handle = create_handle;
	ops->get_fs_dynamic_info = get_dynamic_info;
	ops->fs_supported_attrs = fs_supported_attrs;
	ops->fs_acl_support = fs_acl_support;
	ops->alloc_state = allocate_state;
	ops->free_state = free_state;
}
