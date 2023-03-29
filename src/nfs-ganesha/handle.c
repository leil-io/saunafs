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
#include "config.h"

#ifdef LINUX
#include <sys/sysmacros.h> /* for makedev(3) */
#endif
#include <libgen.h>		/* used for 'dirname' */
#include <pthread.h>
#include <string.h>
#include <sys/types.h>
#include "gsh_list.h"
#include "fsal.h"
#include "fsal_convert.h"
#include "fsal_handle_syscalls.h"
#include "FSAL/fsal_commonlib.h"
#include "FSAL/fsal_localfs.h"
#include <os/subr.h>
#include "city.h"
#include "nfs_core.h"
#include "nfs_proto_tools.h"

#include "lzfs_fsal_methods.h"
#include "context_wrap.h"
#include "common/lizardfs_error_codes.h"

/*! \brief Clean up a filehandle
 *
 * \see fsal_api.h for more information
 */
static void _release(struct fsal_obj_handle *objectHandle)
{
    struct FSHandle *handle;
    handle = container_of(objectHandle, struct FSHandle, fileHandle);

    if (handle != handle->export->rootHandle) {
        deleteHandle(handle);
    }
}

/*! \brief Look up a filename
 *
 * \see fsal_api.h for more information
 */
static fsal_status_t _lookup(struct fsal_obj_handle *dirHandle,
                             const char *path,
                             struct fsal_obj_handle **objectHandle,
                             struct fsal_attrlist *attributes)
{
    struct FSExport *export;
    struct FSHandle *directory;
    struct liz_entry node;

    export = container_of(op_ctx->fsal_export, struct FSExport, export);
    directory = container_of(dirHandle, struct FSHandle, fileHandle);

    int rc = fs_lookup(export->fsInstance, &op_ctx->creds,
                       directory->inode, path, &node);

    if (rc < 0) {
        return fsalLastError();
    }

    if (attributes != NULL) {
        posix2fsal_attributes_all(&node.attr, attributes);
    }

    struct FSHandle *handle = allocateNewHandle(&node.attr, export);
    *objectHandle = &handle->fileHandle;

    return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/*! \brief Read a directory
 *
 * \see fsal_api.h for more information
 */
static fsal_status_t _readdir(struct fsal_obj_handle *dirHandle,
                              fsal_cookie_t *whence, void *dirState,
                              fsal_readdir_cb readDirCb,
                              attrmask_t attributesMask, bool *eof)
{
    static const int batchSize = 100;
    struct liz_direntry buffer[batchSize];

    struct FSExport *export;
    struct FSHandle *directory, *handle;

    struct fsal_attrlist attributes;
    off_t direntryOffset = 0;
    enum fsal_dir_result result;
    int rc;

    export = container_of(op_ctx->fsal_export, struct FSExport, export);
    directory = container_of(dirHandle, struct FSHandle, fileHandle);

    liz_context_t *context;
    context = fsal_create_context(export->fsInstance, &op_ctx->creds);

    struct liz_fileinfo *fileDescriptor;
    fileDescriptor = liz_opendir(export->fsInstance, context, directory->inode);

    if (!fileDescriptor) {
        liz_destroy_context(context);
        return fsalLastError();
    }

    if (whence != NULL) {
        direntryOffset = *whence;
    }

    while (1) {
        size_t i, entries = 0;

        rc = liz_readdir(export->fsInstance, context, fileDescriptor,
                         direntryOffset, batchSize, buffer, &entries);
        if (rc < 0) {
            liz_destroy_context(context);
            return fsalLastError();
        }

        result = DIR_CONTINUE;
        for (i = 0; i < entries && result != DIR_TERMINATE; ++i) {
            if (strcmp(buffer[i].name, ".")  == 0 ||
                strcmp(buffer[i].name, "..") == 0)
                continue;

            handle = allocateNewHandle(&buffer[i].attr, export);

            fsal_prepare_attrs(&attributes, attributesMask);
            posix2fsal_attributes_all(&buffer[i].attr, &attributes);

            direntryOffset = buffer[i].next_entry_offset;
            result = readDirCb(buffer[i].name, &handle->fileHandle,
                               &attributes, dirState, direntryOffset + 1);

            fsal_release_attrs(&attributes);
        }

        liz_destroy_direntry(buffer, entries);
        *eof = (entries < batchSize) && (i == entries);

        if (result != DIR_CONTINUE || entries < batchSize) {
            break;
        }
    }

    rc = liz_releasedir(export->fsInstance, fileDescriptor);
    liz_destroy_context(context);

    if (rc < 0) {
        return fsalLastError();
    }

    return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/*! \brief Get attributes
 *
 * \see fsal_api.h for more information
 */
static fsal_status_t _getattrs(struct fsal_obj_handle *obj_hdl,
                               struct fsal_attrlist *attrs)
{
    struct FSExport *export;
    struct FSHandle *handle;
    struct liz_attr_reply lzfs_attrs;

    export = container_of(op_ctx->fsal_export, struct FSExport, export);
    handle = container_of(obj_hdl, struct FSHandle, fileHandle);

    LogFullDebug(COMPONENT_FSAL, "ALLI: export = %" PRIu16 " inode = %" PRIu32,
                 export->export.export_id, handle->inode);

    int rc = fs_getattr(export->fsInstance, &op_ctx->creds,
                        handle->inode, &lzfs_attrs);

    if (rc < 0) {
        if (attrs->request_mask & ATTR_RDATTR_ERR) {
            attrs->valid_mask = ATTR_RDATTR_ERR;
        }
        return fsalLastError();
    }

    posix2fsal_attributes_all(&lzfs_attrs.attr, attrs);
#ifdef ENABLE_NFS_ACL_SUPPORT
    if (attrs->request_mask & ATTR_ACL) {
        fsal_status_t status;
        status = getACL(export, handle->inode,
                                 lzfs_attrs.attr.st_uid, &attrs->acl);

        if (!FSAL_IS_ERROR(status)) {
            attrs->valid_mask |= ATTR_ACL;
        }
    }
#endif

    return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/*! \brief Write wire handle
 *
 * \see fsal_api.h for more information
 */
static fsal_status_t _handle_to_wire(
        const struct fsal_obj_handle *obj_hdl,
        uint32_t output_type, struct gsh_buffdesc *fh_desc)
{
    // Unused variable
    (void ) output_type;

    struct FSHandle *handle;
    handle = container_of(obj_hdl, struct FSHandle, fileHandle);

    liz_inode_t inode = handle->inode;
    if (fh_desc->len < sizeof(liz_inode_t)) {
        LogMajor(COMPONENT_FSAL,
                 "Space too small for handle. Need  %zu, have %zu",
                 sizeof(liz_inode_t), fh_desc->len);
        return fsalstat(ERR_FSAL_TOOSMALL, 0);
    }

    memcpy(fh_desc->addr, &inode, sizeof(liz_inode_t));
    fh_desc->len = sizeof(inode);

    return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/*! \brief Get key for handle
 *
 * \see fsal_api.h for more information
 */
static void _handle_to_key(struct fsal_obj_handle *obj_hdl,
                           struct gsh_buffdesc *fh_desc)
{
    struct FSHandle *handle;
    handle = container_of(obj_hdl, struct FSHandle, fileHandle);

    fh_desc->addr = &handle->uniqueKey;
    fh_desc->len = sizeof(struct FSALKey);
}

static fsal_status_t open_fd(struct FSHandle *handle,
                             fsal_openflags_t openflags,
                             struct FSFileDescriptor *fd,
                             bool no_access_check)
{
    struct FSExport *export;
    int posix_flags;

    fsal2posix_openflags(openflags, &posix_flags);
    if (no_access_check) {
        posix_flags |= O_CREAT;
    }

    export = container_of(op_ctx->fsal_export,
                          struct FSExport, export);

    LogFullDebug(COMPONENT_FSAL,
                 "fd = %p fd->fd = %p openflags = %x, posix_flags = %x",
                 fd, fd->fileDescriptor, openflags, posix_flags);

    assert(fd->fileDescriptor == NULL &&
           fd->openFlags == FSAL_O_CLOSED &&
           openflags != 0);

    fd->fileDescriptor = fs_open(export->fsInstance, &op_ctx->creds,
                                 handle->inode, posix_flags);

    if (!fd->fileDescriptor) {
        LogFullDebug(COMPONENT_FSAL, "open failed with %s",
                     liz_error_string(liz_last_err()));
        return fsalLastError();
    }

    fd->openFlags = openflags;
    return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

static fsal_status_t close_fd(struct FSHandle *handle,
                              struct FSFileDescriptor *fd)
{
    if (fd->fileDescriptor != NULL && fd->openFlags != FSAL_O_CLOSED) {
        int rc = liz_release(handle->export->fsInstance, fd->fileDescriptor);

        fd->fileDescriptor = NULL;
        fd->openFlags = FSAL_O_CLOSED;
        if (rc < 0) {
            return fsalLastError();
        }
    }

    return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

static fsal_status_t open_by_handle(struct fsal_obj_handle *obj_hdl,
                                    struct state_t *state,
                                    fsal_openflags_t openflags,
                                    enum fsal_create_mode createmode,
                                    fsal_verifier_t verifier,
                                    struct fsal_attrlist *attrs_out,
                                    bool *caller_perm_check,
                                    bool after_mknod)
{
    struct FSExport *export;
    struct FSHandle *handle;
    struct FSFileDescriptor *fd;
    fsal_status_t status = fsalstat(ERR_FSAL_NO_ERROR, 0);
    int posix_flags;

    handle = container_of(obj_hdl, struct FSHandle, fileHandle);
    export = container_of(op_ctx->fsal_export,
                          struct FSExport, export);

    PTHREAD_RWLOCK_wrlock(&obj_hdl->obj_lock);

    if (state != NULL) {
        fd = &container_of(state, struct FSFileDescriptorState, state)->fileDescriptor;
        status = check_share_conflict(&handle->share, openflags, false);

        if (FSAL_IS_ERROR(status)) {
            PTHREAD_RWLOCK_unlock(&obj_hdl->obj_lock);
            return status;
        }

        update_share_counters(&handle->share, FSAL_O_CLOSED, openflags);
        PTHREAD_RWLOCK_unlock(&obj_hdl->obj_lock);
    }
    else {
        fd = &handle->fileDescriptor;
    }

    status = open_fd(handle, openflags, fd, after_mknod);
    if (FSAL_IS_ERROR(status)) {
        if (state != NULL) {
            goto undo_share;
        }

        PTHREAD_RWLOCK_unlock(&obj_hdl->obj_lock);
        return status;
    }

    fsal2posix_openflags(openflags, &posix_flags);
    bool truncated = (posix_flags & O_TRUNC) != 0;

    if (createmode >= FSAL_EXCLUSIVE || truncated || attrs_out) {
        struct liz_attr_reply lzfs_attrs;
        int rc = fs_getattr(export->fsInstance, &op_ctx->creds,
                                  handle->inode, &lzfs_attrs);

        if (rc == 0) {
            LogFullDebug(COMPONENT_FSAL, "New size = %" PRIx64,
                         (int64_t)lzfs_attrs.attr.st_size);
        }
        else {
            status = fsalLastError();
        }

        if (!FSAL_IS_ERROR(status) &&
            createmode >= FSAL_EXCLUSIVE &&
            createmode != FSAL_EXCLUSIVE_9P &&
            !check_verifier_stat(&lzfs_attrs.attr, verifier, false)) {
            // Verifier didn't match, return EEXIST
            status = fsalstat(posix2fsal_error(EEXIST), EEXIST);
        }

        if (!FSAL_IS_ERROR(status) && attrs_out) {
            posix2fsal_attributes_all(&lzfs_attrs.attr, attrs_out);
        }
    }

    if (state == NULL) {
        PTHREAD_RWLOCK_unlock(&obj_hdl->obj_lock);
        // If success, we haven't done any permission check so ask the
        // caller to do so.
        *caller_perm_check = !FSAL_IS_ERROR(status);
        return status;
    }

    if (!FSAL_IS_ERROR(status)) {
        // Return success. We haven't done any permission check so ask
        // the caller to do so.
        *caller_perm_check = true;
        return status;
    }

    close_fd(handle, fd);

undo_share:
    // On error we need to release our share reservation
    // and undo the update of the share counters.
    PTHREAD_RWLOCK_wrlock(&obj_hdl->obj_lock);
    update_share_counters(&handle->share, openflags, FSAL_O_CLOSED);
    PTHREAD_RWLOCK_unlock(&obj_hdl->obj_lock);
    return status;
}

static fsal_status_t open_by_name(struct fsal_obj_handle *obj_hdl,
                                  struct state_t *state,
                                  fsal_openflags_t openflags,
                                  const char *name,
                                  fsal_verifier_t verifier,
                                  struct fsal_attrlist *attrs_out,
                                  bool *caller_perm_check)
{
    struct fsal_obj_handle *temp = NULL;
    fsal_status_t status;
    status = obj_hdl->obj_ops->lookup(obj_hdl, name, &temp, NULL);

    if (FSAL_IS_ERROR(status)) {
        LogFullDebug(COMPONENT_FSAL, "lookup returned %s",
                     fsal_err_txt(status));
        return status;
    }

    status = open_by_handle(temp, state, openflags, FSAL_NO_CREATE, verifier,
                            attrs_out, caller_perm_check, false);

    if (FSAL_IS_ERROR(status)) {
        temp->obj_ops->release(temp);
        LogFullDebug(COMPONENT_FSAL, "open returned %s", fsal_err_txt(status));
    }

    return status;
}

/*! \brief Open a file descriptor for read or write and possibly create
 *
 * \see fsal_api.h for more information
 */
static fsal_status_t _open2(struct fsal_obj_handle *obj_hdl,
                                struct state_t *state,
                                fsal_openflags_t openflags,
                                enum fsal_create_mode createmode,
                                const char *name,
                                struct fsal_attrlist *attr_set,
                                fsal_verifier_t verifier,
                                struct fsal_obj_handle **new_obj,
                                struct fsal_attrlist *attrs_out,
                                bool *caller_perm_check)
{
    struct FSExport *export;
    struct FSHandle *handle;
    fsal_status_t status;
    int rc;

    LogAttrlist(COMPONENT_FSAL, NIV_FULL_DEBUG, "attrs ", attr_set, false);

    if (createmode >= FSAL_EXCLUSIVE) {
        set_common_verifier(attr_set, verifier, false);
    }

    if (name == NULL) {
        return open_by_handle(obj_hdl, state, openflags, createmode, verifier,
                              attrs_out, caller_perm_check, false);
    }

    if (createmode == FSAL_NO_CREATE) {
        return open_by_name(obj_hdl, state, openflags, name, verifier,
                            attrs_out, caller_perm_check);
    }

    // Create file
    export = container_of(op_ctx->fsal_export,
                          struct FSExport, export);
    handle = container_of(obj_hdl, struct FSHandle, fileHandle);

    mode_t unix_mode = fsal2unix_mode(attr_set->mode) &
        ~op_ctx->fsal_export->exp_ops.fs_umask(op_ctx->fsal_export);

    FSAL_UNSET_MASK(attr_set->valid_mask, ATTR_MODE);

    struct liz_entry lzfs_attrs;
    rc = fs_mknode(export->fsInstance, &op_ctx->creds,
                        handle->inode, name, unix_mode, 0, &lzfs_attrs);

    if (rc < 0 && liz_last_err() == LIZARDFS_ERROR_EEXIST &&
            createmode == FSAL_UNCHECKED) {
        return open_by_name(obj_hdl, state, openflags, name, verifier,
                            attrs_out, caller_perm_check);
    }

    if (rc < 0) {
        return fsalLastError();
    }

    // File has been created by us.
    *caller_perm_check = false;
    struct FSHandle *new_handle;
    new_handle = allocateNewHandle(&lzfs_attrs.attr, export);

    if (new_handle == NULL) {
        status = fsalstat(posix2fsal_error(ENOMEM), ENOMEM);
        goto fileerr;
    }

    *new_obj = &new_handle->fileHandle;

    if (attr_set->valid_mask != 0) {
        status = (*new_obj)->obj_ops->setattr2(*new_obj, false,
                                               state, attr_set);
        if (FSAL_IS_ERROR(status)) {
            goto fileerr;
        }

        if (attrs_out != NULL) {
            status = (*new_obj)->obj_ops->getattrs(*new_obj, attrs_out);
            if (FSAL_IS_ERROR(status) &&
                    (attrs_out->request_mask & ATTR_RDATTR_ERR) == 0) {
                goto fileerr;
            }

            attrs_out = NULL;
        }
    }

    if (attrs_out != NULL) {
        posix2fsal_attributes_all(&lzfs_attrs.attr, attrs_out);
    }

    return open_by_handle(*new_obj, state, openflags, createmode,
                          verifier, NULL, caller_perm_check, true);

fileerr:
    (*new_obj)->obj_ops->release(*new_obj);
    *new_obj = NULL;

    rc = fs_unlink(export->fsInstance, &op_ctx->creds,
                         handle->inode, name);

    if (rc < 0) {
        return fsalLastError();
    }

    return status;
}

static fsal_status_t open_func(struct fsal_obj_handle *obj_hdl,
                               fsal_openflags_t openflags,
                               struct fsal_fd *fd)
{
    struct FSHandle *handle;
    handle = container_of(obj_hdl, struct FSHandle, fileHandle);
    return open_fd(handle, openflags, (struct FSFileDescriptor *)fd, true);
}

static fsal_status_t close_func(struct fsal_obj_handle *obj_hdl,
                                struct fsal_fd *fd)
{
    struct FSHandle *handle;
    handle = container_of(obj_hdl, struct FSHandle, fileHandle);
    return close_fd(handle, (struct FSFileDescriptor *)fd);
}

static fsal_status_t find_fd(struct FSFileDescriptor *fd,
                             struct fsal_obj_handle *obj_hdl,
                             bool bypass, struct state_t *state,
                             fsal_openflags_t openflags,
                             bool *has_lock, bool *closefd,
                             bool open_for_locks)
{
    struct FSHandle *handle;
    handle = container_of(obj_hdl, struct FSHandle, fileHandle);

    struct FSFileDescriptor temp_fd = {0, NULL}, *out_fd = &temp_fd;
    fsal_status_t status;

    bool reusing_open_state_fd = false;
    status = fsal_find_fd((struct fsal_fd **)&out_fd, obj_hdl,
                          (struct fsal_fd *)&handle->fileDescriptor,
                          &handle->share, bypass, state, openflags,
                          open_func, close_func, has_lock, closefd,
                          open_for_locks, &reusing_open_state_fd);

    *fd = *out_fd;
    return status;
}

/**
 * \brief Read data from a file
 *
 * \see fsal_api.h for more information
 */
static void _read2(struct fsal_obj_handle *obj_hdl,
                       bool bypass, fsal_async_cb done_cb,
                       struct fsal_io_arg *read_arg, void *caller_arg)
{
    struct FSExport *export;
    struct FSHandle *handle;
    struct FSFileDescriptor fd;
    fsal_status_t status;

    bool has_lock = false;
    bool closefd = false;
    ssize_t nb_read;
    uint64_t offset = read_arg->offset;

    export = container_of(op_ctx->fsal_export,
                          struct FSExport, export);
    handle = container_of(obj_hdl, struct FSHandle, fileHandle);

    LogFullDebug(COMPONENT_FSAL, "export = %" PRIu16 " inode = %" PRIu32
                 " offset=%" PRIu64, export->export.export_id,
                 handle->inode, offset);

    if (read_arg->info != NULL) {
        /* Currently we don't support READ_PLUS */
        done_cb(obj_hdl, fsalstat(ERR_FSAL_NOTSUPP, 0), read_arg, caller_arg);
        return;
    }

    status = find_fd(&fd, obj_hdl, bypass, read_arg->state,
                     FSAL_O_READ, &has_lock, &closefd, false);

    if (FSAL_IS_ERROR(status)) {
        goto out;
    }

    read_arg->io_amount = 0;
    for (int i = 0; i < read_arg->iov_count; i++) {
        nb_read = fs_read(export->fsInstance,
                                &op_ctx->creds, fd.fileDescriptor, offset,
                                read_arg->iov[i].iov_len,
                                read_arg->iov[i].iov_base);

        if (nb_read < 0) {
            status = fsalLastError();
            goto out;
        }
        else if (nb_read == 0) {
            read_arg->end_of_file = true;
            break;
        }

        read_arg->io_amount += nb_read;
        offset += nb_read;
    }

    read_arg->end_of_file = (read_arg->io_amount == 0);

out:
    if (closefd) {
        close_fd(handle, &fd);
    }

    if (has_lock) {
        PTHREAD_RWLOCK_unlock(&obj_hdl->obj_lock);
    }

    done_cb(obj_hdl, status, read_arg, caller_arg);
}

/*! \brief Create a directory
 *
 * \see fsal_api.h for more information
 */
static fsal_status_t _mkdir(struct fsal_obj_handle *dir_hdl,
                                const char *name,
                                struct fsal_attrlist *attrib,
                                struct fsal_obj_handle **new_obj,
                                struct fsal_attrlist *attrs_out)
{
    struct FSExport *export;
    struct FSHandle *directory, *handle;
    struct liz_entry dir_entry;
    fsal_status_t status;

    export = container_of(op_ctx->fsal_export,
                          struct FSExport, export);
    directory = container_of(dir_hdl, struct FSHandle, fileHandle);

    LogFullDebug(COMPONENT_FSAL, "export=%" PRIu16 " parent_inode=%"
                 PRIu32 " mode=%" PRIo32 " name=%s", export->export.export_id,
                 directory->inode, attrib->mode, name);

    mode_t unix_mode = fsal2unix_mode(attrib->mode) &
            ~op_ctx->fsal_export->exp_ops.fs_umask(op_ctx->fsal_export);

    int rc = fs_mkdir(export->fsInstance, &op_ctx->creds,
                            directory->inode, name, unix_mode, &dir_entry);

    if (rc < 0) {
        return fsalLastError();
    }

    handle = allocateNewHandle(&dir_entry.attr, export);
    *new_obj = &handle->fileHandle;

    FSAL_UNSET_MASK(attrib->valid_mask, ATTR_MODE);

    if (attrib->valid_mask) {
        status = (*new_obj)->obj_ops->setattr2(*new_obj, false, NULL,
                                               attrib);
        if (FSAL_IS_ERROR(status)) {
            LogFullDebug(COMPONENT_FSAL, "setattr2 status=%s",
                         fsal_err_txt(status));
            // Release the handle we just allocate
            (*new_obj)->obj_ops->release(*new_obj);
            *new_obj = NULL;
        }
        else if (attrs_out != NULL) {
            /*
             * We ignore errors here. The mkdir and setattr
             * succeeded, so we don't want to return error if the
             * getattrs fails. We'll just return no attributes
             * in that case.*/
            (*new_obj)->obj_ops->getattrs(*new_obj, attrs_out);
        }
    }
    else if (attrs_out != NULL) {
        /* Since we haven't set any attributes other than what
         * was set on create, just use the stat results we used
         * to create the fsal_obj_handle.*/
        posix2fsal_attributes_all(&dir_entry.attr, attrs_out);
    }

    FSAL_SET_MASK(attrib->valid_mask, ATTR_MODE);
    return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/*! \brief Create a new link
 *
 * \see fsal_api.h for more information
 */
static fsal_status_t _link(struct fsal_obj_handle *obj_hdl,
                               struct fsal_obj_handle *destdir_hdl,
                               const char *name)
{
    struct FSExport *export;
    struct FSHandle *handle, *dest_directory;

    export = container_of(op_ctx->fsal_export,
                          struct FSExport, export);
    handle = container_of(obj_hdl,
                          struct FSHandle, fileHandle);
    dest_directory = container_of(destdir_hdl,
                                  struct FSHandle, fileHandle);

    LogFullDebug(COMPONENT_FSAL, "export = %"
                 PRIu16 " inode = %" PRIu32 " dest_inode = %" PRIu32
                 " name = %s", export->export.export_id,
                 handle->inode, dest_directory->inode, name);

    liz_entry_t result;
    int rc = fs_link(export->fsInstance, &op_ctx->creds,
                           handle->inode, dest_directory->inode, name,
                           &result);
    if (rc < 0) {
        return fsalLastError();
    }

    return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/*! \brief Rename a file
 *
 * \see fsal_api.h for more information
 */
static fsal_status_t _rename(struct fsal_obj_handle *obj_hdl,
                                 struct fsal_obj_handle *olddir_hdl,
                                 const char *old_name,
                                 struct fsal_obj_handle *newdir_hdl,
                                 const char *new_name)
{
    struct FSExport *export;
    struct FSHandle *olddir, *newdir;

    export = container_of(op_ctx->fsal_export,
                          struct FSExport, export);
    olddir = container_of(olddir_hdl,
                          struct FSHandle, fileHandle);
    newdir = container_of(newdir_hdl,
                          struct FSHandle, fileHandle);

    LogFullDebug(COMPONENT_FSAL, "export=%" PRIu16 " old_inode=%" PRIu32
                 " new_inode=%" PRIu32 " old_name=%s new_name=%s",
                 export->export.export_id, olddir->inode,
                 newdir->inode, old_name, new_name);

    int rc = fs_rename(export->fsInstance, &op_ctx->creds,
                             olddir->inode, old_name, newdir->inode,
                             new_name);

    if (rc < 0) {
        return fsalLastError();
    }

    return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/*! \brief Remove a name from a directory
 *
 * \see fsal_api.h for more information
 */
static fsal_status_t _unlink(struct fsal_obj_handle *dir_hdl,
                                 struct fsal_obj_handle *obj_hdl,
                                 const char *name)
{
    struct FSExport *export;
    struct FSHandle *directory;
    int rc;

    export = container_of(op_ctx->fsal_export,
                          struct FSExport, export);
    directory = container_of(dir_hdl,
                             struct FSHandle, fileHandle);

    LogFullDebug(COMPONENT_FSAL, "export = %" PRIu16 " parent_inode = %"
                 PRIu32 " name = %s type = %s", export->export.export_id,
                 directory->inode, name,
                 object_file_type_to_str(obj_hdl->type));

    if (obj_hdl->type != DIRECTORY) {
        rc = fs_unlink(export->fsInstance, &op_ctx->creds,
                             directory->inode, name);
    }
    else {
        rc = fs_rmdir(export->fsInstance, &op_ctx->creds,
                            directory->inode, name);
    }

    if (rc < 0) {
        return fsalLastError();
    }

    return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/*! \brief Close a file
 *
 * \see fsal_api.h for more information
 */
static fsal_status_t _close(struct fsal_obj_handle *obj_hdl)
{
    fsal_status_t status;
    struct FSHandle *handle;
    handle = container_of(obj_hdl, struct FSHandle, fileHandle);

    LogFullDebug(COMPONENT_FSAL, "export=%" PRIu16 " inode=%" PRIu32,
                 handle->uniqueKey.exportId, handle->inode);

    PTHREAD_RWLOCK_wrlock(&obj_hdl->obj_lock);
    if (handle->fileDescriptor.openFlags == FSAL_O_CLOSED)
        status = fsalstat(ERR_FSAL_NOT_OPENED, 0);
    else
        status = close_fd(handle, &handle->fileDescriptor);
    PTHREAD_RWLOCK_unlock(&obj_hdl->obj_lock);
    return status;
}

/*! \brief Write data to a file
 *
 * \see fsal_api.h for more information
 */
static void _write2(struct fsal_obj_handle *obj_hdl,
                        bool bypass, fsal_async_cb done_cb,
                        struct fsal_io_arg *write_arg,
                        void *caller_arg)
{
    struct FSExport *export;
    struct FSHandle *handle;
    struct FSFileDescriptor file_descriptor;
    fsal_status_t status;
    bool has_lock = false;
    bool closefd = false;
    ssize_t nb_written;
    uint64_t offset = write_arg->offset;

    export = container_of(op_ctx->fsal_export,
                          struct FSExport, export);

    handle = container_of(obj_hdl,
                          struct FSHandle, fileHandle);

    LogFullDebug(COMPONENT_FSAL, "export=%" PRIu16 " inode=%" PRIu32
                 " offset=%" PRIu64, export->export.export_id,
                 handle->inode, offset);

    if (write_arg->info) {
        return done_cb(obj_hdl, fsalstat(ERR_FSAL_NOTSUPP, 0),
                       write_arg, caller_arg);
    }

    status = find_fd(&file_descriptor, obj_hdl, bypass, write_arg->state,
                     FSAL_O_WRITE, &has_lock, &closefd, false);

    if (FSAL_IS_ERROR(status)) {
        goto out;
    }

    for (int i = 0; i < write_arg->iov_count; i++) {
        nb_written = fs_write(export->fsInstance, &op_ctx->creds,
                                    file_descriptor.fileDescriptor, offset,
                                    write_arg->iov[i].iov_len,
                                    write_arg->iov[i].iov_base);

        if (nb_written < 0) {
            status = fsalLastError();
            goto out;
        }
        else {
            write_arg->io_amount = nb_written;
            if (write_arg->fsal_stable) {
                int rc = fs_fsync(export->fsInstance,
                                        &op_ctx->creds, file_descriptor.fileDescriptor);

                if (rc < 0) {
                    status = fsalLastError();
                }
            }
        }

        write_arg->io_amount += nb_written;
        offset += nb_written;
    }

out:
    if (closefd) {
        close_fd(handle, &file_descriptor);
    }

    if (has_lock) {
        PTHREAD_RWLOCK_unlock(&obj_hdl->obj_lock);
    }

    done_cb(obj_hdl, status, write_arg, caller_arg);
}

/*! \brief Commit written data
 *
 * \see fsal_api.h for more information
 */
static fsal_status_t _commit2(struct fsal_obj_handle *obj_hdl,
                                  off_t offset, size_t len)
{
    struct FSExport *export;
    struct FSHandle *handle;
    struct FSFileDescriptor temp_fd = {0, NULL}, *out_fd = &temp_fd;
    fsal_status_t status;
    bool has_lock = false;
    bool closefd = false;

    export = container_of(op_ctx->fsal_export,
                          struct FSExport, export);
    handle = container_of(obj_hdl,
                          struct FSHandle, fileHandle);

    LogFullDebug(COMPONENT_FSAL, "export = %" PRIu16 " inode = %" PRIu32
                 " offset = %lli len = %zu", export->export.export_id,
                 handle->inode, (long long)offset, len);

    status = fsal_reopen_obj(obj_hdl, false, false, FSAL_O_WRITE,
                             (struct fsal_fd *)&handle->fileDescriptor,
                             &handle->share, open_func, close_func,
                             (struct fsal_fd **)&out_fd, &has_lock,
                             &closefd);

    if (!FSAL_IS_ERROR(status)) {
        int rc = fs_fsync(export->fsInstance,
                                &op_ctx->creds, out_fd->fileDescriptor);

        if (rc < 0) {
            status = fsalLastError();
        }
    }

    if (closefd) {
        int rc = liz_release(export->fsInstance, out_fd->fileDescriptor);

        if (rc < 0) {
            status = fsalLastError();
        }
    }

    if (has_lock) {
        PTHREAD_RWLOCK_unlock(&obj_hdl->obj_lock);
    }

    return status;
}

/*! \brief Set attributes on an object
 *
 * \see fsal_api.h for more information
 */
static fsal_status_t _setattr2(struct fsal_obj_handle *obj_hdl,
                                   bool bypass, struct state_t *state,
                                   struct fsal_attrlist *attrib_set)
{
    struct FSExport *export;
    struct FSHandle *handle;
    bool has_lock = false;
    bool closefd = false;
    fsal_status_t status = fsalstat(ERR_FSAL_NO_ERROR, 0);

    export = container_of(op_ctx->fsal_export,
                          struct FSExport, export);
    handle = container_of(obj_hdl,
                          struct FSHandle, fileHandle);

    LogAttrlist(COMPONENT_FSAL, NIV_FULL_DEBUG, "attrs ", attrib_set, false);

    if (FSAL_TEST_MASK(attrib_set->valid_mask, ATTR_MODE)) {
        attrib_set->mode &=
          ~op_ctx->fsal_export->exp_ops.fs_umask(op_ctx->fsal_export);
    }

    if (FSAL_TEST_MASK(attrib_set->valid_mask, ATTR_SIZE)) {
        if (obj_hdl->type != REGULAR_FILE) {
            LogFullDebug(COMPONENT_FSAL, "Setting size on non-regular file");
            return fsalstat(ERR_FSAL_INVAL, EINVAL);
        }

        bool reusing_open_state_fd = false;
        status = fsal_find_fd(NULL, obj_hdl, NULL, &handle->share,
                              bypass, state, FSAL_O_RDWR, NULL, NULL,
                              &has_lock, &closefd, false,
                              &reusing_open_state_fd);

        if (FSAL_IS_ERROR(status)) {
            LogFullDebug(COMPONENT_FSAL, "fsal_find_fd status = %s",
                         fsal_err_txt(status));
            goto out;
        }
    }

    struct stat attr;
    int mask = 0;
    memset(&attr, 0, sizeof(attr));

    if (FSAL_TEST_MASK(attrib_set->valid_mask, ATTR_SIZE)) {
        mask |= LIZ_SET_ATTR_SIZE;
        attr.st_size = attrib_set->filesize;
        LogFullDebug(COMPONENT_FSAL, "setting size to %lld",
                     (long long)attr.st_size);
    }

    if (FSAL_TEST_MASK(attrib_set->valid_mask, ATTR_MODE)) {
        mask |= LIZ_SET_ATTR_MODE;
        attr.st_mode = fsal2unix_mode(attrib_set->mode);
    }

    if (FSAL_TEST_MASK(attrib_set->valid_mask, ATTR_OWNER)) {
        mask |= LIZ_SET_ATTR_UID;
        attr.st_uid = attrib_set->owner;
    }

    if (FSAL_TEST_MASK(attrib_set->valid_mask, ATTR_GROUP)) {
        mask |= LIZ_SET_ATTR_GID;
        attr.st_gid = attrib_set->group;
    }

    if (FSAL_TEST_MASK(attrib_set->valid_mask, ATTR_ATIME)) {
        mask |= LIZ_SET_ATTR_ATIME;
        attr.st_atim = attrib_set->atime;
    }

    if (FSAL_TEST_MASK(attrib_set->valid_mask, ATTR_ATIME_SERVER)) {
        mask |= LIZ_SET_ATTR_ATIME_NOW;
    }

    if (FSAL_TEST_MASK(attrib_set->valid_mask, ATTR_MTIME)) {
        mask |= LIZ_SET_ATTR_MTIME;
        attr.st_mtim = attrib_set->mtime;
    }

    if (FSAL_TEST_MASK(attrib_set->valid_mask, ATTR_MTIME_SERVER)) {
        mask |= LIZ_SET_ATTR_MTIME_NOW;
    }

    liz_attr_reply_t reply;
    int rc = fs_setattr(export->fsInstance, &op_ctx->creds,
                              handle->inode, &attr, mask, &reply);

    if (rc < 0) {
        status = fsalLastError();
        goto out;
    }

#ifdef ENABLE_NFS_ACL_SUPPORT
    if (FSAL_TEST_MASK(attrib_set->valid_mask, ATTR_ACL)) {
        status = setACL(export, handle->inode, attrib_set->acl,
                                 reply.attr.st_mode);
    }
#endif

out:
    if (has_lock) {
        PTHREAD_RWLOCK_unlock(&obj_hdl->obj_lock);
    }

    return status;
}

/*! \brief Manage closing a file when a state is no longer needed.
 *
 * \see fsal_api.h for more information
 */
static fsal_status_t _close2(struct fsal_obj_handle *obj_hdl,
                                 struct state_t *state)
{
    struct FSHandle *handle;
    handle = container_of(obj_hdl,
                          struct FSHandle, fileHandle);

    LogFullDebug(COMPONENT_FSAL, "export = %" PRIu16 " inode = %" PRIu32,
                 handle->uniqueKey.exportId, handle->inode);

    if (state->state_type == STATE_TYPE_SHARE ||
        state->state_type == STATE_TYPE_NLM_SHARE ||
        state->state_type == STATE_TYPE_9P_FID) {
        /* This is a share state, we must update the share counters */

        /* This can block over an I/O operation. */
        PTHREAD_RWLOCK_wrlock(&obj_hdl->obj_lock);
        update_share_counters(&handle->share, handle->fileDescriptor.openFlags,
                              FSAL_O_CLOSED);
        PTHREAD_RWLOCK_unlock(&obj_hdl->obj_lock);
    }

    return close_fd(handle, &handle->fileDescriptor);
}

/*! \brief Create a symbolic link
 *
 * \see fsal_api.h for more information
 */
static fsal_status_t _symlink(struct fsal_obj_handle *dir_hdl,
                                  const char *name,
                                  const char *link_path,
                                  struct fsal_attrlist *attrib,
                                  struct fsal_obj_handle **new_obj,
                                  struct fsal_attrlist *attrs_out)
{
    struct FSExport *export;
    struct FSHandle *directory, *handle;
    struct liz_entry node_entry;
    int rc;

    export = container_of(op_ctx->fsal_export,
                          struct FSExport, export);
    directory = container_of(dir_hdl,
                             struct FSHandle, fileHandle);

    LogFullDebug(COMPONENT_FSAL, "export = %" PRIu16 " parent_inode = %"
                 PRIu32 " name = %s", export->export.export_id,
                 directory->inode, name);

    rc = fs_symlink(export->fsInstance, &op_ctx->creds,
                          link_path, directory->inode, name, &node_entry);
    if (rc < 0) {
        return fsalLastError();
    }

    handle = allocateNewHandle(&node_entry.attr, export);
    *new_obj = &handle->fileHandle;

    /* We handled the mode above. */
    FSAL_UNSET_MASK(attrib->valid_mask, ATTR_MODE);

    if (attrib->valid_mask) {
        fsal_status_t status;
        /* Now per support_ex API, if there are any other attributes
         * set, go ahead and get them set now.*/
        status = (*new_obj)->obj_ops->setattr2(*new_obj, false, NULL, attrib);
        if (FSAL_IS_ERROR(status)) {
            /* Release the handle we just allocated. */
            LogFullDebug(COMPONENT_FSAL, "setattr2 status = %s",
                         fsal_err_txt(status));
            (*new_obj)->obj_ops->release(*new_obj);
            *new_obj = NULL;
        }
    }
    else {
        if (attrs_out != NULL) {
            posix2fsal_attributes_all(&node_entry.attr, attrs_out);
        }
    }

    FSAL_SET_MASK(attrib->valid_mask, ATTR_MODE);
    return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

fsal_status_t _lock_op2(struct fsal_obj_handle *obj_hdl,
                            struct state_t *state,
                            void *owner, fsal_lock_op_t lock_op,
                            fsal_lock_param_t *request_lock,
                            fsal_lock_param_t *conflicting_lock)
{
    struct FSExport *export;

    liz_err_t last_err;
    liz_fileinfo_t *fileinfo;
    liz_lock_info_t lock_info;
    fsal_status_t status = {0, 0};
    int retval = 0;
    struct FSFileDescriptor file_descriptor;
    bool has_lock = false;
    bool closefd = false;
    bool bypass = false;
    fsal_openflags_t openflags = FSAL_O_RDWR;

    export = container_of(op_ctx->fsal_export,
                          struct FSExport, export);

    LogFullDebug(COMPONENT_FSAL, "op:%d type:%d start:%" PRIu64 " length:%"
                 PRIu64 " ", lock_op, request_lock->lock_type,
                 request_lock->lock_start, request_lock->lock_length);

    // Sanity checks
    if (obj_hdl == NULL) {
        LogCrit(COMPONENT_FSAL, "obj_hdl arg is NULL.");
        return fsalstat(ERR_FSAL_FAULT, 0);
    }

    if (owner == NULL) {
        LogCrit(COMPONENT_FSAL, "owner arg is NULL.");
        return fsalstat(ERR_FSAL_FAULT, 0);
    }

    if (lock_op == FSAL_OP_LOCKT) {
        // We may end up using global fd, don't fail on a deny mode
        bypass = true;
        openflags = FSAL_O_ANY;
    }
    else if (lock_op == FSAL_OP_LOCK) {
        if (request_lock->lock_type == FSAL_LOCK_R) {
            openflags = FSAL_O_READ;
        }
        else if (request_lock->lock_type == FSAL_LOCK_W) {
            openflags = FSAL_O_WRITE;
        }
    }
    else if (lock_op == FSAL_OP_UNLOCK) {
        openflags = FSAL_O_ANY;
    }
    else {
        LogFullDebug(COMPONENT_FSAL,
                     "ERROR: Lock operation requested was not "
                     "TEST, READ, or WRITE.");
        return fsalstat(ERR_FSAL_NOTSUPP, 0);
    }

    if (lock_op != FSAL_OP_LOCKT && state == NULL) {
        LogCrit(COMPONENT_FSAL, "Non TEST operation with NULL state");
        return posix2fsal_status(EINVAL);
    }

    if (request_lock->lock_type == FSAL_LOCK_R) {
        lock_info.l_type = F_RDLCK;
    }
    else if (request_lock->lock_type == FSAL_LOCK_W) {
        lock_info.l_type = F_WRLCK;
    }
    else {
        LogFullDebug(COMPONENT_FSAL,
                     "ERROR: The requested lock type was not read or write.");
        return fsalstat(ERR_FSAL_NOTSUPP, 0);
    }

    if (lock_op == FSAL_OP_UNLOCK) {
        lock_info.l_type = F_UNLCK;
    }

    lock_info.l_pid = 0;
    lock_info.l_len = request_lock->lock_length;
    lock_info.l_start = request_lock->lock_start;

    status = find_fd(&file_descriptor, obj_hdl, bypass, state,
                     openflags, &has_lock, &closefd, true);
    // IF find_fd returned DELAY, then fd caching in mdcache is
    // turned off, which means that the consecutive attempt is very likely
    // to succeed immediately.
    if (status.major == ERR_FSAL_DELAY) {
        status = find_fd(&file_descriptor, obj_hdl, bypass, state,
                         openflags, &has_lock, &closefd, true);
    }

    if (FSAL_IS_ERROR(status)) {
        LogCrit(COMPONENT_FSAL, "Unable to find fd for lock operation");
        return status;
    }

    fileinfo = file_descriptor.fileDescriptor;
    liz_set_lock_owner(fileinfo, (uint64_t)owner);
    if (lock_op == FSAL_OP_LOCKT) {
        retval = fs_getlk(export->fsInstance,
                                &op_ctx->creds, fileinfo, &lock_info);
    }
    else {
        retval = fs_setlk(export->fsInstance,
                                &op_ctx->creds, fileinfo, &lock_info);
    }

    if (retval < 0) {
        goto err;
    }

    /* F_UNLCK is returned then the tested operation would be possible. */
    if (conflicting_lock != NULL) {
        if (lock_op == FSAL_OP_LOCKT && lock_info.l_type != F_UNLCK) {
            conflicting_lock->lock_length = lock_info.l_len;
            conflicting_lock->lock_start = lock_info.l_start;
            conflicting_lock->lock_type = lock_info.l_type;
        }
        else {
            conflicting_lock->lock_length = 0;
            conflicting_lock->lock_start = 0;
            conflicting_lock->lock_type = FSAL_NO_LOCK;
        }
    }

err:
    last_err = liz_last_err();
    if (closefd) {
        liz_release(export->fsInstance, fileinfo);
    }

    if (has_lock) {
        PTHREAD_RWLOCK_unlock(&obj_hdl->obj_lock);
    }

    if (retval < 0) {
        LogFullDebug(COMPONENT_FSAL, "Returning error %d", last_err);
        return lizardfsToFsalError(last_err);
    }
    return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/*! \brief Re-open a file that may be already opened
 *
 * \see fsal_api.h for more information
 */
static fsal_status_t _reopen2(struct fsal_obj_handle *obj_hdl,
                                  struct state_t *state,
                                  fsal_openflags_t openflags)
{
    struct FSHandle *handle;
    struct FSFileDescriptor fd, *shared_fd;
    fsal_status_t status;
    fsal_openflags_t old_openflags;

    handle = container_of(obj_hdl, struct FSHandle, fileHandle);
    shared_fd = &container_of(state,
                              struct FSFileDescriptorState, state)->fileDescriptor;

    LogFullDebug(COMPONENT_FSAL, "export = %" PRIu16 " inode = %" PRIu32,
                 handle->uniqueKey.exportId, handle->inode);

    memset(&fd, 0, sizeof(fd));

    /* This can block over an I/O operation. */
    PTHREAD_RWLOCK_wrlock(&obj_hdl->obj_lock);
    old_openflags = shared_fd->openFlags;

    /* We can conflict with old share, so go ahead and check now. */
    status = check_share_conflict(&handle->share, openflags, false);
    if (FSAL_IS_ERROR(status)) {
        PTHREAD_RWLOCK_unlock(&obj_hdl->obj_lock);
        return status;
    }

    /* Set up the new share so we can drop the lock and not have a
     * conflicting share be asserted, updating the share counters. */
    update_share_counters(&handle->share, old_openflags, openflags);
    PTHREAD_RWLOCK_unlock(&obj_hdl->obj_lock);

    status = open_fd(handle, openflags, &fd, true);
    if (!FSAL_IS_ERROR(status)) {
        close_fd(handle, shared_fd);
        *shared_fd = fd;
    }
    else {
        /* We had a failure on open - we need to revert the share.
         * This can block over an I/O operation. */
        PTHREAD_RWLOCK_wrlock(&obj_hdl->obj_lock);
        update_share_counters(&handle->share, openflags, old_openflags);
        PTHREAD_RWLOCK_unlock(&obj_hdl->obj_lock);
    }

    return status;
}

/*! \brief Create a special file
 *
 * \see fsal_api.h for more information
 */
static fsal_status_t _mknode(struct fsal_obj_handle *dir_hdl,
                                 const char *name,
                                 object_file_type_t nodetype,
                                 struct fsal_attrlist *attrib,
                                 struct fsal_obj_handle **new_obj,
                                 struct fsal_attrlist *attrs_out)
{
    struct FSExport *export;
    struct FSHandle *directory, *handle;
    struct liz_entry node_entry;
    mode_t unix_mode;
    dev_t unix_dev = 0;

    export = container_of(op_ctx->fsal_export,
                          struct FSExport, export);
    directory = container_of(dir_hdl,
                             struct FSHandle, fileHandle);

    LogFullDebug(COMPONENT_FSAL,
                 "export = %" PRIu16 " parent_inode = %" PRIu32 " mode = %"
                 PRIo32 " name = %s", export->export.export_id,
                 directory->inode, attrib->mode, name);

    unix_mode = fsal2unix_mode(attrib->mode) &
        ~op_ctx->fsal_export->exp_ops.fs_umask(op_ctx->fsal_export);

    switch (nodetype) {
    case BLOCK_FILE:
        unix_mode |= S_IFBLK;
        unix_dev = makedev(attrib->rawdev.major, attrib->rawdev.minor);
        break;
    case CHARACTER_FILE:
        unix_mode |= S_IFCHR;
        unix_dev = makedev(attrib->rawdev.major, attrib->rawdev.minor);
        break;
    case FIFO_FILE:
        unix_mode |= S_IFIFO;
        break;
    case SOCKET_FILE:
        unix_mode |= S_IFSOCK;
        break;
    default:
        LogMajor(COMPONENT_FSAL,
                 "Invalid node type in FSAL_mknode: %d", nodetype);
        return fsalstat(ERR_FSAL_INVAL, EINVAL);
    }

    int rc = fs_mknode(export->fsInstance, &op_ctx->creds,
                       directory->inode, name, unix_mode, unix_dev,
                       &node_entry);
    if (rc < 0) {
        return fsalLastError();
    }

    handle = allocateNewHandle(&node_entry.attr, export);
    *new_obj = &handle->fileHandle;

    // We handled the mode above.
    FSAL_UNSET_MASK(attrib->valid_mask, ATTR_MODE);

    if (attrib->valid_mask) {
        fsal_status_t status;
        // Setting attributes for the created object
        status = (*new_obj)->obj_ops->setattr2(*new_obj, false,
                                               NULL, attrib);
        if (FSAL_IS_ERROR(status)) {
            LogFullDebug(COMPONENT_FSAL, "setattr2 status = %s",
                         fsal_err_txt(status));
            /* Release the handle we just allocated. */
            (*new_obj)->obj_ops->release(*new_obj);
            *new_obj = NULL;
        }
    }
    else {
        if (attrs_out != NULL) {
            /* Since we haven't set any attributes other than what
             * was set on create, just use the stat results we used
             * to create the fsal_obj_handle. */
            posix2fsal_attributes_all(&node_entry.attr, attrs_out);
        }
    }

    FSAL_SET_MASK(attrib->valid_mask, ATTR_MODE);
    return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/*! \brief Read the content of a link
 *
 * \see fsal_api.h for more information
 */
static fsal_status_t _readlink(struct fsal_obj_handle *obj_hdl,
                                   struct gsh_buffdesc *link_content,
                                   bool refresh)
{
    struct FSExport *export;
    struct FSHandle *handle;
    char result[LIZARDFS_MAX_READLINK_LENGTH];

    // Sanity check
    if (obj_hdl->type != SYMBOLIC_LINK)
        return fsalstat(ERR_FSAL_FAULT, 0);

    export = container_of(op_ctx->fsal_export,
                          struct FSExport, export);
    handle = container_of(obj_hdl,
                          struct FSHandle, fileHandle);

    LogFullDebug(COMPONENT_FSAL, "export = %" PRIu16 " inode = %" PRIu32,
                 export->export.export_id, handle->inode);

    int rc = fs_readlink(export->fsInstance, &op_ctx->creds,
                               handle->inode, result,
                               LIZARDFS_MAX_READLINK_LENGTH);
    if (rc < 0) {
        return fsalLastError();
    }

    rc = MIN(rc, LIZARDFS_MAX_READLINK_LENGTH);
    link_content->addr = gsh_strldup(result, rc, &link_content->len);

    return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/*! \brief Return open status of a state.
 *
 * \see fsal_api.h for more information
 */
static fsal_openflags_t _status2(struct fsal_obj_handle *obj_hdl,
                                     struct state_t *state)
{
    struct FSFileDescriptor *file_descriptor;
    file_descriptor = &container_of(state,
                                    struct FSFileDescriptorState, state)->fileDescriptor;
    return file_descriptor->openFlags;
}

/*! \brief Merge a duplicate handle with an original handle
 *
 * \see fsal_api.h for more information
 */
static fsal_status_t _merge(struct fsal_obj_handle *orig_hdl,
                                struct fsal_obj_handle *dupe_hdl)
{
    fsal_status_t status = fsalstat(ERR_FSAL_NO_ERROR, 0);

    if (orig_hdl->type == REGULAR_FILE &&
        dupe_hdl->type == REGULAR_FILE) {
        /* We need to merge the share reservations on this file.
         * This could result in ERR_FSAL_SHARE_DENIED. */
        struct FSHandle *orig, *dupe;

        orig = container_of(orig_hdl,
                            struct FSHandle, fileHandle);
        dupe = container_of(dupe_hdl,
                            struct FSHandle, fileHandle);

        /* This can block over an I/O operation. */
        status = merge_share(orig_hdl, &orig->share, &dupe->share);
    }

    return status;
}

/*! \brief Get Extended Attribute.
 *
 *  This function gets an extended attribute of an object.
 *
 *  \param [in]     obj_hdl 	Input object to query
 *  \param [in]     xa_name     Input xattr name
 *  \param [out]    xa_value	Output xattr value
 *
 *  \returns: FSAL status
 *
 *  \see fsal_api.h for more information
 */
static fsal_status_t _getxattrs(struct fsal_obj_handle *obj_hdl,
                                    xattrkey4 *xa_name,
                                    xattrvalue4 *xa_value)
{
    struct FSExport *export;
    export = container_of(op_ctx->fsal_export,
                          struct FSExport, export);

    struct FSHandle *handle;
    handle = container_of(obj_hdl, struct FSHandle, fileHandle);

    size_t curr_size = 0;
    int rc = fs_getxattr(export->fsInstance, &op_ctx->creds,
                               handle->inode, xa_name->utf8string_val,
                               xa_value->utf8string_len, &curr_size,
                               (uint8_t *)xa_value->utf8string_val);

    if (rc < 0) {
        LogFullDebug(COMPONENT_FSAL, "GETXATTRS failed returned rc = %d ", rc);
        return lizardfsToFsalError(rc);
    }

    if (curr_size && curr_size <= xa_value->utf8string_len) {
        // Updating the real size
        xa_value->utf8string_len = curr_size;
        // Make sure utf8string is NUL terminated
        xa_value->utf8string_val[xa_value->utf8string_len] = '\0';
    }

    return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/*! \brief Set Extended Attribute.
 *
 *  This function gets an extended attribute of an object.
 *
 *  \param [in]     obj_hdl 	Input object to set
 *  \param [in]     option      Input xattr type
 *  \param [in]     xa_name     Input xattr name to set
 *  \param [in]     xa_value	Input xattr value to set
 *
 *  \returns: FSAL status
 *
 *  \see fsal_api.h for more information
 */
static fsal_status_t _setxattrs(struct fsal_obj_handle *obj_hdl,
                                    setxattr_option4 option,
                                    xattrkey4 *xa_name,
                                    xattrvalue4 *xa_value)
{
    struct FSExport *export;
    export = container_of(op_ctx->fsal_export,
                          struct FSExport, export);

    struct FSHandle *handle;
    handle = container_of(obj_hdl, struct FSHandle, fileHandle);

    int rc = fs_setxattr(export->fsInstance, &op_ctx->creds,
                               handle->inode, xa_name->utf8string_val,
                               (const uint8_t *)xa_value->utf8string_val,
                               xa_value->utf8string_len, option);

    if (rc < 0) {
        LogDebug(COMPONENT_FSAL, "SETXATTRS returned rc %d", rc);
        return lizardfsToFsalError(rc);
    }

    return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/*! \brief List Extended Attributes.
 *
 *  This function gets an extended attribute of an object.
 *
 *  \param [in]     obj_hdl 	Input object to list
 *  \param [in]     la_maxcount	Input maximum number of bytes for names
 *  \param [in,out]	la_cookie	In/out cookie
 *  \param [out]	lr_eof      Output eof set if no more extended attributes
 *  \param [out]	lr_names	Output list of extended attribute names this
 *                              buffer size is double the size of la_maxcount
 *                              to allow for component4 overhead
 *
 *  \returns: FSAL status
 *
 *  \see fsal_api.h for more information
 */
static fsal_status_t _listxattrs(struct fsal_obj_handle *obj_hdl,
                                     count4 la_maxcount,
                                     nfs_cookie4 *la_cookie,
                                     bool_t *lr_eof,
                                     xattrlist4 *lr_names)
{
    char *buf = NULL;
    fsal_status_t status;

    struct FSExport *export;
    export = container_of(op_ctx->fsal_export,
                          struct FSExport, export);

    struct FSHandle *handle;
    handle = container_of(obj_hdl, struct FSHandle, fileHandle);

    LogFullDebug(COMPONENT_FSAL, "in cookie %llu length %d",
                 (unsigned long long)*la_cookie, la_maxcount);

    // Size of list of extended attributes
    size_t curr_size = 0;

    // First time, the function is called to get the size of xattr list
    int rc = fs_listxattr(export->fsInstance, &op_ctx->creds,
                                handle->inode, 0, &curr_size, NULL);

    if (rc < 0) {
        LogDebug(COMPONENT_FSAL, "LISTXATTRS returned rc %d", rc);
        return lizardfsToFsalError(rc);
    }

    // If xattr were retrieved and they can be allocated
    if (curr_size && curr_size < la_maxcount) {
        buf = gsh_malloc(curr_size);

        // Second time, the function is called to retrieve the xattr list
        rc = fs_listxattr(export->fsInstance, &op_ctx->creds,
                                handle->inode, curr_size, &curr_size, buf);

        if (rc < 0) {
            LogDebug(COMPONENT_FSAL, "LISTXATTRS returned rc %d", rc);
            gsh_free(buf);
            return lizardfsToFsalError(rc);
        }

        // Setting retrieved extended attributes to Ganesha
        status = fsal_listxattr_helper(buf, curr_size, la_maxcount, la_cookie,
                                       lr_eof, lr_names);
        // Releasing allocated buffer
        gsh_free(buf);
    }

    return status;
}

/*! \brief Remove Extended Attributes.
 *
 *  This function remove an extended attribute of an object.
 *
 *  \param [in] obj_hdl 	Input object to set
 *  \param [in] xa_name     Input xattr name to remove
 *
 *  \returns: FSAL status
 *
 *  \see fsal_api.h for more information
 */
static fsal_status_t _removexattrs(struct fsal_obj_handle *obj_hdl,
                                       xattrkey4 *xa_name)
{
    struct FSExport *export;
    export = container_of(op_ctx->fsal_export,
                          struct FSExport, export);

    struct FSHandle *handle;
    handle = container_of(obj_hdl, struct FSHandle, fileHandle);

    int rc = fs_removexattr(export->fsInstance, &op_ctx->creds,
                                  handle->inode, xa_name->utf8string_val);

    if (rc < 0) {
        LogFullDebug(COMPONENT_FSAL, "REMOVEXATTR returned rc %d", rc);
        return lizardfsToFsalError(rc);
    }

    return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

void initializeFilesystemOperations(struct fsal_obj_ops *ops)
{
    fsal_default_obj_ops_init(ops);
    ops->release = _release;
    ops->lookup = _lookup;
    ops->readdir = _readdir;
    ops->getattrs = _getattrs;
    ops->handle_to_wire = _handle_to_wire;
    ops->handle_to_key = _handle_to_key;
    ops->open2 = _open2;
    ops->read2 = _read2;
    ops->mkdir = _mkdir;
    ops->link = _link;
    ops->rename = _rename;
    ops->unlink = _unlink;
    ops->close = _close;
    ops->write2 = _write2;
    ops->commit2 = _commit2;
    ops->setattr2 = _setattr2;
    ops->close2 = _close2;
    ops->symlink = _symlink;
    ops->lock_op2 = _lock_op2;
    ops->reopen2 = _reopen2;
    ops->mknode = _mknode;
    ops->readlink = _readlink;
    ops->status2 = _status2;
    ops->merge = _merge;
    ops->getxattrs = _getxattrs;
    ops->setxattrs = _setxattrs;
    ops->listxattrs = _listxattrs;
    ops->removexattrs = _removexattrs;
}

struct FSHandle *allocateNewHandle(const struct stat *attr,
                                   struct FSExport *export)
{
    struct FSHandle *result = NULL;
    result = gsh_calloc(1, sizeof(struct FSHandle));

    result->inode = attr->st_ino;
    result->uniqueKey.moduleId = FSAL_ID_LIZARDFS;
    result->uniqueKey.exportId = export->export.export_id;
    result->uniqueKey.inode = attr->st_ino;

    fsal_obj_handle_init(&result->fileHandle, &export->export,
                         posix2fsal_type(attr->st_mode));

    result->fileHandle.obj_ops = &LizardFS.operations;
    result->fileHandle.fsid = posix2fsal_fsid(attr->st_dev);
    result->fileHandle.fileid = attr->st_ino;
    result->export = export;
    return result;
}

void deleteHandle(struct FSHandle *obj)
{
    fsal_obj_handle_fini(&obj->fileHandle);
    gsh_free(obj);
}
