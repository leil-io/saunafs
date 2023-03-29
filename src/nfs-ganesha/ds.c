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

static void lzfs_int_clear_fileinfo_cache(struct FSExport *export,
                                          int count)
{
    // Sanity Check
    if (export == NULL)
        return;

    for (int i = 0; i < count; ++i) {
        liz_fileinfo_entry_t *cache_handle;
        liz_fileinfo_t *file_handle;

        cache_handle = liz_fileinfo_cache_pop_expired(export->fileinfoCache);
        if (cache_handle == NULL) {
            break;
        }

        file_handle = liz_extract_fileinfo(cache_handle);
        liz_release(export->fsInstance, file_handle);
        liz_fileinfo_entry_free(cache_handle);
    }
}

/*! \brief Clean up a DS handle
 *
 * \see fsal_api.h for more information
 */
static void lzfs_fsal_ds_handle_release(struct fsal_ds_handle *const ds_pub)
{
    struct FSExport *export;
    struct DataServerHandle *dataServer;

    export = container_of(op_ctx->ctx_pnfs_ds->mds_fsal_export,
                          struct FSExport, export);
    dataServer = container_of(ds_pub, struct DataServerHandle, dsHandle);

    assert(export->fileinfoCache);

    if (dataServer->cacheHandle != NULL) {
        liz_fileinfo_cache_release(export->fileinfoCache,
                                   dataServer->cacheHandle);
    }

    gsh_free(dataServer);
    lzfs_int_clear_fileinfo_cache(export, 5);
}

static nfsstat4 lzfs_int_openfile(struct FSExport *export,
                                  struct DataServerHandle *dataServer)
{
    // Sanity Check
    if (export == NULL)
        return NFS4ERR_IO;

    if (dataServer->cacheHandle != NULL) {
		return NFS4_OK;
	}

    lzfs_int_clear_fileinfo_cache(export, 2);

    dataServer->cacheHandle = liz_fileinfo_cache_acquire(
                                    export->fileinfoCache,
                                    dataServer->inode);
    if (dataServer->cacheHandle == NULL) {
		return NFS4ERR_IO;
	}

    liz_fileinfo_t *file_handle = liz_extract_fileinfo(dataServer->cacheHandle);
	if (file_handle != NULL) {
		return NFS4_OK;
	}

    file_handle = fs_open(export->fsInstance, NULL,
                                dataServer->inode, O_RDWR);
	if (file_handle == NULL) {
        liz_fileinfo_cache_erase(export->fileinfoCache,
                                 dataServer->cacheHandle);
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
static nfsstat4 lzfs_fsal_ds_handle_read(struct fsal_ds_handle *const ds_hdl,
                                         const stateid4 *stateid,
                                         const offset4 offset,
                                         const count4 requested_length,
                                         void *const buffer,
                                         count4 *const supplied_length,
                                         bool *const end_of_file)
{
    // Unused variable
    (void ) stateid;

    struct FSExport *export;
    struct DataServerHandle *dataServer;
	liz_fileinfo_t *file_handle;

    export = container_of(op_ctx->ctx_pnfs_ds->mds_fsal_export,
                               struct FSExport, export);
    dataServer = container_of(ds_hdl, struct DataServerHandle, dsHandle);

	LogFullDebug(COMPONENT_FSAL,
                 "export=%" PRIu16 " inode=%" PRIu32 " offset=%" PRIu64
                 " size=%" PRIu32, export->export.export_id,
                 dataServer->inode, offset, requested_length);

    nfsstat4 nfs_status = lzfs_int_openfile(export, dataServer);
	if (nfs_status != NFS4_OK) {
		return nfs_status;
	}

    file_handle = liz_extract_fileinfo(dataServer->cacheHandle);
    ssize_t nb_read = fs_read(export->fsInstance, NULL, file_handle,
                                    offset, requested_length, buffer);

	if (nb_read < 0) {
        return Nfs4LastError();
    }

	*supplied_length = nb_read;
	*end_of_file = (nb_read == 0);

	return NFS4_OK;
}

/*! \brief Write to a data-server handle.
 *
 * \see fsal_api.h for more information
 */
static nfsstat4 lzfs_fsal_ds_handle_write(struct fsal_ds_handle *const ds_hdl,
                                          const stateid4 *stateid,
                                          const offset4 offset,
                                          const count4 write_length,
                                          const void *buffer,
                                          const stable_how4 stability_wanted,
                                          count4 *const written_length,
                                          verifier4 *const writeverf,
                                          stable_how4 *const stability_got)
{
    // Unused variables
    (void ) stateid;
    (void ) writeverf;

    struct FSExport *export;
    struct DataServerHandle *dataServer;
	liz_fileinfo_t *file_handle;
	ssize_t nb_write;
	nfsstat4 nfs_status;

    export = container_of(op_ctx->ctx_pnfs_ds->mds_fsal_export,
                          struct FSExport, export);

    dataServer = container_of(ds_hdl, struct DataServerHandle, dsHandle);

	LogFullDebug(COMPONENT_FSAL,
                 "export=%" PRIu16 " inode=%" PRIu32 " offset=%" PRIu64
                 " size=%" PRIu32, export->export.export_id,
                  dataServer->inode, offset, write_length);

    nfs_status = lzfs_int_openfile(export, dataServer);
	if (nfs_status != NFS4_OK) {
		return nfs_status;
	}

    file_handle = liz_extract_fileinfo(dataServer->cacheHandle);
    nb_write = fs_write(export->fsInstance, NULL,
                              file_handle, offset, write_length, buffer);

	if (nb_write < 0) {
        return Nfs4LastError();
    }

	int rc = 0;

    if (stability_wanted != UNSTABLE4) {
        rc = liz_cred_flush(export->fsInstance, NULL, file_handle);
    }

    *written_length = nb_write;
	*stability_got = (rc < 0) ? UNSTABLE4 : stability_wanted;

	return NFS4_OK;
}

/*! \brief Commit a byte range to a DS handle.
 *
 * \see fsal_api.h for more information
 */
static nfsstat4 lzfs_fsal_ds_handle_commit(struct fsal_ds_handle *const ds_hdl,
                                           const offset4 offset,
                                           const count4 count,
                                           verifier4 *const writeverf)
{
    struct FSExport *export;
    struct DataServerHandle *dataServer;
	liz_fileinfo_t *file_handle;
	nfsstat4 nfs_status;

    memset(writeverf, 0, NFS4_VERIFIER_SIZE);

    export = container_of(op_ctx->ctx_pnfs_ds->mds_fsal_export,
                          struct FSExport, export);
    dataServer = container_of(ds_hdl, struct DataServerHandle, dsHandle);

	LogFullDebug(COMPONENT_FSAL,
                 "export=%" PRIu16 " inode=%" PRIu32 " offset=%" PRIu64
                 " size=%" PRIu32, export->export.export_id,
                 dataServer->inode, offset, count);

    nfs_status = lzfs_int_openfile(export, dataServer);
	if (nfs_status != NFS4_OK) {
        // If we failed here then there is no opened LizardFS file descriptor,
        // which implies that we don't need to flush anything
        return NFS4_OK;
	}

    file_handle = liz_extract_fileinfo(dataServer->cacheHandle);

    int rc = liz_cred_flush(export->fsInstance, NULL, file_handle);
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
static nfsstat4 lzfs_fsal_ds_read_plus(struct fsal_ds_handle *const ds_hdl,
                                       const stateid4 *stateid,
                                       const offset4 offset,
                                       const count4 requested_length,
                                       void *const buffer,
                                       const count4 supplied_length,
                                       bool *const end_of_file,
                                       struct io_info *info)
{
    // Unused variables
    (void ) ds_hdl;
    (void ) stateid;
    (void ) offset;
    (void ) requested_length;
    (void ) buffer;
    (void ) supplied_length;
    (void ) end_of_file;
    (void ) info;

	LogCrit(COMPONENT_PNFS, "Unimplemented DS read_plus!");
	return NFS4ERR_NOTSUPP;
}


/*! \brief Create a FSAL data server handle from a wire handle
 *
 * \see fsal_api.h for more information
 */
static nfsstat4 lzfs_fsal_make_ds_handle(struct fsal_pnfs_ds *const pds,
                                         const struct gsh_buffdesc *const desc,
                                         struct fsal_ds_handle **const handle,
                                         int flags)
{
    // Unused variable
    (void ) pds;

    struct DataServerWire *dataServerWire = (struct DataServerWire *)desc->addr;
    struct DataServerHandle *dataServer;

    *handle = NULL;

    if (desc->len != sizeof(struct DataServerWire))
        return NFS4ERR_BADHANDLE;
    if (dataServerWire->inode == 0)
        return NFS4ERR_BADHANDLE;

    dataServer = gsh_calloc(1, sizeof(struct DataServerHandle));
    *handle = &dataServer->dsHandle;

    if (flags & FH_FSAL_BIG_ENDIAN) {
#if (BYTE_ORDER != BIG_ENDIAN)
        dataServer->inode = bswap_32(dataServerWire->inode);
#else
        dataServer->inode = dsw->inode;
#endif
    }
    else {
#if (BYTE_ORDER == BIG_ENDIAN)
        dataServer->inode = bswap_32(dsw->inode);
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
static nfsstat4 lzfs_fsal_pds_permissions(struct fsal_pnfs_ds *const pds,
                                          struct svc_req *req)
{
    // Unused variables
    (void ) pds;
    (void ) req;

    op_ctx->export_perms.set = root_op_export_set;
    op_ctx->export_perms.options = root_op_export_options;
	return NFS4_OK;
}

void initializeDataServerOperations(struct fsal_pnfs_ds_ops *ops)
{
    memcpy(ops, &def_pnfs_ds_ops, sizeof(struct fsal_pnfs_ds_ops));
    ops->make_ds_handle = lzfs_fsal_make_ds_handle;
    ops->dsh_release = lzfs_fsal_ds_handle_release;
    ops->dsh_read = lzfs_fsal_ds_handle_read;
    ops->dsh_write = lzfs_fsal_ds_handle_write;
    ops->dsh_commit = lzfs_fsal_ds_handle_commit;
    ops->dsh_read_plus = lzfs_fsal_ds_read_plus;
    ops->ds_permissions = lzfs_fsal_pds_permissions;
}
