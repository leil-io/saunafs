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

static void release(struct fsal_export *export_handle)
{
    struct lzfs_fsal_export *export;

    export = container_of(export_handle, struct lzfs_fsal_export, export);

    delete_handle(export->root);
    export->root = NULL;

    fsal_detach_export(export->export.fsal, &export->export.exports);
    free_export_ops(&export->export);

    if (export->fileinfo_cache) {
        liz_reset_fileinfo_cache_params(export->fileinfo_cache, 0, 0);

        while (1) {
            liz_fileinfo_entry_t *cache_handle;
            liz_fileinfo_t *file_handle;

            cache_handle = liz_fileinfo_cache_pop_expired(
                        export->fileinfo_cache);
            if (!cache_handle) {
                break;
            }

            file_handle = liz_extract_fileinfo(cache_handle);
            liz_release(export->lzfs_instance, file_handle);
            liz_fileinfo_entry_free(cache_handle);
        }

        liz_destroy_fileinfo_cache(export->fileinfo_cache);
        export->fileinfo_cache = NULL;
    }

    liz_destroy(export->lzfs_instance);
    export->lzfs_instance = NULL;
    gsh_free((char *)export->lzfs_params.subfolder);
    gsh_free(export);
}

// lookup_path
// modeled on old api except we don't stuff attributes.
fsal_status_t lookup_path(struct fsal_export *export_hdl,
                          const char *path, struct fsal_obj_handle **handle,
                          struct fsal_attrlist *attrs_out)
{
    static const char *root_dir_path = "/";

    struct lzfs_fsal_export *export;
    struct lzfs_fsal_handle *obj_handle = NULL;
    const char *real_path;
    int rc;

    LogFullDebug(COMPONENT_FSAL, "export_id=%" PRIu16 " path=%s",
                 export_hdl->export_id, path);

    export = container_of(export_hdl, struct lzfs_fsal_export, export);

    *handle = NULL;

    // set the real_path to the path without the prefix from
    // ctx_export->fullpath
    if (*path != '/') {
        real_path = strchr(path, ':');
        if (real_path == NULL) {
            return fsalstat(ERR_FSAL_INVAL, 0);
        }
        ++real_path;
        if (*real_path != '/') {
            return fsalstat(ERR_FSAL_INVAL, 0);
        }
    }
    else {
        real_path = path;
    }

    if (strstr(real_path, CTX_FULLPATH(op_ctx)) != real_path) {
        return fsalstat(ERR_FSAL_SERVERFAULT, 0);
    }

    real_path += strlen(CTX_FULLPATH(op_ctx));
    if (*real_path == '\0') {
        real_path = root_dir_path;
    }

    LogFullDebug(COMPONENT_FSAL, "real_path=%s", real_path);

    // special case the root
    if (strcmp(real_path, "/") == 0) {
        assert(export->root);
        *handle = &export->root->handle;
        if (attrs_out == NULL) {
            return fsalstat(ERR_FSAL_NO_ERROR, 0);
        }
    }

    liz_entry_t result;
    rc = liz_cred_lookup(export->lzfs_instance, &op_ctx->creds,
                         SPECIAL_INODE_ROOT, real_path, &result);

    if (rc < 0) {
        return lzfs_fsal_last_err();
    }

    if (attrs_out != NULL) {
        posix2fsal_attributes_all(&result.attr, attrs_out);
    }

    if (*handle == NULL) {
        obj_handle = allocate_new_handle(&result.attr, export);
        *handle = &obj_handle->handle;
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
static fsal_status_t get_dynamic_info(struct fsal_export *export_handle,
                                      struct fsal_obj_handle *obj_handle,
                                      fsal_dynamicfsinfo_t *info)
{
    //Unused variable
    (void ) obj_handle;

    struct lzfs_fsal_export *export;
    int rc;

    export = container_of(export_handle, struct lzfs_fsal_export, export);

    liz_stat_t st;

    rc = liz_statfs(export->lzfs_instance, &st);
    if (rc < 0) {
        return lzfs_fsal_last_err();
    }

    memset(info, 0, sizeof(fsal_dynamicfsinfo_t));
    info->total_bytes = st.total_space;
    info->free_bytes = st.avail_space;
    info->avail_bytes = st.avail_space;
    info->total_files = MAX_REGULAR_INODE;
    info->free_files = MAX_REGULAR_INODE - st.inodes;
    info->avail_files = MAX_REGULAR_INODE - st.inodes;
    info->time_delta.tv_sec = 0;
    info->time_delta.tv_nsec = FSAL_DEFAULT_TIME_DELTA_NSEC;

    return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

struct state_t *allocate_state(struct fsal_export *exp_hdl,
                               enum state_type state_type,
                               struct state_t *related_state)
{
    struct state_t *state;
    struct lzfs_fd *my_fd;

    state = init_state(gsh_calloc(1, sizeof(struct lzfs_state_fd)),
                       exp_hdl, state_type, related_state);

    my_fd = &container_of(state, struct lzfs_state_fd, state)->fd;

    my_fd->fd = NULL;
    my_fd->openflags = FSAL_O_CLOSED;
    return state;
}

/*! \brief Free a state_t structure
 *
 * @param[in] exp_hdl    Export state_t is associated with
 * @param[in] state      state_t structure to free
 *
 * \see fsal_api.h for more information
 *
 * @return  NULL on failure otherwise a state structure.
 *
 */
void free_state(struct fsal_export *exp_hdl, struct state_t *state)
{
    // Unused variable
    (void ) exp_hdl;

    struct lzfs_state_fd *state_fd;
    state_fd = container_of(state, struct lzfs_state_fd, state);
    gsh_free(state_fd);
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
 * @param[in]     exp_handle  Export handle
 * @param[in]     in_type     Protocol through which buffer was received.
 * @param[in]     flags       Flags to describe the wire handle. Example,
 *                            if the handle is a big endian handle.
 * @param[in,out] fh_desc     Buffer descriptor. The address of the buffer is
 *                            given in fh_desc->buf and must not be changed.
 *                            fh_desc->len is the length of the data contained
 *                            in the buffer, fh_desc->len must be updated to
 *                            the correct host handle size.
 *
 */
static fsal_status_t wire_to_host(struct fsal_export *exp_hdl,
                                  fsal_digesttype_t in_type,
                                  struct gsh_buffdesc *fh_desc,
                                  int flags)
{
    // Unused variables
    (void ) in_type;
    (void ) exp_hdl;

    liz_inode_t *inode;

    if (!fh_desc || !fh_desc->addr)
        return fsalstat(ERR_FSAL_FAULT, 0);

    inode = (liz_inode_t *)fh_desc->addr;
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

    if (fh_desc->len != sizeof(liz_inode_t)) {
        LogMajor(COMPONENT_FSAL,
                 "Size mismatch for handle. Should be %zu, got %zu",
                 sizeof(liz_inode_t), fh_desc->len);
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

fsal_status_t create_handle(struct fsal_export *exp_hdl,
                            struct gsh_buffdesc *desc,
                            struct fsal_obj_handle **pub_handle,
                            struct fsal_attrlist *attrs_out)
{
    struct lzfs_fsal_export *export;
    struct lzfs_fsal_handle *handle = NULL;
    liz_inode_t *inode;
    int rc;

    export = container_of(exp_hdl, struct lzfs_fsal_export, export);
    inode = (liz_inode_t *)desc->addr;

    *pub_handle = NULL;
    if (desc->len != sizeof(liz_inode_t)) {
        return fsalstat(ERR_FSAL_INVAL, 0);
    }

    liz_attr_reply_t result;
    rc = liz_cred_getattr(export->lzfs_instance, &op_ctx->creds,
                          *inode, &result);

    if (rc < 0) {
        return lzfs_fsal_last_err();
    }

    handle = allocate_new_handle(&result.attr, export);

    if (attrs_out != NULL) {
        posix2fsal_attributes_all(&result.attr, attrs_out);
    }

    *pub_handle = &handle->handle;
    return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/*! \brief Get supported ACL types
 *
 * \see fsal_api.h for more information
 */
static fsal_aclsupp_t fs_acl_support(struct fsal_export *exp_hdl)
{
    return fsal_acl_support(&exp_hdl->fsal->fs_info);
}

static attrmask_t fs_supported_attrs(struct fsal_export *exp_hdl)
{
    attrmask_t supported_mask;
    supported_mask = fsal_supported_attrs(&exp_hdl->fsal->fs_info);

    /* Fixup supported_mask to indicate if ACL is actually supported for
     * this export. */
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

void lzfs_export_ops_init(struct export_ops *ops)
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
