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
                              fsal_readdir_cb readdirCb,
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
    context = createFSALContext(export->fsInstance, &op_ctx->creds);

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

            result = readdirCb(buffer[i].name, &handle->fileHandle,
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
static fsal_status_t _getattrs(struct fsal_obj_handle *objectHandle,
                               struct fsal_attrlist *attributes)
{
    struct FSExport *export;
    struct FSHandle *handle;
    struct liz_attr_reply lzfs_attrs;

    export = container_of(op_ctx->fsal_export, struct FSExport, export);
    handle = container_of(objectHandle, struct FSHandle, fileHandle);

    LogFullDebug(COMPONENT_FSAL, "ALLI: export = %" PRIu16 " inode = %" PRIu32,
                 export->export.export_id, handle->inode);

    int rc = fs_getattr(export->fsInstance, &op_ctx->creds,
                        handle->inode, &lzfs_attrs);

    if (rc < 0) {
        if (attributes->request_mask & ATTR_RDATTR_ERR) {
            attributes->valid_mask = ATTR_RDATTR_ERR;
        }
        return fsalLastError();
    }

    posix2fsal_attributes_all(&lzfs_attrs.attr, attributes);

#ifdef ENABLE_NFS_ACL_SUPPORT
    if (attributes->request_mask & ATTR_ACL) {
        fsal_status_t status = getACL(export, handle->inode,
                                      lzfs_attrs.attr.st_uid, &attributes->acl);

        if (!FSAL_IS_ERROR(status)) {
            attributes->valid_mask |= ATTR_ACL;
        }
    }
#endif

    return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/*! \brief Write wire handle
 *
 * \see fsal_api.h for more information
 */
static fsal_status_t _handle_to_wire(const struct fsal_obj_handle *objectHandle,
                                     uint32_t outputType,
                                     struct gsh_buffdesc *bufferDescriptor)
{
    // Unused variable
    (void ) outputType;

    struct FSHandle *handle;
    handle = container_of(objectHandle, struct FSHandle, fileHandle);

    liz_inode_t inode = handle->inode;
    if (bufferDescriptor->len < sizeof(liz_inode_t)) {
        LogMajor(COMPONENT_FSAL,
                 "Space too small for handle. Need  %zu, have %zu",
                 sizeof(liz_inode_t), bufferDescriptor->len);
        return fsalstat(ERR_FSAL_TOOSMALL, 0);
    }

    memcpy(bufferDescriptor->addr, &inode, sizeof(liz_inode_t));
    bufferDescriptor->len = sizeof(inode);

    return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/*! \brief Get key for handle
 *
 * \see fsal_api.h for more information
 */
static void _handle_to_key(struct fsal_obj_handle *objectHandle,
                           struct gsh_buffdesc *bufferDescriptor)
{
    struct FSHandle *handle;
    handle = container_of(objectHandle, struct FSHandle, fileHandle);

    bufferDescriptor->addr = &handle->uniqueKey;
    bufferDescriptor->len = sizeof(struct FSALKey);
}

static fsal_status_t openFileDescriptor(struct FSHandle *handle,
                                        fsal_openflags_t openflags,
                                        struct FSFileDescriptor *fd,
                                        bool noAccessCheck)
{
    struct FSExport *export;
    int posixFlags;

    fsal2posix_openflags(openflags, &posixFlags);
    if (noAccessCheck) {
        posixFlags |= O_CREAT;
    }

    export = container_of(op_ctx->fsal_export, struct FSExport, export);

    LogFullDebug(COMPONENT_FSAL,
                 "fd = %p fd->fd = %p openflags = %x, posix_flags = %x",
                 fd, fd->fileDescriptor, openflags, posixFlags);

    assert(fd->fileDescriptor == NULL &&
           fd->openFlags == FSAL_O_CLOSED && openflags != 0);

    fd->fileDescriptor = fs_open(export->fsInstance, &op_ctx->creds,
                                 handle->inode, posixFlags);

    if (!fd->fileDescriptor) {
        LogFullDebug(COMPONENT_FSAL, "open failed with %s",
                     liz_error_string(liz_last_err()));
        return fsalLastError();
    }

    fd->openFlags = openflags;
    return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

static fsal_status_t closeFileDescriptor(struct FSHandle *handle,
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

static fsal_status_t openByHandle(struct fsal_obj_handle *objectHandle,
                                  struct state_t *state,
                                  fsal_openflags_t openflags,
                                  enum fsal_create_mode createmode,
                                  fsal_verifier_t verifier,
                                  struct fsal_attrlist *attributes,
                                  bool *callerPermissionCheck,
                                  bool afterMknode)
{
    struct FSExport *export;
    struct FSHandle *handle;
    struct FSFileDescriptor *fd;
    fsal_status_t status;
    int posixFlags;

    handle = container_of(objectHandle, struct FSHandle, fileHandle);
    export = container_of(op_ctx->fsal_export, struct FSExport, export);

    PTHREAD_RWLOCK_wrlock(&objectHandle->obj_lock);

    if (state != NULL) {
        fd = &container_of(state, struct FSFileDescriptorState,
                           state)->fileDescriptor;

        // Prepare to take the share reservation, but only if we
        // are called with a valid state

        // Check share reservation conflicts
        status = check_share_conflict(&handle->share, openflags, false);

        if (FSAL_IS_ERROR(status)) {
            PTHREAD_RWLOCK_unlock(&objectHandle->obj_lock);
            return status;
        }

        // Take the share reservation now by updating the counters
        update_share_counters(&handle->share, FSAL_O_CLOSED, openflags);
        PTHREAD_RWLOCK_unlock(&objectHandle->obj_lock);
    }
    else {
        // We need to use the global file descriptor to continue
        fd = &handle->fileDescriptor;
    }

    status = openFileDescriptor(handle, openflags, fd, afterMknode);
    if (FSAL_IS_ERROR(status)) {
        if (state != NULL) {
            goto undo_share;
        }

        PTHREAD_RWLOCK_unlock(&objectHandle->obj_lock);
        return status;
    }

    fsal2posix_openflags(openflags, &posixFlags);
    bool truncated = (posixFlags & O_TRUNC) != 0;

    bool isValidFile = (createmode >= FSAL_EXCLUSIVE || truncated || attributes);

    if (isValidFile) {
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

        if (!FSAL_IS_ERROR(status)) {
                // Now check verifier for exclusive
                if (createmode >= FSAL_EXCLUSIVE &&
                    createmode != FSAL_EXCLUSIVE_9P &&
                    !check_verifier_stat(&lzfs_attrs.attr, verifier, false)) {
                    // Verifier didn't match, return EEXIST
                    status = fsalstat(posix2fsal_error(EEXIST), EEXIST);
                }
        }

        if (!FSAL_IS_ERROR(status) && attributes) {
            posix2fsal_attributes_all(&lzfs_attrs.attr, attributes);
        }
    }

    if (state == NULL) {
        PTHREAD_RWLOCK_unlock(&objectHandle->obj_lock);

        // If success, we haven't done any permission check so ask the
        // caller to do so
        *callerPermissionCheck = !FSAL_IS_ERROR(status);

        // If no state, return status
        return status;
    }

    if (!FSAL_IS_ERROR(status)) {
        // Return success. We haven't done any permission check so ask
        // the caller to do so.
        *callerPermissionCheck = true;
        return status;
    }

    closeFileDescriptor(handle, fd);

undo_share:
    // On error we need to release our share reservation
    // and undo the update of the share counters
    PTHREAD_RWLOCK_wrlock(&objectHandle->obj_lock);
    update_share_counters(&handle->share, openflags, FSAL_O_CLOSED);
    PTHREAD_RWLOCK_unlock(&objectHandle->obj_lock);

    return status;
}

static fsal_status_t openByName(struct fsal_obj_handle *objectHandle,
                                struct state_t *state,
                                fsal_openflags_t openflags,
                                const char *name,
                                fsal_verifier_t verifier,
                                struct fsal_attrlist *attributes,
                                bool *callerPermissionCheck)
{
    struct fsal_obj_handle *temp = NULL;
    fsal_status_t status;

    // Ganesha doesn't has open by name, so we need to get the name with lookup
    status = objectHandle->obj_ops->lookup(objectHandle, name, &temp, NULL);

    if (FSAL_IS_ERROR(status)) {
        LogFullDebug(COMPONENT_FSAL, "lookup returned %s",
                     fsal_err_txt(status));
        return status;
    }

    status = openByHandle(temp, state, openflags, FSAL_NO_CREATE, verifier,
                          attributes, callerPermissionCheck, false);

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
static fsal_status_t _open2(struct fsal_obj_handle *objectHandle,
                            struct state_t *state,
                            fsal_openflags_t openflags,
                            enum fsal_create_mode createmode,
                            const char *name,
                            struct fsal_attrlist *attributesToSet,
                            fsal_verifier_t verifier,
                            struct fsal_obj_handle **createdObject,
                            struct fsal_attrlist *attributes,
                            bool *callerPermissionCheck)
{
    struct FSExport *export;
    struct FSHandle *handle;
    fsal_status_t status;

    LogAttrlist(COMPONENT_FSAL, NIV_FULL_DEBUG, "attrs ", attributesToSet, false);

    if (createmode >= FSAL_EXCLUSIVE) {
        // Now fixup attrs for verifier if exclusive create
        set_common_verifier(attributesToSet, verifier, false);
    }

    if (name == NULL) {
        return openByHandle(objectHandle, state, openflags, createmode,
                            verifier, attributes, callerPermissionCheck, false);
    }

    if (createmode == FSAL_NO_CREATE) {
        return openByName(objectHandle, state, openflags, name, verifier,
                          attributes, callerPermissionCheck);
    }

    // Create file
    export = container_of(op_ctx->fsal_export, struct FSExport, export);
    handle = container_of(objectHandle, struct FSHandle, fileHandle);

    // Fetch the mode attribute to use in the openat system call
    mode_t unix_mode = fsal2unix_mode(attributesToSet->mode) &
        ~op_ctx->fsal_export->exp_ops.fs_umask(op_ctx->fsal_export);

    // Don't set the mode if we later set the attributes
    FSAL_UNSET_MASK(attributesToSet->valid_mask, ATTR_MODE);

    struct liz_entry fsAttributes;
    int rc = fs_mknode(export->fsInstance, &op_ctx->creds, handle->inode,
                       name, unix_mode, 0, &fsAttributes);

    if (rc < 0 && liz_last_err() == LIZARDFS_ERROR_EEXIST &&
            createmode == FSAL_UNCHECKED) {
        return openByName(objectHandle, state, openflags, name, verifier,
                          attributes, callerPermissionCheck);
    }

    if (rc < 0) {
        return fsalLastError();
    }

    // File has been created by us.
    *callerPermissionCheck = false;

    struct FSHandle *newHandle = allocateNewHandle(&fsAttributes.attr, export);
    if (newHandle == NULL) {
        status = fsalstat(posix2fsal_error(ENOMEM), ENOMEM);
        goto fileerr;
    }

    *createdObject = &newHandle->fileHandle;

    if (attributesToSet->valid_mask != 0) {
        status = (*createdObject)->obj_ops->setattr2(*createdObject, false,
                                                     state, attributesToSet);
        if (FSAL_IS_ERROR(status)) {
            goto fileerr;
        }

        if (attributes != NULL) {
            status = (*createdObject)->obj_ops->getattrs(*createdObject, attributes);
            if (FSAL_IS_ERROR(status) &&
                    (attributes->request_mask & ATTR_RDATTR_ERR) == 0) {
                goto fileerr;
            }

            attributes = NULL;
        }
    }

    if (attributes != NULL) {
        posix2fsal_attributes_all(&fsAttributes.attr, attributes);
    }

    return openByHandle(*createdObject, state, openflags, createmode,
                        verifier, NULL, callerPermissionCheck, true);

fileerr:
    (*createdObject)->obj_ops->release(*createdObject);
    *createdObject = NULL;

    rc = fs_unlink(export->fsInstance, &op_ctx->creds, handle->inode, name);

    if (rc < 0) {
        return fsalLastError();
    }

    return status;
}

static fsal_status_t openFunction(struct fsal_obj_handle *objectHandle,
                                  fsal_openflags_t openflags,
                                  struct fsal_fd *fileDescriptor)
{
    struct FSHandle *handle;
    handle = container_of(objectHandle, struct FSHandle, fileHandle);
    return openFileDescriptor(handle, openflags,
                              (struct FSFileDescriptor *)fileDescriptor, true);
}

static fsal_status_t closeFunction(struct fsal_obj_handle *objectHandle,
                                   struct fsal_fd *fileDescriptor)
{
    struct FSHandle *handle;
    handle = container_of(objectHandle, struct FSHandle, fileHandle);
    return closeFileDescriptor(handle,
                               (struct FSFileDescriptor *)fileDescriptor);
}

static fsal_status_t findFileDescriptor(struct FSFileDescriptor *fileDescriptor,
                                        struct fsal_obj_handle *objectHandle,
                                        bool bypass, struct state_t *state,
                                        fsal_openflags_t openflags, bool *hasLock,
                                        bool *closeFileDescriptor,
                                        bool openForLocks)
{
    struct FSHandle *handle;
    handle = container_of(objectHandle, struct FSHandle, fileHandle);

    struct FSFileDescriptor emptyFileDescriptor = {0, NULL};
    struct FSFileDescriptor *usableFileDescriptor = &emptyFileDescriptor;

    fsal_status_t status;

    bool canReuseOpenedFd = false;
    status = fsal_find_fd((struct fsal_fd **)&usableFileDescriptor,
                          objectHandle,
                          (struct fsal_fd *)&handle->fileDescriptor,
                          &handle->share, bypass, state, openflags,
                          openFunction, closeFunction, hasLock,
                          closeFileDescriptor, openForLocks,
                          &canReuseOpenedFd);

    *fileDescriptor = *usableFileDescriptor;
    return status;
}

/**
 * \brief Read data from a file
 *
 * \see fsal_api.h for more information
 */
static void _read2(struct fsal_obj_handle *objectHandle,
                   bool bypass, fsal_async_cb doneCb,
                   struct fsal_io_arg *readArg, void *callerArg)
{
    struct FSExport *export;
    struct FSHandle *handle;
    struct FSFileDescriptor fileDescriptor;
    fsal_status_t status;

    bool hasLock = false;
    bool closeFd = false;
    ssize_t bytes;
    uint64_t offset = readArg->offset;

    export = container_of(op_ctx->fsal_export, struct FSExport, export);
    handle = container_of(objectHandle, struct FSHandle, fileHandle);

    LogFullDebug(COMPONENT_FSAL, "export = %" PRIu16 " inode = %" PRIu32
                 " offset=%" PRIu64, export->export.export_id,
                 handle->inode, offset);

    if (readArg->info != NULL) {
        // Currently we don't support READ_PLUS
        doneCb(objectHandle, fsalstat(ERR_FSAL_NOTSUPP, 0), readArg, callerArg);
        return;
    }

    status = findFileDescriptor(&fileDescriptor, objectHandle, bypass,
                                readArg->state, FSAL_O_READ, &hasLock,
                                &closeFd, false);

    if (FSAL_IS_ERROR(status)) {
        goto out;
    }

    readArg->io_amount = 0;
    for (int i = 0; i < readArg->iov_count; i++) {
        bytes = fs_read(export->fsInstance, &op_ctx->creds,
                        fileDescriptor.fileDescriptor, offset,
                        readArg->iov[i].iov_len,
                        readArg->iov[i].iov_base);

        if (bytes < 0) {
            status = fsalLastError();
            goto out;
        }
        else if (bytes == 0) {
            readArg->end_of_file = true;
            break;
        }

        readArg->io_amount += bytes;
        offset += bytes;
    }

    readArg->end_of_file = (readArg->io_amount == 0);

out:
    if (closeFd) {
        closeFileDescriptor(handle, &fileDescriptor);
    }

    if (hasLock) {
        PTHREAD_RWLOCK_unlock(&objectHandle->obj_lock);
    }

    doneCb(objectHandle, status, readArg, callerArg);
}

/*! \brief Create a directory
 *
 * \see fsal_api.h for more information
 */
static fsal_status_t _mkdir(struct fsal_obj_handle *directoryHandle,
                            const char *name,
                            struct fsal_attrlist *attributesToSet,
                            struct fsal_obj_handle **createdObject,
                            struct fsal_attrlist *attributes)
{
    struct FSExport *export;
    struct FSHandle *directory, *handle;
    struct liz_entry directoryEntry;
    fsal_status_t status;

    export = container_of(op_ctx->fsal_export, struct FSExport, export);
    directory = container_of(directoryHandle, struct FSHandle, fileHandle);

    LogFullDebug(COMPONENT_FSAL, "export = %" PRIu16 " parent_inode = %"
                 PRIu32 " mode = %" PRIo32 " name = %s", export->export.export_id,
                 directory->inode, attributesToSet->mode, name);

    mode_t unix_mode = fsal2unix_mode(attributesToSet->mode) &
            ~op_ctx->fsal_export->exp_ops.fs_umask(op_ctx->fsal_export);

    int rc = fs_mkdir(export->fsInstance, &op_ctx->creds, directory->inode,
                      name, unix_mode, &directoryEntry);

    if (rc < 0) {
        return fsalLastError();
    }

    handle = allocateNewHandle(&directoryEntry.attr, export);
    *createdObject = &handle->fileHandle;

    FSAL_UNSET_MASK(attributesToSet->valid_mask, ATTR_MODE);

    if (attributesToSet->valid_mask) {
        status = (*createdObject)->obj_ops->setattr2(*createdObject, false,
                                                     NULL, attributesToSet);

        if (FSAL_IS_ERROR(status)) {
            LogFullDebug(COMPONENT_FSAL, "setattr2 status=%s",
                         fsal_err_txt(status));

            // Release the handle we just allocate
            (*createdObject)->obj_ops->release(*createdObject);
            *createdObject = NULL;
        }
        else if (attributes != NULL) {
             // We ignore errors here. The mkdir and setattr succeeded,
             // so we don't want to return error if the getattrs fails.
             // We'll just return no attributes in that case.
            (*createdObject)->obj_ops->getattrs(*createdObject, attributes);
        }
    }
    else if (attributes != NULL) {
        // Since we haven't set any attributes other than what
        // was set on create, just use the stat results we used
        // to create the fsal_obj_handle.
        posix2fsal_attributes_all(&directoryEntry.attr, attributes);
    }

    FSAL_SET_MASK(attributesToSet->valid_mask, ATTR_MODE);
    return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/*! \brief Create a new link
 *
 * \see fsal_api.h for more information
 */
static fsal_status_t _link(struct fsal_obj_handle *objectHandle,
                           struct fsal_obj_handle *destinationDirHandle,
                           const char *name)
{
    struct FSExport *export;
    struct FSHandle *handle, *dest_directory;

    export = container_of(op_ctx->fsal_export, struct FSExport, export);
    handle = container_of(objectHandle, struct FSHandle, fileHandle);

    dest_directory = container_of(destinationDirHandle,
                                  struct FSHandle, fileHandle);

    LogFullDebug(COMPONENT_FSAL, "export = %"
                 PRIu16 " inode = %" PRIu32 " dest_inode = %" PRIu32
                 " name = %s", export->export.export_id,
                 handle->inode, dest_directory->inode, name);

    liz_entry_t entry;
    int rc = fs_link(export->fsInstance, &op_ctx->creds, handle->inode,
                     dest_directory->inode, name, &entry);

    if (rc < 0) {
        return fsalLastError();
    }

    return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/*! \brief Rename a file
 *
 * \see fsal_api.h for more information
 */
static fsal_status_t _rename(struct fsal_obj_handle *objectHandle,
                             struct fsal_obj_handle *oldParentHandle,
                             const char *oldName,
                             struct fsal_obj_handle *newParentHandle,
                             const char *newName)
{
    struct FSExport *export;
    struct FSHandle *oldDir, *newDir;

    export = container_of(op_ctx->fsal_export, struct FSExport, export);
    oldDir = container_of(oldParentHandle, struct FSHandle, fileHandle);
    newDir = container_of(newParentHandle, struct FSHandle, fileHandle);

    LogFullDebug(COMPONENT_FSAL, "export=%" PRIu16 " old_inode=%" PRIu32
                 " new_inode=%" PRIu32 " old_name=%s new_name=%s",
                 export->export.export_id, oldDir->inode,
                 newDir->inode, oldName, newName);

    int rc = fs_rename(export->fsInstance, &op_ctx->creds, oldDir->inode,
                       oldName, newDir->inode, newName);

    if (rc < 0) {
        return fsalLastError();
    }

    return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/*! \brief Remove a name from a directory
 *
 * \see fsal_api.h for more information
 */
static fsal_status_t _unlink(struct fsal_obj_handle *parentDirHandle,
                             struct fsal_obj_handle *objectHandle,
                             const char *name)
{
    struct FSExport *export;
    struct FSHandle *directory;
    int rc;

    export = container_of(op_ctx->fsal_export, struct FSExport, export);
    directory = container_of(parentDirHandle, struct FSHandle, fileHandle);

    LogFullDebug(COMPONENT_FSAL, "export = %" PRIu16 " parent_inode = %"
                 PRIu32 " name = %s type = %s", export->export.export_id,
                 directory->inode, name,
                 object_file_type_to_str(objectHandle->type));

    if (objectHandle->type != DIRECTORY) {
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
static fsal_status_t _close(struct fsal_obj_handle *objectHandle)
{
    fsal_status_t status;
    struct FSHandle *handle;
    handle = container_of(objectHandle, struct FSHandle, fileHandle);

    LogFullDebug(COMPONENT_FSAL, "export=%" PRIu16 " inode=%" PRIu32,
                 handle->uniqueKey.exportId, handle->inode);

    PTHREAD_RWLOCK_wrlock(&objectHandle->obj_lock);

    if (handle->fileDescriptor.openFlags == FSAL_O_CLOSED)
        status = fsalstat(ERR_FSAL_NOT_OPENED, 0);
    else
        status = closeFileDescriptor(handle, &handle->fileDescriptor);

    PTHREAD_RWLOCK_unlock(&objectHandle->obj_lock);

    return status;
}

/*! \brief Write data to a file
 *
 * \see fsal_api.h for more information
 */
static void _write2(struct fsal_obj_handle *objectHandle,
                    bool bypass, fsal_async_cb doneCb,
                    struct fsal_io_arg *writeArg,
                    void *callerArg)
{
    struct FSExport *export;
    struct FSHandle *handle;
    struct FSFileDescriptor fileDescriptor;

    fsal_status_t status;
    bool hasLock = false;
    bool closeFd = false;

    ssize_t bytes;
    uint64_t offset = writeArg->offset;

    export = container_of(op_ctx->fsal_export, struct FSExport, export);
    handle = container_of(objectHandle, struct FSHandle, fileHandle);

    LogFullDebug(COMPONENT_FSAL, "export=%" PRIu16 " inode=%" PRIu32
                 " offset=%" PRIu64, export->export.export_id,
                 handle->inode, offset);

    if (writeArg->info) {
        return doneCb(objectHandle, fsalstat(ERR_FSAL_NOTSUPP, 0),
                      writeArg, callerArg);
    }

    status = findFileDescriptor(&fileDescriptor, objectHandle, bypass,
                                writeArg->state, FSAL_O_WRITE,
                                &hasLock, &closeFd, false);

    if (FSAL_IS_ERROR(status)) {
        goto out;
    }

    for (int i = 0; i < writeArg->iov_count; i++) {
        bytes = fs_write(export->fsInstance, &op_ctx->creds,
                         fileDescriptor.fileDescriptor, offset,
                         writeArg->iov[i].iov_len,
                         writeArg->iov[i].iov_base);

        if (bytes < 0) {
            status = fsalLastError();
            goto out;
        }
        else {
            writeArg->io_amount = bytes;
            if (writeArg->fsal_stable) {
                int rc = fs_fsync(export->fsInstance, &op_ctx->creds,
                                  fileDescriptor.fileDescriptor);

                if (rc < 0) {
                    status = fsalLastError();
                }
            }
        }

        writeArg->io_amount += bytes;
        offset += bytes;
    }

out:
    if (closeFd) {
        closeFileDescriptor(handle, &fileDescriptor);
    }

    if (hasLock) {
        PTHREAD_RWLOCK_unlock(&objectHandle->obj_lock);
    }

    doneCb(objectHandle, status, writeArg, callerArg);
}

/*! \brief Commit written data
 *
 * \see fsal_api.h for more information
 */
static fsal_status_t _commit2(struct fsal_obj_handle *objectHandle,
                              off_t offset, size_t length)
{
    struct FSExport *export;
    struct FSHandle *handle;

    struct FSFileDescriptor emptyFileDescriptor = {0, NULL};
    struct FSFileDescriptor *fileDescriptor = &emptyFileDescriptor;

    fsal_status_t status;
    bool hasLock = false;
    bool closeFd = false;

    export = container_of(op_ctx->fsal_export, struct FSExport, export);
    handle = container_of(objectHandle, struct FSHandle, fileHandle);

    LogFullDebug(COMPONENT_FSAL, "export = %" PRIu16 " inode = %" PRIu32
                 " offset = %lli len = %zu", export->export.export_id,
                 handle->inode, (long long)offset, length);

    status = fsal_reopen_obj(objectHandle, false, false, FSAL_O_WRITE,
                             (struct fsal_fd *)&handle->fileDescriptor,
                             &handle->share, openFunction, closeFunction,
                             (struct fsal_fd **)&fileDescriptor,
                             &hasLock, &closeFd);

    if (!FSAL_IS_ERROR(status)) {
        int rc = fs_fsync(export->fsInstance, &op_ctx->creds,
                          fileDescriptor->fileDescriptor);

        if (rc < 0) {
            status = fsalLastError();
        }
    }

    if (closeFd) {
        int rc = liz_release(export->fsInstance, fileDescriptor->fileDescriptor);

        if (rc < 0) {
            status = fsalLastError();
        }
    }

    if (hasLock) {
        PTHREAD_RWLOCK_unlock(&objectHandle->obj_lock);
    }

    return status;
}

/*! \brief Set attributes on an object
 *
 * \see fsal_api.h for more information
 */
static fsal_status_t _setattr2(struct fsal_obj_handle *objectHandle,
                               bool bypass, struct state_t *state,
                               struct fsal_attrlist *attributes)
{
    struct FSExport *export;
    struct FSHandle *handle;

    bool hasLock = false;
    bool closeFd = false;
    fsal_status_t status;

    export = container_of(op_ctx->fsal_export, struct FSExport, export);
    handle = container_of(objectHandle, struct FSHandle, fileHandle);

    LogAttrlist(COMPONENT_FSAL, NIV_FULL_DEBUG, "attrs ", attributes, false);

    if (FSAL_TEST_MASK(attributes->valid_mask, ATTR_MODE)) {
        attributes->mode &=
          ~op_ctx->fsal_export->exp_ops.fs_umask(op_ctx->fsal_export);
    }

    if (FSAL_TEST_MASK(attributes->valid_mask, ATTR_SIZE)) {
        if (objectHandle->type != REGULAR_FILE) {
            LogFullDebug(COMPONENT_FSAL, "Setting size on non-regular file");
            return fsalstat(ERR_FSAL_INVAL, EINVAL);
        }

        bool canReuseOpenedFd = false;
        status = fsal_find_fd(NULL, objectHandle, NULL, &handle->share,
                              bypass, state, FSAL_O_RDWR, NULL, NULL,
                              &hasLock, &closeFd, false,
                              &canReuseOpenedFd);

        if (FSAL_IS_ERROR(status)) {
            LogFullDebug(COMPONENT_FSAL, "fsal_find_fd status = %s",
                         fsal_err_txt(status));
            goto out;
        }
    }

    struct stat posixAttributes;
    int mask = 0;
    memset(&posixAttributes, 0, sizeof(posixAttributes));

    if (FSAL_TEST_MASK(attributes->valid_mask, ATTR_SIZE)) {
        mask |= LIZ_SET_ATTR_SIZE;
        posixAttributes.st_size = attributes->filesize;
        LogFullDebug(COMPONENT_FSAL, "setting size to %lld",
                     (long long)posixAttributes.st_size);
    }

    if (FSAL_TEST_MASK(attributes->valid_mask, ATTR_MODE)) {
        mask |= LIZ_SET_ATTR_MODE;
        posixAttributes.st_mode = fsal2unix_mode(attributes->mode);
    }

    if (FSAL_TEST_MASK(attributes->valid_mask, ATTR_OWNER)) {
        mask |= LIZ_SET_ATTR_UID;
        posixAttributes.st_uid = attributes->owner;
    }

    if (FSAL_TEST_MASK(attributes->valid_mask, ATTR_GROUP)) {
        mask |= LIZ_SET_ATTR_GID;
        posixAttributes.st_gid = attributes->group;
    }

    if (FSAL_TEST_MASK(attributes->valid_mask, ATTR_ATIME)) {
        mask |= LIZ_SET_ATTR_ATIME;
        posixAttributes.st_atim = attributes->atime;
    }

    if (FSAL_TEST_MASK(attributes->valid_mask, ATTR_ATIME_SERVER)) {
        mask |= LIZ_SET_ATTR_ATIME_NOW;
    }

    if (FSAL_TEST_MASK(attributes->valid_mask, ATTR_MTIME)) {
        mask |= LIZ_SET_ATTR_MTIME;
        posixAttributes.st_mtim = attributes->mtime;
    }

    if (FSAL_TEST_MASK(attributes->valid_mask, ATTR_MTIME_SERVER)) {
        mask |= LIZ_SET_ATTR_MTIME_NOW;
    }

    liz_attr_reply_t reply;
    int rc = fs_setattr(export->fsInstance, &op_ctx->creds, handle->inode,
                        &posixAttributes, mask, &reply);

    if (rc < 0) {
        status = fsalLastError();
        goto out;
    }

#ifdef ENABLE_NFS_ACL_SUPPORT
    if (FSAL_TEST_MASK(attributes->valid_mask, ATTR_ACL)) {
        status = setACL(export, handle->inode, attributes->acl,
                        reply.attr.st_mode);
    }
#endif

out:
    if (hasLock) {
        PTHREAD_RWLOCK_unlock(&objectHandle->obj_lock);
    }

    return status;
}

/*! \brief Manage closing a file when a state is no longer needed.
 *
 * \see fsal_api.h for more information
 */
static fsal_status_t _close2(struct fsal_obj_handle *objectHandle,
                             struct state_t *state)
{
    struct FSHandle *handle;
    handle = container_of(objectHandle, struct FSHandle, fileHandle);

    LogFullDebug(COMPONENT_FSAL, "export = %" PRIu16 " inode = %" PRIu32,
                 handle->uniqueKey.exportId, handle->inode);

    if (state->state_type == STATE_TYPE_SHARE ||
        state->state_type == STATE_TYPE_NLM_SHARE ||
        state->state_type == STATE_TYPE_9P_FID) {
        // This is a share state, we must update the share counters
        // This can block over an I/O operation
        PTHREAD_RWLOCK_wrlock(&objectHandle->obj_lock);

        update_share_counters(&handle->share, handle->fileDescriptor.openFlags,
                              FSAL_O_CLOSED);

        PTHREAD_RWLOCK_unlock(&objectHandle->obj_lock);
    }

    return closeFileDescriptor(handle, &handle->fileDescriptor);
}

/*! \brief Create a symbolic link
 *
 * \see fsal_api.h for more information
 */
static fsal_status_t _symlink(struct fsal_obj_handle *directoryHandle,
                              const char *name,
                              const char *symbolicLinkPath,
                              struct fsal_attrlist *attributesToSet,
                              struct fsal_obj_handle **createdObject,
                              struct fsal_attrlist *attributes)
{
    struct FSExport *export;
    struct FSHandle *directory;
    struct liz_entry entry;
    int rc;

    export = container_of(op_ctx->fsal_export, struct FSExport, export);
    directory = container_of(directoryHandle, struct FSHandle, fileHandle);

    LogFullDebug(COMPONENT_FSAL, "export = %" PRIu16 " parent_inode = %"
                 PRIu32 " name = %s", export->export.export_id,
                 directory->inode, name);

    rc = fs_symlink(export->fsInstance, &op_ctx->creds, symbolicLinkPath,
                    directory->inode, name, &entry);
    if (rc < 0) {
        return fsalLastError();
    }

    struct FSHandle *handle = allocateNewHandle(&entry.attr, export);
    *createdObject = &handle->fileHandle;

    // We handled the mode above
    FSAL_UNSET_MASK(attributesToSet->valid_mask, ATTR_MODE);

    if (attributesToSet->valid_mask) {
        fsal_status_t status;

        // Now per support_ex API, if there are any other attributes set,
        // go ahead and get them set now
        status = (*createdObject)->obj_ops->setattr2(*createdObject, false,
                                                     NULL, attributesToSet);

        if (FSAL_IS_ERROR(status)) {
            // Release the handle we just allocated
            LogFullDebug(COMPONENT_FSAL, "setattr2 status = %s",
                         fsal_err_txt(status));

            (*createdObject)->obj_ops->release(*createdObject);
            *createdObject = NULL;
        }
    }
    else if (attributes != NULL) {
        posix2fsal_attributes_all(&entry.attr, attributes);
    }

    FSAL_SET_MASK(attributesToSet->valid_mask, ATTR_MODE);
    return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

fsal_status_t _lock_op2(struct fsal_obj_handle *objectHandle,
                        struct state_t *state,
                        void *owner, fsal_lock_op_t lockOperation,
                        fsal_lock_param_t *requestedLock,
                        fsal_lock_param_t *conflictingLock)
{
    struct FSExport *export;

    liz_err_t lastError;
    fileinfo_t *fileinfo;
    liz_lock_info_t lockInfo;

    fsal_status_t status = {0, 0};
    int retval = 0;

    struct FSFileDescriptor fileDescriptor;
    fsal_openflags_t openflags = FSAL_O_RDWR;

    bool hasLock = false;
    bool closeFd = false;
    bool bypass = false;

    export = container_of(op_ctx->fsal_export, struct FSExport, export);

    LogFullDebug(COMPONENT_FSAL, "op:%d type:%d start:%" PRIu64 " length:%"
                 PRIu64 " ", lockOperation, requestedLock->lock_type,
                 requestedLock->lock_start, requestedLock->lock_length);

    // Sanity checks
    if (objectHandle == NULL) {
        LogCrit(COMPONENT_FSAL, "objectHandle arg is NULL.");
        return fsalstat(ERR_FSAL_FAULT, 0);
    }

    if (owner == NULL) {
        LogCrit(COMPONENT_FSAL, "owner arg is NULL.");
        return fsalstat(ERR_FSAL_FAULT, 0);
    }

    if (lockOperation == FSAL_OP_LOCKT) {
        // We may end up using global fd, don't fail on a deny mode
        bypass = true;
        openflags = FSAL_O_ANY;
    }
    else if (lockOperation == FSAL_OP_LOCK) {
        if (requestedLock->lock_type == FSAL_LOCK_R)
            openflags = FSAL_O_READ;
        else if (requestedLock->lock_type == FSAL_LOCK_W)
            openflags = FSAL_O_WRITE;
    }
    else if (lockOperation == FSAL_OP_UNLOCK) {
        openflags = FSAL_O_ANY;
    }
    else {
        LogFullDebug(COMPONENT_FSAL, "ERROR: Lock operation requested was not "
                     "TEST, READ, or WRITE.");

        return fsalstat(ERR_FSAL_NOTSUPP, 0);
    }

    if (lockOperation != FSAL_OP_LOCKT && state == NULL) {
        LogCrit(COMPONENT_FSAL, "Non TEST operation with NULL state");
        return posix2fsal_status(EINVAL);
    }

    if (requestedLock->lock_type == FSAL_LOCK_R) {
        lockInfo.l_type = F_RDLCK;
    }
    else if (requestedLock->lock_type == FSAL_LOCK_W) {
        lockInfo.l_type = F_WRLCK;
    }
    else {
        LogFullDebug(COMPONENT_FSAL,
                     "ERROR: The requested lock type was not read or write.");

        return fsalstat(ERR_FSAL_NOTSUPP, 0);
    }

    if (lockOperation == FSAL_OP_UNLOCK)
        lockInfo.l_type = F_UNLCK;

    lockInfo.l_pid = 0;
    lockInfo.l_len = requestedLock->lock_length;
    lockInfo.l_start = requestedLock->lock_start;

    // Get a usable file descriptor
    status = findFileDescriptor(&fileDescriptor, objectHandle, bypass, state,
                                openflags, &hasLock, &closeFd, true);

    // IF find_fd returned DELAY, then fd caching in mdcache is
    // turned off, which means that the consecutive attempt is very likely
    // to succeed immediately.
    if (status.major == ERR_FSAL_DELAY) {
        status = findFileDescriptor(&fileDescriptor, objectHandle, bypass,
                                    state, openflags, &hasLock, &closeFd, true);
    }

    if (FSAL_IS_ERROR(status)) {
        LogCrit(COMPONENT_FSAL, "Unable to find fd for lock operation");

        return status;
    }

    fileinfo = fileDescriptor.fileDescriptor;
    liz_set_lock_owner(fileinfo, (uint64_t)owner);

    if (lockOperation == FSAL_OP_LOCKT) {
        retval = fs_getlk(export->fsInstance, &op_ctx->creds,
                          fileinfo, &lockInfo);
    }
    else {
        retval = fs_setlk(export->fsInstance, &op_ctx->creds,
                          fileinfo, &lockInfo);
    }

    if (retval < 0) {
        goto err;
    }

    /* F_UNLCK is returned then the tested operation would be possible. */
    if (conflictingLock != NULL) {
        if (lockOperation == FSAL_OP_LOCKT && lockInfo.l_type != F_UNLCK) {
            conflictingLock->lock_length = lockInfo.l_len;
            conflictingLock->lock_start = lockInfo.l_start;
            conflictingLock->lock_type = lockInfo.l_type;
        }
        else {
            conflictingLock->lock_length = 0;
            conflictingLock->lock_start = 0;
            conflictingLock->lock_type = FSAL_NO_LOCK;
        }
    }

err:
    lastError = liz_last_err();

    if (closeFd) {
        liz_release(export->fsInstance, fileinfo);
    }

    if (hasLock) {
        PTHREAD_RWLOCK_unlock(&objectHandle->obj_lock);
    }

    if (retval < 0) {
        LogFullDebug(COMPONENT_FSAL, "Returning error %d", lastError);
        return lizardfsToFsalError(lastError);
    }

    return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/*! \brief Re-open a file that may be already opened
 *
 * \see fsal_api.h for more information
 */
static fsal_status_t _reopen2(struct fsal_obj_handle *objectHandle,
                              struct state_t *state,
                              fsal_openflags_t openflags)
{
    struct FSHandle *handle;
    struct FSFileDescriptor *sharedFileDescriptor;
    fsal_status_t status;

    handle = container_of(objectHandle, struct FSHandle, fileHandle);

    sharedFileDescriptor = &container_of(state, struct FSFileDescriptorState,
                                         state)->fileDescriptor;

    LogFullDebug(COMPONENT_FSAL, "export = %" PRIu16 " inode = %" PRIu32,
                 handle->uniqueKey.exportId, handle->inode);

    struct FSFileDescriptor fileDescriptor = { FSAL_O_CLOSED, NULL };

    // This can block over an I/O operation
    PTHREAD_RWLOCK_wrlock(&objectHandle->obj_lock);

    fsal_openflags_t oldOpenflags = sharedFileDescriptor->openFlags;

    // We can conflict with old share, so go ahead and check now
    status = check_share_conflict(&handle->share, openflags, false);

    if (FSAL_IS_ERROR(status)) {
        PTHREAD_RWLOCK_unlock(&objectHandle->obj_lock);
        return status;
    }

    // Set up the new share so we can drop the lock and not have a
    // conflicting share be asserted, updating the share counters
    update_share_counters(&handle->share, oldOpenflags, openflags);

    PTHREAD_RWLOCK_unlock(&objectHandle->obj_lock);

    status = openFileDescriptor(handle, openflags, &fileDescriptor, true);

    if (!FSAL_IS_ERROR(status)) {
        closeFileDescriptor(handle, sharedFileDescriptor);
        *sharedFileDescriptor = fileDescriptor;
    }
    else {
        // We had a failure on open - we need to revert the share.
        // This can block over an I/O operation
        PTHREAD_RWLOCK_wrlock(&objectHandle->obj_lock);

        update_share_counters(&handle->share, openflags, oldOpenflags);

        PTHREAD_RWLOCK_unlock(&objectHandle->obj_lock);
    }

    return status;
}

/*! \brief Create a special file
 *
 * \see fsal_api.h for more information
 */
static fsal_status_t _mknode(struct fsal_obj_handle *directoryHandle,
                             const char *name,
                             object_file_type_t nodeType,
                             struct fsal_attrlist *attributesToSet,
                             struct fsal_obj_handle **createdObject,
                             struct fsal_attrlist *attributes)
{
    struct FSExport *export;
    struct FSHandle *directory, *handle;
    struct liz_entry entry;
    mode_t unixMode;
    dev_t unixDev = 0;

    export = container_of(op_ctx->fsal_export, struct FSExport, export);
    directory = container_of(directoryHandle, struct FSHandle, fileHandle);

    LogFullDebug(COMPONENT_FSAL,
                 "export = %" PRIu16 " parent_inode = %" PRIu32 " mode = %"
                 PRIo32 " name = %s", export->export.export_id,
                 directory->inode, attributesToSet->mode, name);

    unixMode = fsal2unix_mode(attributesToSet->mode) &
        ~op_ctx->fsal_export->exp_ops.fs_umask(op_ctx->fsal_export);

    switch (nodeType) {
    case BLOCK_FILE:
        unixMode |= S_IFBLK;
        unixDev = makedev(attributesToSet->rawdev.major, attributesToSet->rawdev.minor);
        break;
    case CHARACTER_FILE:
        unixMode |= S_IFCHR;
        unixDev = makedev(attributesToSet->rawdev.major, attributesToSet->rawdev.minor);
        break;
    case FIFO_FILE:
        unixMode |= S_IFIFO;
        break;
    case SOCKET_FILE:
        unixMode |= S_IFSOCK;
        break;
    default:
        LogMajor(COMPONENT_FSAL,
                 "Invalid node type in FSAL_mknode: %d", nodeType);

        return fsalstat(ERR_FSAL_INVAL, EINVAL);
    }

    int rc = fs_mknode(export->fsInstance, &op_ctx->creds, directory->inode,
                       name, unixMode, unixDev, &entry);

    if (rc < 0) {
        return fsalLastError();
    }

    handle = allocateNewHandle(&entry.attr, export);
    *createdObject = &handle->fileHandle;

    // We handled the mode above
    FSAL_UNSET_MASK(attributesToSet->valid_mask, ATTR_MODE);

    if (attributesToSet->valid_mask) {
        fsal_status_t status;
        // Setting attributes for the created object
        status = (*createdObject)->obj_ops->setattr2(*createdObject, false,
                                                     NULL, attributesToSet);

        if (FSAL_IS_ERROR(status)) {
            LogFullDebug(COMPONENT_FSAL, "setattr2 status = %s",
                         fsal_err_txt(status));

            // Release the handle we just allocated
            (*createdObject)->obj_ops->release(*createdObject);
            *createdObject = NULL;
        }
    }
    else if (attributes != NULL) {
        // Since we haven't set any attributes other than what was set on create,
        // just use the stat results we used to create the fsal_obj_handle
        posix2fsal_attributes_all(&entry.attr, attributes);
    }

    FSAL_SET_MASK(attributesToSet->valid_mask, ATTR_MODE);
    return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/*! \brief Read the content of a link
 *
 * \see fsal_api.h for more information
 */
static fsal_status_t _readlink(struct fsal_obj_handle *objectHandle,
                               struct gsh_buffdesc *linkContent,
                               bool refresh)
{
    struct FSExport *export;
    struct FSHandle *handle;
    char result[LIZARDFS_MAX_READLINK_LENGTH];

    // Sanity check
    if (objectHandle->type != SYMBOLIC_LINK)
        return fsalstat(ERR_FSAL_FAULT, 0);

    export = container_of(op_ctx->fsal_export, struct FSExport, export);
    handle = container_of(objectHandle, struct FSHandle, fileHandle);

    LogFullDebug(COMPONENT_FSAL, "export = %" PRIu16 " inode = %" PRIu32,
                 export->export.export_id, handle->inode);

    int size = fs_readlink(export->fsInstance, &op_ctx->creds, handle->inode,
                           result, LIZARDFS_MAX_READLINK_LENGTH);

    // fs_readlink() returns the size of the read link if succeed.
    // Otherwise returns -1 to indicate an error occurred
    if (size < 0) {
        return fsalLastError();
    }

    size = MIN(size, LIZARDFS_MAX_READLINK_LENGTH);
    linkContent->addr = gsh_strldup(result, size, &linkContent->len);

    return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/*! \brief Return open status of a state.
 *
 * \see fsal_api.h for more information
 */
static fsal_openflags_t _status2(struct fsal_obj_handle *objectHandle,
                                 struct state_t *state)
{
    struct FSFileDescriptor *fileDescriptor;
    fileDescriptor = &container_of(state, struct FSFileDescriptorState,
                                   state)->fileDescriptor;

    return fileDescriptor->openFlags;
}

/*! \brief Merge a duplicate handle with an original handle
 *
 * \see fsal_api.h for more information
 */
static fsal_status_t _merge(struct fsal_obj_handle *originalHandle,
                            struct fsal_obj_handle *toMergeHandle)
{
    fsal_status_t status = fsalstat(ERR_FSAL_NO_ERROR, 0);

    if (originalHandle->type == REGULAR_FILE &&
        toMergeHandle->type == REGULAR_FILE)
    {
        // We need to merge the share reservations on this file.
        // This could result in ERR_FSAL_SHARE_DENIED.
        struct FSHandle *original, *toMerge;

        original = container_of(originalHandle, struct FSHandle, fileHandle);
        toMerge = container_of(toMergeHandle, struct FSHandle, fileHandle);

        // This can block over an I/O operation
        status = merge_share(originalHandle, &original->share, &toMerge->share);
    }

    return status;
}

/*! \brief Get Extended Attribute.
 *
 *  This function gets an extended attribute of an object.
 *
 *  \param [in]     objectHandle       Input object to query
 *  \param [in]     xattributeName     Input extended attribute name
 *  \param [out]    xattributeValue    Output extended attribute value
 *
 *  \returns: FSAL status
 */
static fsal_status_t _getxattrs(struct fsal_obj_handle *objectHandle,
                                xattrkey4 *xattributeName,
                                xattrvalue4 *xattributeValue)
{
    struct FSExport *export;
    export = container_of(op_ctx->fsal_export, struct FSExport, export);

    struct FSHandle *handle;
    handle = container_of(objectHandle, struct FSHandle, fileHandle);

    size_t curr_size = 0;
    int rc = fs_getxattr(export->fsInstance, &op_ctx->creds,
                         handle->inode, xattributeName->utf8string_val,
                         xattributeValue->utf8string_len, &curr_size,
                         (uint8_t *)xattributeValue->utf8string_val);

    if (rc < 0) {
        LogFullDebug(COMPONENT_FSAL, "GETXATTRS failed returned rc = %d ", rc);
        return lizardfsToFsalError(rc);
    }

    if (curr_size && curr_size <= xattributeValue->utf8string_len) {
        // Updating the real size
        xattributeValue->utf8string_len = curr_size;
        // Make sure utf8string is NUL terminated
        xattributeValue->utf8string_val[curr_size] = '\0';
    }

    return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/*! \brief Set Extended Attribute.
 *
 *  This function gets an extended attribute of an object.
 *
 *  \param [in]     objectHandle        Input object to set
 *  \param [in]     option              Input extended attribute type
 *  \param [in]     xattributeName      Input extended attribute name to set
 *  \param [in]     xattributeValue     Input extended attribute value to set
 *
 *  \returns: FSAL status
 */
static fsal_status_t _setxattrs(struct fsal_obj_handle *objectHandle,
                                setxattr_option4 option,
                                xattrkey4 *xattributeName,
                                xattrvalue4 *xattributeValue)
{
    struct FSExport *export;
    export = container_of(op_ctx->fsal_export, struct FSExport, export);

    struct FSHandle *handle;
    handle = container_of(objectHandle, struct FSHandle, fileHandle);

    int rc = fs_setxattr(export->fsInstance, &op_ctx->creds,
                         handle->inode, xattributeName->utf8string_val,
                         (const uint8_t *)xattributeValue->utf8string_val,
                         xattributeValue->utf8string_len, option);

    if (rc < 0) {
        LogDebug(COMPONENT_FSAL, "SETXATTRS returned rc %d", rc);
        return lizardfsToFsalError(rc);
    }

    return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/*! \brief List Extended Attributes.
 *
 *  This function lists the extended attributes of an object.
 *
 *  \param [in]     objectHandle        Input object to list
 *  \param [in]     maximumNameSize     Input maximum number of bytes for names
 *  \param [in,out] cookie              In/out cookie
 *  \param [out]    eof                 Output eof set if no more extended attributes
 *  \param [out]    xattributesNames    Output list of extended attribute names this
 *                                      buffer size is double the size of maximumNameSize
 *                                      to allow for component4 overhead
 *
 *  \returns: FSAL status
 */
static fsal_status_t _listxattrs(struct fsal_obj_handle *objectHandle,
                                 count4 maximumNameSize,
                                 nfs_cookie4 *cookie,
                                 bool_t *eof,
                                 xattrlist4 *xattributesNames)
{
    char *buffer = NULL;
    fsal_status_t status;

    struct FSExport *export;
    export = container_of(op_ctx->fsal_export, struct FSExport, export);

    struct FSHandle *handle;
    handle = container_of(objectHandle, struct FSHandle, fileHandle);

    LogFullDebug(COMPONENT_FSAL, "in cookie %llu length %d",
                 (unsigned long long)*cookie, maximumNameSize);

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
    if (curr_size && curr_size < maximumNameSize) {
        buffer = gsh_malloc(curr_size);

        // Second time, the function is called to retrieve the xattr list
        rc = fs_listxattr(export->fsInstance, &op_ctx->creds, handle->inode,
                          curr_size, &curr_size, buffer);

        if (rc < 0) {
            LogDebug(COMPONENT_FSAL, "LISTXATTRS returned rc %d", rc);
            gsh_free(buffer);
            return lizardfsToFsalError(rc);
        }

        // Setting retrieved extended attributes to Ganesha
        status = fsal_listxattr_helper(buffer, curr_size, maximumNameSize,
                                       cookie, eof, xattributesNames);

        // Releasing allocated buffer
        gsh_free(buffer);
    }

    return status;
}

/*! \brief Remove Extended Attribute.
 *
 *  This function remove an extended attribute of an object.
 *
 *  \param [in] objectHandle       Input object to set
 *  \param [in] xattributeName     Input xattr name to remove
 *
 *  \returns: FSAL status
 */
static fsal_status_t _removexattrs(struct fsal_obj_handle *objectHandle,
                                   xattrkey4 *xattributeName)
{
    struct FSExport *export;
    export = container_of(op_ctx->fsal_export, struct FSExport, export);

    struct FSHandle *handle;
    handle = container_of(objectHandle, struct FSHandle, fileHandle);

    int rc = fs_removexattr(export->fsInstance, &op_ctx->creds, handle->inode,
                            xattributeName->utf8string_val);

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

struct FSHandle *allocateNewHandle(const struct stat *attribute,
                                   struct FSExport *export)
{
    struct FSHandle *result = NULL;
    result = gsh_calloc(1, sizeof(struct FSHandle));

    result->inode = attribute->st_ino;
    result->uniqueKey.moduleId = FSAL_ID_LIZARDFS;
    result->uniqueKey.exportId = export->export.export_id;
    result->uniqueKey.inode = attribute->st_ino;

    fsal_obj_handle_init(&result->fileHandle, &export->export,
                         posix2fsal_type(attribute->st_mode));

    result->fileHandle.obj_ops = &LizardFS.operations;
    result->fileHandle.fsid = posix2fsal_fsid(attribute->st_dev);
    result->fileHandle.fileid = attribute->st_ino;
    result->export = export;
    return result;
}

void deleteHandle(struct FSHandle *object)
{
    fsal_obj_handle_fini(&object->fileHandle);
    gsh_free(object);
}
