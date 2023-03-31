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
#include "fsal_types.h"
#include "fsal_up.h"
#include "FSAL/fsal_commonlib.h"
#include "pnfs_utils.h"

#include "context_wrap.h"
#include "lzfs_fsal_methods.h"
#include "protocol/MFSCommunication.h"

/*! \brief Grant a layout segment.
 *
 * \see fsal_api.h for more information
 */
static nfsstat4 _layoutget(struct fsal_obj_handle *objectHandle,
                           XDR *xdrStream,
                           const struct fsal_layoutget_arg *arguments,
                           struct fsal_layoutget_res *output)
{
    struct FSHandle *handle;
    struct DataServerWire dataServerWire;

    struct gsh_buffdesc dsBufferDescriptor = {
            .addr = &dataServerWire,
            .len = sizeof(struct DataServerWire)
    };

    struct pnfs_deviceid deviceid = DEVICE_ID_INIT_ZERO(FSAL_ID_LIZARDFS);
    nfl_util4 layout_util = 0;
    nfsstat4 nfs_status = NFS4_OK;

    handle = container_of(objectHandle, struct FSHandle, fileHandle);

    if (arguments->type != LAYOUT4_NFSV4_1_FILES) {
        LogMajor(COMPONENT_PNFS, "Unsupported layout type: %x",
                 arguments->type);

        return NFS4ERR_UNKNOWN_LAYOUTTYPE;
    }

    LogDebug(COMPONENT_PNFS, "will issue layout offset: %" PRIu64
             " length: %" PRIu64, output->segment.offset,
             output->segment.length);

    deviceid.device_id2 = handle->export->export.export_id;
    deviceid.devid = handle->inode;

    dataServerWire.inode = handle->inode;
    layout_util = MFSCHUNKSIZE;

    nfs_status = FSAL_encode_file_layout(xdrStream, &deviceid, layout_util,
                                         0, 0, &op_ctx->ctx_export->export_id,
                                         1, &dsBufferDescriptor);

    if (nfs_status) {
        LogMajor(COMPONENT_PNFS, "Failed to encode nfsv4_1_file_layout.");
        return nfs_status;
    }

    output->return_on_close = true;
    output->last_segment = true;

    return nfs_status;
}


/*! \brief Potentially return one layout segment
 *
 * \see fsal_api.h for more information
 */
static nfsstat4 _layoutreturn(struct fsal_obj_handle *objectHandle,
                              XDR *xdrStream,
                              const struct fsal_layoutreturn_arg *arguments)
{
    // Unused variables
    (void ) objectHandle;
    (void ) xdrStream;

    if (arguments->lo_type != LAYOUT4_NFSV4_1_FILES) {
        LogDebug(COMPONENT_PNFS, "Unsupported layout type: %x",
                 arguments->lo_type);

        return NFS4ERR_UNKNOWN_LAYOUTTYPE;
    }

    return NFS4_OK;
}

/*! \brief Commit a segment of a layout
 *
 * \see fsal_api.h for more information
 */
static nfsstat4 _layoutcommit(struct fsal_obj_handle *objectHandle,
                              XDR *xdrStream,
                              const struct fsal_layoutcommit_arg *arguments,
                              struct fsal_layoutcommit_res *output)
{
    // Unused variable
    (void ) xdrStream;

    struct FSExport *export;
    struct FSHandle *handle;
    struct liz_attr_reply previousReply;

    // FIXME(haze): Does this function make sense for our implementation ?

    /* Sanity check on type */
    if (arguments->type != LAYOUT4_NFSV4_1_FILES) {
        LogCrit(COMPONENT_PNFS, "Unsupported layout type: %x", arguments->type);
        return NFS4ERR_UNKNOWN_LAYOUTTYPE;
    }

    export = container_of(op_ctx->fsal_export, struct FSExport, export);
    handle = container_of(objectHandle, struct FSHandle, fileHandle);

    int rc = fs_getattr(export->fsInstance, &op_ctx->creds,
                        handle->inode, &previousReply);

    if (rc < 0) {
        LogCrit(COMPONENT_PNFS, "Error '%s' in attempt to get "
                "attributes of file %lli.",
                liz_error_string(liz_last_err()),
                (long long)handle->inode);

        return Nfs4LastError();
    }

    struct stat posixAttributes;
    int mask = 0;

    memset(&posixAttributes, 0, sizeof(posixAttributes));

    if (arguments->new_offset &&
        previousReply.attr.st_size < (long)arguments->last_write + 1)
    {
        mask |= LIZ_SET_ATTR_SIZE;
        posixAttributes.st_size = arguments->last_write + 1;
        output->size_supplied = true;
        output->new_size = arguments->last_write + 1;
    }

    if (arguments->time_changed &&
        (arguments->new_time.seconds > previousReply.attr.st_mtim.tv_sec ||
        (arguments->new_time.seconds == previousReply.attr.st_mtim.tv_sec &&
         arguments->new_time.nseconds > previousReply.attr.st_mtim.tv_nsec)))
    {
            posixAttributes.st_mtim.tv_sec = arguments->new_time.seconds;
            posixAttributes.st_mtim.tv_sec = arguments->new_time.nseconds;
            mask |= LIZ_SET_ATTR_MTIME;
    }

    liz_attr_reply_t reply;
    rc = fs_setattr(export->fsInstance, &op_ctx->creds, handle->inode,
                    &posixAttributes, mask, &reply);

    if (rc < 0) {
        LogCrit(COMPONENT_PNFS, "Error '%s' in attempt to set attributes "
                "of file %lli.", liz_error_string(liz_last_err()),
                (long long)handle->inode);
        return Nfs4LastError();
    }

    output->commit_done = true;
    return NFS4_OK;
}

void initializeMetaDataServerOperations(struct fsal_obj_ops *ops)
{
    ops->layoutget = _layoutget;
    ops->layoutreturn = _layoutreturn;
    ops->layoutcommit = _layoutcommit;
}
