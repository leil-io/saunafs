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
#include "fsal_api.h"
#include "fsal_convert.h"
#include "fsal_types.h"
#include "fsal_up.h"
#include "FSAL/fsal_commonlib.h"
#include "../FSAL/fsal_private.h"
#include "nfs_exports.h"
#include "pnfs_utils.h"

#include "context_wrap.h"
#include "lzfs_fsal_types.h"
#include "lzfs_fsal_methods.h"

static void clearFileinfoCache(struct FSExport *export, int count)
{
    // Sanity Check
    if (export == NULL)
        return;

    for (int i = 0; i < count; ++i) {
        liz_fileinfo_entry_t *cacheHandle;
	    fileinfo_t *fileHandle;

        cacheHandle = liz_fileinfo_cache_pop_expired(export->fileinfoCache);
        if (cacheHandle == NULL) {
            break;
        }

        fileHandle = liz_extract_fileinfo(cacheHandle);
        liz_release(export->fsInstance, fileHandle);
        liz_fileinfo_entry_free(cacheHandle);
    }
}

/*! \brief Clean up a DS handle
 *
 * \see fsal_api.h for more information
 */
static void _dsh_release(struct fsal_ds_handle *const dataServerHandle)
{
    struct FSExport *export;
    struct DataServerHandle *dataServer;

    export = container_of(op_ctx->ctx_pnfs_ds->mds_fsal_export,
                          struct FSExport, export);

<<<<<<< HEAD
=======
    dataServer = container_of(dataServerHandle, struct DataServerHandle,
                              dsHandle);

>>>>>>> 1ce8dfee (refactor(nfs-ganesha): Refactor remaining code)
    assert(export->fileinfoCache);

    if (dataServer->cacheHandle != NULL) {
        liz_fileinfo_cache_release(export->fileinfoCache,
                                   dataServer->cacheHandle);
    }

    gsh_free(dataServer);
    clearFileinfoCache(export, 5);
}

static nfsstat4 openfile(struct FSExport *export,
                         struct DataServerHandle *dataServer)
{
    // Sanity Check
    if (export == NULL)
        return NFS4ERR_IO;

    if (dataServer->cacheHandle != NULL) {
        return NFS4_OK;
    }

    clearFileinfoCache(export, 2);

    struct liz_fileinfo_entry *entry;
    entry = liz_fileinfo_cache_acquire(export->fileinfoCache, dataServer->inode);

    dataServer->cacheHandle = entry;
    if (dataServer->cacheHandle == NULL) {
        return NFS4ERR_IO;
    }

	fileinfo_t *file_handle = liz_extract_fileinfo(dataServer->cacheHandle);
    if (file_handle != NULL) {
        return NFS4_OK;
    }

    file_handle = fs_open(export->fsInstance, NULL, dataServer->inode, O_RDWR);

    if (file_handle == NULL) {
        liz_fileinfo_cache_erase(export->fileinfoCache, dataServer->cacheHandle);
        dataServer->cacheHandle = NULL;
        return NFS4ERR_IO;
    }

    liz_attach_fileinfo(dataServer->cacheHandle, file_handle);
    return NFS4_OK;
}

/*! \brief Read from a data-server handle.
 *
 * \see fsal_api.h for more information
 */
static nfsstat4 _dsh_read(struct fsal_ds_handle *const dataServerHandle,
                          const stateid4 *stateid,
                          const offset4 offset,
                          const count4 requestedLength,
                          void *const buffer,
                          count4 *const suppliedLength,
                          bool *const eof)
{
    // Unused variable
    (void ) stateid;

    struct FSExport *export;
    struct DataServerHandle *dataServer;
	fileinfo_t *fileHandle;

    export = container_of(op_ctx->ctx_pnfs_ds->mds_fsal_export,
                          struct FSExport, export);

    dataServer = container_of(dataServerHandle, struct DataServerHandle,
                              dsHandle);

	LogFullDebug(COMPONENT_FSAL,
                 "export=%" PRIu16 " inode=%" PRIu32 " offset=%" PRIu64
                 " size=%" PRIu32, export->export.export_id,
                 dataServer->inode, offset, requestedLength);

    nfsstat4 nfsStatus = openfile(export, dataServer);

    if (nfsStatus != NFS4_OK) {
        return nfsStatus;
    }

    fileHandle = liz_extract_fileinfo(dataServer->cacheHandle);
    ssize_t bytes = fs_read(export->fsInstance, NULL, fileHandle,
                            offset, requestedLength, buffer);

    if (bytes < 0) {
        return Nfs4LastError();
    }

    *suppliedLength = bytes;
    *eof = (bytes == 0);

    return NFS4_OK;
}

/*! \brief Write to a data-server handle.
 *
 * \see fsal_api.h for more information
 */
static nfsstat4 _dsh_write(struct fsal_ds_handle *const dataServerHandle,
                           const stateid4 *stateid,
                           const offset4 offset,
                           const count4 writeLength,
                           const void *buffer,
                           const stable_how4 stability,
                           count4 *const writtenLength,
                           verifier4 *const writeVerifier,
                           stable_how4 *const stabilityGot)
{
    // Unused variables
    (void ) stateid;
    (void ) writeVerifier;

    struct FSExport *export;
    struct DataServerHandle *dataServer;

	fileinfo_t *fileHandle;
    int rc = 0;

    export = container_of(op_ctx->ctx_pnfs_ds->mds_fsal_export,
                          struct FSExport, export);

    dataServer = container_of(dataServerHandle, struct DataServerHandle,
                              dsHandle);

	LogFullDebug(COMPONENT_FSAL,
                 "export=%" PRIu16 " inode=%" PRIu32 " offset=%" PRIu64
                 " size=%" PRIu32, export->export.export_id,
                  dataServer->inode, offset, writeLength);

    nfsstat4 nfsStatus = openfile(export, dataServer);
    if (nfsStatus != NFS4_OK) {
        return nfsStatus;
    }

    fileHandle = liz_extract_fileinfo(dataServer->cacheHandle);
    ssize_t bytes = fs_write(export->fsInstance, NULL, fileHandle,
                             offset, writeLength, buffer);

    if (bytes < 0) {
        return Nfs4LastError();
    }

    if (stability != UNSTABLE4) {
        rc = fs_flush(export->fsInstance, NULL, fileHandle);
    }

    *writtenLength = bytes;
    *stabilityGot = (rc < 0) ? UNSTABLE4 : stability;

    return NFS4_OK;
}

/*! \brief Commit a byte range to a DS handle.
 *
 * \see fsal_api.h for more information
 */
static nfsstat4 _dsh_commit(struct fsal_ds_handle *const dataServerHandle,
                            const offset4 offset,
                            const count4 count,
                            verifier4 *const writeVerifier)
{
    struct FSExport *export;
    struct DataServerHandle *dataServer;
	fileinfo_t *fileHandle;

    memset(writeVerifier, 0, NFS4_VERIFIER_SIZE);

    export = container_of(op_ctx->ctx_pnfs_ds->mds_fsal_export,
                          struct FSExport, export);

    dataServer = container_of(dataServerHandle, struct DataServerHandle,
                              dsHandle);

    LogFullDebug(COMPONENT_FSAL,
                 "export=%" PRIu16 " inode=%" PRIu32 " offset=%" PRIu64
                 " size=%" PRIu32, export->export.export_id,
                 dataServer->inode, offset, count);

    nfsstat4 nfsStatus = openfile(export, dataServer);

    if (nfsStatus != NFS4_OK) {
        // If we failed here then there is no opened LizardFS file descriptor,
        // which implies that we don't need to flush anything
        return NFS4_OK;
    }

    fileHandle = liz_extract_fileinfo(dataServer->cacheHandle);
    int rc = fs_flush(export->fsInstance, NULL, fileHandle);

    if (rc < 0) {
        LogMajor(COMPONENT_PNFS, "ds_commit() failed  '%s'",
                 liz_error_string(liz_last_err()));
        return NFS4ERR_INVAL;
    }

    return NFS4_OK;
}

/*! \brief Read plus from a data-server handle.
 *
 * \see fsal_api.h for more information
 */
static nfsstat4 _dsh_read_plus(struct fsal_ds_handle *const dataServerHandle,
                               const stateid4 *stateid,
                               const offset4 offset,
                               const count4 requestedLength,
                               void *const buffer,
                               const count4 suppliedLength,
                               bool *const eof,
                               struct io_info *info)
{
    // Unused variables
    (void ) dataServerHandle;
    (void ) stateid;
    (void ) offset;
    (void ) requestedLength;
    (void ) buffer;
    (void ) suppliedLength;
    (void ) eof;
    (void ) info;

    LogCrit(COMPONENT_PNFS, "Unimplemented DS read_plus!");
    return NFS4ERR_NOTSUPP;
}

/*! \brief Create a FSAL data server handle from a wire handle
 *
 * \see fsal_api.h for more information
 */
static nfsstat4 _make_ds_handle(struct fsal_pnfs_ds *const pnfsDataServer,
                                const struct gsh_buffdesc *const bufferDescriptor,
                                struct fsal_ds_handle **const handle,
                                int flags)
{
    // Unused variable
    (void ) pnfsDataServer;

    struct DataServerWire *dataServerWire;
    dataServerWire = (struct DataServerWire *)bufferDescriptor->addr;

    struct DataServerHandle *dataServer;

    *handle = NULL;

    if (bufferDescriptor->len != sizeof(struct DataServerWire))
        return NFS4ERR_BADHANDLE;

    if (dataServerWire->inode == 0)
        return NFS4ERR_BADHANDLE;

    dataServer = gsh_calloc(1, sizeof(struct DataServerHandle));
    *handle = &dataServer->dsHandle;

    if (flags & FH_FSAL_BIG_ENDIAN) {
#if (BYTE_ORDER != BIG_ENDIAN)
        dataServer->inode = bswap_32(dataServerWire->inode);
#else
        dataServer->inode = dataServerWire->inode;
#endif
    }
    else {
#if (BYTE_ORDER == BIG_ENDIAN)
        dataServer->inode = bswap_32(dataServerWire->inode);
#else
        dataServer->inode = dataServerWire->inode;
#endif
    }

    return NFS4_OK;
}

/*! \brief Initialize FSAL specific permissions per pNFS DS
 *
 * \see fsal_api.h for more information
 */
static nfsstat4 _ds_permissions(struct fsal_pnfs_ds *const pnfsDataServer,
                                struct svc_req *request)
{
    // Unused variables
    (void ) pnfsDataServer;
    (void ) request;

    op_ctx->export_perms.set = root_op_export_set;
    op_ctx->export_perms.options = root_op_export_options;
    return NFS4_OK;
}

void initializeDataServerOperations(struct fsal_pnfs_ds_ops *ops)
{
    memcpy(ops, &def_pnfs_ds_ops, sizeof(struct fsal_pnfs_ds_ops));
    ops->make_ds_handle = _make_ds_handle;
    ops->dsh_release = _dsh_release;
    ops->dsh_read = _dsh_read;
    ops->dsh_write = _dsh_write;
    ops->dsh_commit = _dsh_commit;
    ops->dsh_read_plus = _dsh_read_plus;
    ops->ds_permissions = _ds_permissions;
}
