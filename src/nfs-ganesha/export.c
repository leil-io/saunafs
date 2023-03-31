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

#include "fsal.h"
#include <libgen.h>		/* used for 'dirname' */
#include <pthread.h>
#include <string.h>
#include <sys/types.h>
#include <sys/statvfs.h>
#include <os/mntent.h>
#include <os/quota.h>
#include <dlfcn.h>
#include "gsh_list.h"
#include "fsal_convert.h"
#include "config_parsing.h"
#include "FSAL/fsal_commonlib.h"
#include "FSAL/fsal_config.h"
#include "FSAL/fsal_localfs.h"
#include "fsal_handle_syscalls.h"
#include "nfs_exports.h"
#include "export_mgr.h"
#include "gsh_config.h"

#include "lzfs_fsal_types.h"
#include "lzfs_fsal_methods.h"
#include "context_wrap.h"

/* Flags to determine if ACLs are supported */
#define NFSv4_ACL_SUPPORT (!op_ctx_export_has_option(EXPORT_OPTION_DISABLE_ACL))

// export object methods
/**
 * @brief Finalize an export
 *
 * \see fsal_api.h for more information
 *
 * @param[in,out] export The export to be released
 */

static void release(struct fsal_export *exportHandle)
{
    struct FSExport *export;
    export = container_of(exportHandle, struct FSExport, export);

    deleteHandle(export->rootHandle);
    export->rootHandle = NULL;

    fsal_detach_export(export->export.fsal, &export->export.exports);
    free_export_ops(&export->export);

    if (export->fileinfoCache) {
        liz_reset_fileinfo_cache_params(export->fileinfoCache, 0, 0);

        while (1) {
            liz_fileinfo_entry_t *cache_handle;
            liz_fileinfo_t *file_handle;

            cache_handle = liz_fileinfo_cache_pop_expired(export->fileinfoCache);
            if (!cache_handle) {
                break;
            }

            file_handle = liz_extract_fileinfo(cache_handle);
            liz_release(export->fsInstance, file_handle);
            liz_fileinfo_entry_free(cache_handle);
        }

        liz_destroy_fileinfo_cache(export->fileinfoCache);
        export->fileinfoCache = NULL;
    }

    liz_destroy(export->fsInstance);
    export->fsInstance = NULL;

    gsh_free((char *)export->initialParameters.subfolder);
    gsh_free(export);
}

// lookup_path
// modeled on old api except we don't stuff attributes.
fsal_status_t lookup_path(struct fsal_export *exportHandle,
                          const char *path, struct fsal_obj_handle **handle,
                          struct fsal_attrlist *attributes)
{
    static const char *rootDirPath = "/";

    struct FSExport *export;
    struct FSHandle *objectHandle;
    const char *realPath;

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
    int rc = fs_lookup(export->fsInstance, &op_ctx->creds,
                       SPECIAL_INODE_ROOT, realPath, &entry);

    if (rc < 0) {
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
 * @brief Get dynamic filesystem statistics and configuration
 *
 * This function gets information on inodes and space in use and free for a
 * filesystem. See fsal_dynamicfsinfo_t for details of what to fill out.
 *
 * @param[in]  export_handle    Export handle to interrogate
 * @param[in]  obj_hdl          Directory
 * @param[out] info             Buffer to fill with information
 *
 * @return FSAL status.
 */
static fsal_status_t get_dynamic_info(struct fsal_export *exportHandle,
                                      struct fsal_obj_handle *objectHandle,
                                      fsal_dynamicfsinfo_t *info)
{
    //Unused variable
    (void ) objectHandle;

    struct FSExport *export;
    export = container_of(exportHandle, struct FSExport, export);

    liz_stat_t statfsEntry;
    int rc = liz_statfs(export->fsInstance, &statfsEntry);

    if (rc < 0) {
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

struct state_t *allocate_state(struct fsal_export *export,
                               enum state_type stateType,
                               struct state_t *relatedState)
{
    struct state_t *state;
    struct FSFileDescriptor *fileDescriptor;

    state = init_state(gsh_calloc(1, sizeof(struct FSFileDescriptorState)),
                       export, stateType, relatedState);

    fileDescriptor = &container_of(state, struct FSFileDescriptorState,
                                   state)->fileDescriptor;

    fileDescriptor->fileDescriptor = NULL;
    fileDescriptor->openFlags = FSAL_O_CLOSED;
    return state;
}

/*! \brief Free a state_t structure
 *
 * @param[in] export     Export state_t is associated with
 * @param[in] state      state_t structure to free
 *
 * @return  NULL on failure otherwise a state structure.
 */
void free_state(struct fsal_export *export, struct state_t *state)
{
    // Unused variable
    (void ) export;

    struct FSFileDescriptorState *fdState;
    fdState = container_of(state, struct FSFileDescriptorState, state);
    gsh_free(fdState);
}

/**
 * \brief Convert a wire handle to a host handle
 *
 * This function extracts a host handle from a wire handle. That is, when
 * given a handle as passed to a client, this method will extract the handle
 * to create objects.
 *
 * \see fsal_api.h for more information
 *
 * @param[in]     export             Export handle
 * @param[in]     protocol           Protocol through which buffer was received.
 * @param[in]     flags              Flags to describe the wire handle. Example,
 *                                   if the handle is a big endian handle.
 * @param[in,out] bufferDescriptor   Buffer descriptor. The address of the buffer is
 *                                   given in bufferDescriptor->buf and must not be changed.
 *                                   bufferDescriptor->len is the length of the data contained
 *                                   in the buffer, bufferDescriptor->len must be updated to
 *                                   the correct host handle size.
 *
 */
static fsal_status_t wire_to_host(struct fsal_export *export,
                                  fsal_digesttype_t protocol,
                                  struct gsh_buffdesc *bufferDescriptor,
                                  int flags)
{
    // Unused variables
    (void ) protocol;
    (void ) export;

    if (!bufferDescriptor || !bufferDescriptor->addr)
        return fsalstat(ERR_FSAL_FAULT, 0);

    liz_inode_t *inode = (liz_inode_t *)bufferDescriptor->addr;

    if (flags & FH_FSAL_BIG_ENDIAN) {
#if (BYTE_ORDER != BIG_ENDIAN)
        assert(sizeof(liz_inode_t) == 4);
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

/* create_handle
 * Does what original FSAL_ExpandHandle did (sort of)
 * returns a ref counted handle to be later used in mdcache etc.
 * NOTE! you must release this thing when done with it!
 * BEWARE! Thanks to some holes in the *AT syscalls implementation,
 * we cannot get an fd on an AF_UNIX socket, nor reliably on block or
 * character special devices.  Sorry, it just doesn't...
 * we could if we had the handle of the dir it is in, but this method
 * is for getting handles off the wire for cache entries that have LRU'd.
 * Ideas and/or clever hacks are welcome...
 */

fsal_status_t create_handle(struct fsal_export *exportHandle,
                            struct gsh_buffdesc *bufferDescriptor,
                            struct fsal_obj_handle **publicHandle,
                            struct fsal_attrlist *attributes)
{
    struct FSExport *export;
    struct FSHandle *handle = NULL;
    liz_inode_t *inode;
    int rc;

    export = container_of(exportHandle, struct FSExport, export);
    inode = (liz_inode_t *)bufferDescriptor->addr;

    *publicHandle = NULL;
    if (bufferDescriptor->len != sizeof(liz_inode_t)) {
        return fsalstat(ERR_FSAL_INVAL, 0);
    }

    liz_attr_reply_t result;
    rc = fs_getattr(export->fsInstance, &op_ctx->creds, *inode, &result);

    if (rc < 0) {
        return fsalLastError();
    }

    handle = allocateNewHandle(&result.attr, export);

    if (attributes != NULL) {
        posix2fsal_attributes_all(&result.attr, attributes);
    }

    *publicHandle = &handle->fileHandle;
    return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/*! \brief Get supported ACL types
 *
 * \see fsal_api.h for more information
 */
static fsal_aclsupp_t fs_acl_support(struct fsal_export *export)
{
    return fsal_acl_support(&export->fsal->fs_info);
}

/*! \brief Get supported attributes
 *
 */
static attrmask_t fs_supported_attrs(struct fsal_export *export)
{
    attrmask_t supported_mask;
    supported_mask = fsal_supported_attrs(&export->fsal->fs_info);

    // Fixup supported_mask to indicate if ACL is actually supported for this export
    if (NFSv4_ACL_SUPPORT)
        supported_mask |=  ATTR_ACL;
    else
        supported_mask &= ~ATTR_ACL;
    return supported_mask;
}

/**
 * @brief Set operations for exports
 *
 * This function overrides operations that we've implemented, leaving
 * the rest for the default.
 *
 * @param[in,out] ops Operations vector
 */
void initializeExportOperations(struct export_ops *ops)
{
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
