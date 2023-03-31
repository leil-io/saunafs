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
#include "gsh_config.h"
#include "pnfs_utils.h"

#include "context_wrap.h"
#include "lzfs_fsal_methods.h"
#include "mount/client/lizardfs_c_api.h"
#include "protocol/MFSCommunication.h"

static int compare(const void *a, const void *b)
{
    uint32_t ipFromChunkserverA = ((const liz_chunkserver_info_t *)a)->ip;
    uint32_t ipFromChunkserverB = ((const liz_chunkserver_info_t *)b)->ip;

    return ipFromChunkserverA - ipFromChunkserverB;
}

static int is_disconnected(const void *a, void *unused)
{
    // Unused variable
    (void ) unused;

    return ((const liz_chunkserver_info_t *)a)->version ==
            kDisconnectedChunkServerVersion;
}

static int is_same_ip(const void *a, void *base)
{
    if (a == base) {
        return 0;
    }

    return ((const liz_chunkserver_info_t *)a)->ip ==
           ((const liz_chunkserver_info_t *)a - 1)->ip;
}

static size_t remove_if(void *base, size_t num, size_t size,
                        int (*predicate)(const void *a, void *work_data),
                        void *work_data)
{
    size_t j = 0;

    for (size_t i = 0; i < num; ++i) {
        if (!predicate((uint8_t *)base + i * size, work_data)) {
            memcpy((uint8_t *)base + i * size,
                   (uint8_t *)base + j * size,
                   size);
            j++;
        }
    }
    return j;
}

static void shuffle(void *base, size_t num, size_t size)
{
    uint8_t temp[size];

    if (num == 0) {
        return;
    }

    for (size_t i = 0; i < num - 1; ++i) {
        size_t j = i + rand() % (num - i);

        memcpy(temp, (uint8_t *)base + i * size, size);

        memcpy((uint8_t *)base + i * size,
               (uint8_t *)base + j * size, size);

        memcpy((uint8_t *)base + j * size, temp, size);
    }
}

static liz_chunkserver_info_t *randomizedChunkserverList(
        struct FSExport *export, uint32_t *chunkserverCount)
{
    liz_chunkserver_info_t *chunkserverInfo = NULL;

    chunkserverInfo = gsh_malloc(LZFS_BIGGEST_STRIPE_COUNT *
                                  sizeof(liz_chunkserver_info_t));

    int rc = liz_get_chunkservers_info(export->fsInstance, chunkserverInfo,
                                       LZFS_BIGGEST_STRIPE_COUNT,
                                       chunkserverCount);
	if (rc < 0) {
        *chunkserverCount = 0;
        gsh_free(chunkserverInfo);
        return NULL;
    }

    // Free labels, we don't need them.
    liz_destroy_chunkservers_info(chunkserverInfo);

    // remove disconnected
    *chunkserverCount = remove_if(chunkserverInfo, *chunkserverCount,
                                   sizeof(liz_chunkserver_info_t),
                                   is_disconnected, NULL);

    // remove entries with the same ip
    qsort(chunkserverInfo, *chunkserverCount,
          sizeof(liz_chunkserver_info_t), compare);

    *chunkserverCount = remove_if(chunkserverInfo, *chunkserverCount,
                                   sizeof(liz_chunkserver_info_t),
                                   is_same_ip, chunkserverInfo);

    // randomize
    shuffle(chunkserverInfo, *chunkserverCount,
            sizeof(liz_chunkserver_info_t));

    return chunkserverInfo;
}

/*! \brief Fill DS list with entries corresponding to chunks */
static int fillChunkDataServerList(XDR *da_addr_body,
                                   liz_chunk_info_t *chunkInfo,
                                   liz_chunkserver_info_t *chunkserverInfo,
                                   uint32_t chunkCount,
                                   uint32_t stripeCount,
                                   uint32_t chunkserverCount,
                                   uint32_t *chunkserverIndex)
{
    fsal_multipath_member_t host[LZFS_EXPECTED_BACKUP_DS_COUNT];

    uint32_t size = MIN(chunkCount, stripeCount);

    const int upperBound = LZFS_EXPECTED_BACKUP_DS_COUNT;

    for (uint32_t chunkIndex = 0; chunkIndex < size; ++chunkIndex)
    {
            liz_chunk_info_t *chunk = &chunkInfo[chunkIndex];
            int serverCount = 0;

            memset(host, 0, upperBound * sizeof(fsal_multipath_member_t));

            // prefer std chunk part type
            for (size_t i = 0; i < chunk->parts_size &&
                 serverCount < upperBound; ++i)
            {
                    if (chunk->parts[i].part_type_id != LZFS_STD_CHUNK_PART_TYPE) {
                        continue;
                    }

                    host[serverCount].proto = TCP_PROTO_NUMBER;
                    host[serverCount].addr = chunk->parts[i].addr;
                    host[serverCount].port = NFS_PORT;
                    ++serverCount;
            }

            for (size_t i = 0; i < chunk->parts_size
                 && serverCount < upperBound; ++i)
            {
                    if (chunk->parts[i].part_type_id == LZFS_STD_CHUNK_PART_TYPE) {
                        continue;
                    }

                    host[serverCount].proto = TCP_PROTO_NUMBER;
                    host[serverCount].addr = chunk->parts[i].addr;
                    host[serverCount].port = NFS_PORT;
                    ++serverCount;
            }

            // fill unused entries with the servers from randomized chunkserver list
            while (serverCount < upperBound) {
                host[serverCount].proto = TCP_PROTO_NUMBER;
                host[serverCount].addr = chunkserverInfo[*chunkserverIndex].ip;
                host[serverCount].port = NFS_PORT;

                ++serverCount;
                *chunkserverIndex = (*chunkserverIndex + 1) % chunkserverCount;
            }

            // encode ds entry
            nfsstat4 nfs_status = FSAL_encode_v4_multipath(da_addr_body,
                                                           serverCount, host);

            if (nfs_status != NFS4_OK) {
                return -1;
            }
    }

    return 0;
}

/*! \brief Fill unused part of DS list with servers from randomized chunkserver
 *  list */
static int fillUnusedDataServerList(XDR *xdrStream,
                                    liz_chunkserver_info_t *chunkserverInfo,
                                    uint32_t chunkCount,
                                    uint32_t stripeCount,
                                    uint32_t chunkserverCount,
                                    uint32_t *chunkserverIndex)
{
    fsal_multipath_member_t host[LZFS_EXPECTED_BACKUP_DS_COUNT];

    uint32_t size = MIN(chunkCount, stripeCount);

    const int upperBound = LZFS_EXPECTED_BACKUP_DS_COUNT;

    for (uint32_t chunkIndex = size; chunkIndex < stripeCount; ++chunkIndex)
    {
            int serverCount = 0, index;

            memset(host, 0, upperBound * sizeof(fsal_multipath_member_t));

            while (serverCount < LZFS_EXPECTED_BACKUP_DS_COUNT) {
                index = (*chunkserverIndex + serverCount) % chunkserverCount;

                host[serverCount].proto = TCP_PROTO_NUMBER;
                host[serverCount].addr = chunkserverInfo[index].ip;
                host[serverCount].port = NFS_PORT;

                ++serverCount;
            }

            *chunkserverIndex = (*chunkserverIndex + 1) % chunkserverCount;

            nfsstat4 nfs_status = FSAL_encode_v4_multipath(xdrStream,
                                                           serverCount, host);

            if (nfs_status != NFS4_OK) {
                return -1;
            }
    }

    return 0;
}

/*! \brief Get information about a pNFS device
 *
 * The function converts LizardFS file's chunk information to pNFS device info.
 *
 * Linux pNFS client imposes limit on stripe size (LZFS_BIGGEST_STRIPE_COUNT = 4096).
 * If we would use straight forward approach of converting each chunk to stripe entry,
 * we would be limited to file size of 256 GB (4096 * 64MB).
 *
 * To avoid this problem each DS can read/write data from any chunk (Remember that pNFS client
 * takes DS address from DS list in round robin fashion). Of course it's more efficient
 * if DS is answering queries about chunks residing locally.
 *
 * To achieve the best performance we fill the DS list in a following way:
 *
 * First we prepare randomized list of all chunkservers (RCSL).
 * Then for each chunk we fill multipath DS list entry with addresses of chunkservers storing
 * this chunk. If there is less chunkservers than LZFS_EXPECTED_BACKUP_DS_COUNT then
 * we use chunkservers from RCSL.
 *
 * If we didn't use all the possible space in DS list (LZFS_BIGGEST_STRIPE_COUNT), then we fill
 * rest of the stripe entries with addresses from RCSL (again LZFS_EXPECTED_BACKUP_DS_COUNT
 * addresses for each stripe entry).
 *
 * \see fsal_api.h for more information
 */
static nfsstat4 _getdeviceinfo(struct fsal_module *FSALModule,
                               XDR *xdrStream,
                               const layouttype4 type,
                               const struct pnfs_deviceid *deviceid)
{
    struct fsal_export *exportHandle;
    struct FSExport *export = NULL;

    liz_chunk_info_t *chunkInfo = NULL;
    liz_chunkserver_info_t *chunkserverInfo = NULL;

    uint32_t chunkCount, chunkserverCount;
    uint32_t stripeCount, chunkserverIndex;

    struct glist_head *glist, *glistn;

    if (type != LAYOUT4_NFSV4_1_FILES) {
        LogCrit(COMPONENT_PNFS, "Unsupported layout type: %x", type);
        return NFS4ERR_UNKNOWN_LAYOUTTYPE;
    }

    uint16_t export_id = deviceid->device_id2;

    glist_for_each_safe(glist, glistn, &FSALModule->exports) {
        exportHandle = glist_entry(glist, struct fsal_export, exports);

        if (exportHandle->export_id == export_id) {
            export = container_of(exportHandle, struct FSExport, export);
            break;
        }
    }

    if (!export) {
        LogCrit(COMPONENT_PNFS, "Couldn't find export with id: %"
                PRIu16, export_id);

        return NFS4ERR_SERVERFAULT;
    }

    // get the chunk list for file
    chunkInfo = gsh_malloc(LZFS_BIGGEST_STRIPE_COUNT * sizeof(liz_chunk_info_t));

    int rc = fs_get_chunks_info(export->fsInstance, &op_ctx->creds,
                                deviceid->devid, 0, chunkInfo,
                                LZFS_BIGGEST_STRIPE_COUNT, &chunkCount);

    if (rc < 0) {
        LogCrit(COMPONENT_PNFS,
                "Failed to get LizardFS layout for export = %"
                PRIu16 " inode = %" PRIu64, export_id, deviceid->devid);

        goto generic_err;
    }

    chunkserverInfo = randomizedChunkserverList(export, &chunkserverCount);

    if (chunkserverInfo == NULL || chunkserverCount == 0) {
        LogCrit(COMPONENT_PNFS,
                "Failed to get LizardFS layout for export = %" PRIu16
                " inode = %" PRIu64, export_id, deviceid->devid);

        goto generic_err;
    }

    chunkserverIndex = 0;

    stripeCount = MIN(chunkCount + chunkserverCount,
                      LZFS_BIGGEST_STRIPE_COUNT);

    if (!inline_xdr_u_int32_t(xdrStream, &stripeCount)) {
        goto encode_err;
    }

    for (uint32_t chunkIndex = 0; chunkIndex < stripeCount; ++chunkIndex) {
        if (!inline_xdr_u_int32_t(xdrStream, &chunkIndex)) {
            goto encode_err;
        }
    }

    if (!inline_xdr_u_int32_t(xdrStream, &stripeCount)) {
        goto encode_err;
    }

    rc = fillChunkDataServerList(xdrStream, chunkInfo, chunkserverInfo,
                                 chunkCount, stripeCount, chunkserverCount,
                                 &chunkserverIndex);

    if (rc < 0) {
        goto encode_err;
    }

    rc = fillUnusedDataServerList(xdrStream, chunkserverInfo, chunkCount,
                                  stripeCount, chunkserverCount,
                                  &chunkserverIndex);

    if (rc < 0) {
        goto encode_err;
    }

    liz_destroy_chunks_info(chunkInfo);

    gsh_free(chunkInfo);
    gsh_free(chunkserverInfo);

    return NFS4_OK;

encode_err:
    LogCrit(COMPONENT_PNFS,
            "Failed to encode device information for export = %" PRIu16
            " inode = %" PRIu64, export_id, deviceid->devid);

generic_err:
    if (chunkInfo) {
        liz_destroy_chunks_info(chunkInfo);
        gsh_free(chunkInfo);
    }

    if (chunkserverInfo) {
        gsh_free(chunkserverInfo);
    }

    return NFS4ERR_SERVERFAULT;
}

/*! \brief Get list of available devices
 *
 * \see fsal_api.h for more information
 */
static nfsstat4 _getdevicelist(struct fsal_export *exportHandle,
                               layouttype4 type, void *opaque,
                               bool (*cb)(void *opaque,
                                          const uint64_t id),
                               struct fsal_getdevicelist_res *res)
{
    // Unused variables to avoid compiler warnings
    (void ) exportHandle;
    (void ) type;
    (void ) opaque;
    (void ) cb;

    res->eof = true;
    return NFS4_OK;
}

/*! \brief Get layout types supported by export
 *
 * \see fsal_api.h for more information
 */
static void _fs_layouttypes(struct fsal_export *exportHandle,
                            int32_t *count,
                            const layouttype4 **types)
{
    // Unused variables to avoid compiler warnings
    (void ) exportHandle;

    static const layouttype4 supportedLayoutType = LAYOUT4_NFSV4_1_FILES;
    *types = &supportedLayoutType;
    *count = 1;
}

/*! \brief Get layout block size for export
 *
 * \see fsal_api.h for more information
 */
static uint32_t _fs_layout_blocksize(struct fsal_export *exportHandle)
{
    // Unused variable to avoid compiler warnings
    (void ) exportHandle;

    return MFSCHUNKSIZE;
}

/*! \brief Maximum number of segments we will use
 *
 * \see fsal_api.h for more information
 */
static uint32_t _fs_maximum_segments(struct fsal_export *exportHandle)
{
    // Unused variables to avoid compiler warnings
    (void ) exportHandle;

    return 1;
}

/*! \brief Size of the buffer needed for loc_body at layoutget
 *
 * \see fsal_api.h for more information
 */
static size_t _fs_loc_body_size(struct fsal_export *exportHandle)
{
    // Unused variables to avoid compiler warnings
    (void ) exportHandle;

	return 0x100;  // typical value in NFS FSAL plugins
}

/*! \brief Max Size of the buffer needed for da_addr_body in getdeviceinfo
 *
 * \see fsal_api.h for more information
 */
static size_t _fs_da_addr_size(struct fsal_module *FSALModule)
{
    // Unused variables to avoid compiler warnings
    (void ) FSALModule;

    // one stripe index + number of addresses +
    // LZFS_EXPECTED_BACKUP_DS_COUNT addresses per chunk each address takes
    // 37 bytes (we use 40 for safety) we add 32 bytes of overhead
    // (includes stripe count and DS count)
    return LZFS_BIGGEST_STRIPE_COUNT *
            (4 + (4 + LZFS_EXPECTED_BACKUP_DS_COUNT * 40)) + 32;
}

void initializePnfsExportOperations(struct export_ops *ops)
{
    ops->getdevicelist = _getdevicelist;
    ops->fs_layouttypes = _fs_layouttypes;
    ops->fs_layout_blocksize = _fs_layout_blocksize;
    ops->fs_maximum_segments = _fs_maximum_segments;
    ops->fs_loc_body_size = _fs_loc_body_size;
}

void initializePnfsOperations(struct fsal_ops *ops)
{
    ops->getdeviceinfo = _getdeviceinfo;
    ops->fs_da_addr_size = _fs_da_addr_size;
}
