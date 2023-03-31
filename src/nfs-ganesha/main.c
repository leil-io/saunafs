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

#include <stdlib.h>
#include <assert.h>
#include "fsal.h"
#include "fsal_types.h"
#include "FSAL/fsal_init.h"
#include "FSAL/fsal_commonlib.h"
#include "fsal_api.h"
#include "abstract_mem.h"
#include "nfs_exports.h"
#include "export_mgr.h"
#include "nfs_core.h"
#include "sal_functions.h"
#include "pnfs_utils.h"

#include "lzfs_fsal_types.h"
#include "lzfs_fsal_methods.h"

#include "context_wrap.h"

/* FSAL name determines name of shared library: libfsal<name>.so */
static const char *module_name = "LizardFS";

/**
 * my module private storage
 */

struct FSModule LizardFS = {
    .module = {
        .fs_info = {
            .maxfilesize = UINT64_MAX,
            .maxlink = _POSIX_LINK_MAX,
            .maxnamelen = MFS_NAME_MAX,
            .maxpathlen = MAXPATHLEN,
            .no_trunc = true,
            .chown_restricted = false,
            .case_insensitive = false,
            .case_preserving = true,
            .link_support = true,
            .symlink_support = true,
            .lock_support = true,
            .lock_support_async_block = false,
            .named_attr = true,
            .unique_handles = true,
        #ifdef ENABLE_NFS_ACL_SUPPORT
            .acl_support = FSAL_ACLSUPPORT_ALLOW | FSAL_ACLSUPPORT_DENY,
        #else
            .acl_support = 0,
        #endif
            .cansettime = true,
            .homogenous = true,
            .supported_attrs = LZFS_SUPPORTED_ATTRS,
            .maxread = FSAL_MAXIOSIZE,
            .maxwrite = FSAL_MAXIOSIZE,
            .umask = 0,
            .auth_exportpath_xdev = false,
            .pnfs_mds = true,
            .pnfs_ds = true,
            .fsal_trace = false,
            .fsal_grace = false,
            .link_supports_permission_checks = true,
            .xattr_support = true,
        }
    },
    .onlyOneUser = false
};

static struct config_item export_params[] = {
    CONF_ITEM_MODE("umask", 0, fsal_staticfsinfo_t, umask),
    CONF_ITEM_BOOL("link_support", true, fsal_staticfsinfo_t,
                   link_support),
    CONF_ITEM_BOOL("symlink_support", true, fsal_staticfsinfo_t,
                   symlink_support),
    CONF_ITEM_BOOL("cansettime", true, fsal_staticfsinfo_t, cansettime),
    CONF_ITEM_BOOL("auth_xdev_export", false, fsal_staticfsinfo_t,
                   auth_exportpath_xdev),
    CONF_ITEM_UI64("maxread", 512, FSAL_MAXIOSIZE, FSAL_MAXIOSIZE,
                   fsal_staticfsinfo_t, maxread),
    CONF_ITEM_UI64("maxwrite", 512, FSAL_MAXIOSIZE, FSAL_MAXIOSIZE,
                   fsal_staticfsinfo_t, maxwrite),
    CONF_ITEM_BOOL("PNFS_MDS", false, fsal_staticfsinfo_t, pnfs_mds),
    CONF_ITEM_BOOL("PNFS_DS", false, fsal_staticfsinfo_t, pnfs_ds),
    CONF_ITEM_BOOL("fsal_trace", true, fsal_staticfsinfo_t, fsal_trace),
    CONF_ITEM_BOOL("fsal_grace", false, fsal_staticfsinfo_t, fsal_grace),
    CONF_ITEM_BOOL("only_one_user", false, FSModule, onlyOneUser),
    CONFIG_EOL
};

static struct config_block export_param = {
    .dbus_interface_name = "org.ganesha.nfsd.config.fsal.lizardfs",
    .blk_desc.name = "LizardFS",
    .blk_desc.type = CONFIG_BLOCK,
    .blk_desc.u.blk.init = noop_conf_init,
    .blk_desc.u.blk.params = export_params,
    .blk_desc.u.blk.commit = noop_conf_commit
};

static struct config_item fsal_export_params[] = {
    CONF_ITEM_NOOP("name"),
    CONF_MAND_STR("hostname", 1, MAXPATHLEN, NULL, FSExport,
                  initialParameters.host),
    CONF_ITEM_STR("port", 1, MAXPATHLEN, "9421", FSExport,
                  initialParameters.port),
    CONF_ITEM_STR("mountpoint", 1, MAXPATHLEN, "nfs-ganesha",
                  FSExport, initialParameters.mountpoint),
    CONF_ITEM_STR("subfolder", 1, MAXPATHLEN, "/", FSExport,
                  initialParameters.subfolder),
    CONF_ITEM_BOOL("delayed_init", false, FSExport,
                   initialParameters.delayed_init),
    CONF_ITEM_UI32("io_retries", 0, 1024, 30, FSExport,
                   initialParameters.io_retries),
    CONF_ITEM_UI32("chunkserver_round_time_ms", 0, 65536, 200,
                   FSExport, initialParameters.chunkserver_round_time_ms),
    CONF_ITEM_UI32("chunkserver_connect_timeout_ms", 0, 65536, 2000,
                   FSExport,
                   initialParameters.chunkserver_connect_timeout_ms),
    CONF_ITEM_UI32("chunkserver_wave_read_timeout_ms", 0, 65536, 500,
                   FSExport,
                   initialParameters.chunkserver_wave_read_timeout_ms),
    CONF_ITEM_UI32("total_read_timeout_ms", 0, 65536, 2000,
                   FSExport, initialParameters.total_read_timeout_ms),
    CONF_ITEM_UI32("cache_expiration_time_ms", 0, 65536, 1000,
                   FSExport, initialParameters.cache_expiration_time_ms),
    CONF_ITEM_UI32("readahead_max_window_size_kB", 0, 65536, 16384,
                   FSExport, initialParameters.readahead_max_window_size_kB),
    CONF_ITEM_UI32("write_cache_size", 0, 1024, 64, FSExport,
                   initialParameters.write_cache_size),
    CONF_ITEM_UI32("write_workers", 0, 32, 10, FSExport,
                   initialParameters.write_workers),
    CONF_ITEM_UI32("write_window_size", 0, 256, 32, FSExport,
                   initialParameters.write_window_size),
    CONF_ITEM_UI32("chunkserver_write_timeout_ms", 0, 60000, 5000,
                   FSExport, initialParameters.chunkserver_write_timeout_ms),
    CONF_ITEM_UI32("cache_per_inode_percentage", 0, 80, 25,
                   FSExport, initialParameters.cache_per_inode_percentage),
    CONF_ITEM_UI32("symlink_cache_timeout_s", 0, 60000, 3600,
                   FSExport, initialParameters.symlink_cache_timeout_s),
    CONF_ITEM_BOOL("debug_mode", false, FSExport,
                   initialParameters.debug_mode),
    CONF_ITEM_I32("keep_cache", 0, 2, 0, FSExport,
                  initialParameters.keep_cache),
    CONF_ITEM_BOOL("verbose", false, FSExport,
                   initialParameters.verbose),
    CONF_ITEM_UI32("fileinfo_cache_timeout", 1, 3600, 60, FSExport,
                   cacheTimeout),
    CONF_ITEM_UI32("fileinfo_cache_max_size", 100, 1000000, 1000,
                   FSExport, cacheMaximumSize),
    CONF_ITEM_STR("password", 1, 128, NULL, FSExport,
                  initialParameters.password),
    CONF_ITEM_STR("md5_pass", 32, 32, NULL, FSExport,
                  initialParameters.md5_pass),
    CONFIG_EOL
};

static struct config_block fsal_export_param_block = {
    .dbus_interface_name = "org.ganesha.nfsd.config.fsal.lizardfs-export%d",
    .blk_desc.name = "FSAL",
    .blk_desc.type = CONFIG_BLOCK,
    .blk_desc.u.blk.init = noop_conf_init,
    .blk_desc.u.blk.params = fsal_export_params,
    .blk_desc.u.blk.commit = noop_conf_commit
};

static fsal_status_t create_export(struct fsal_module *FSALModule,
                                   void *parseNode,
                                   struct config_error_type *errorType,
                                   const struct fsal_up_vector *upcallOperations)
{
    fsal_status_t status;
    struct fsal_pnfs_ds *pnfsDataServer = NULL;
    int rc;

    struct FSExport *export = gsh_calloc(1, sizeof(struct FSExport));

    fsal_export_init(&export->export);
    initializeExportOperations(&export->export.exp_ops);

    // parse parameters for this export
    liz_set_default_init_params(&export->initialParameters, "", "", "");

    if (parseNode) {
        rc = load_config_from_node(parseNode, &fsal_export_param_block,
                                   export, true, errorType);

        if (rc != 0) {
            LogCrit(COMPONENT_FSAL,
                    "Failed to parse export configuration for %s",
                    CTX_FULLPATH(op_ctx));

            status = fsalstat(ERR_FSAL_INVAL, 0);
            goto error;
        }
    }

    export->initialParameters.subfolder = gsh_strdup(CTX_FULLPATH(op_ctx));
    export->fsInstance = liz_init_with_params(&export->initialParameters);

    if (export->fsInstance == NULL) {
        LogCrit(COMPONENT_FSAL, "Unable to mount LizardFS cluster for %s.",
                CTX_FULLPATH(op_ctx));

        status = fsalstat(ERR_FSAL_SERVERFAULT, 0);
        goto error;
    }

    if (fsal_attach_export(FSALModule, &export->export.exports) != 0) {
        LogCrit(COMPONENT_FSAL, "Unable to attach export for %s.",
                CTX_FULLPATH(op_ctx));

        status = fsalstat(ERR_FSAL_SERVERFAULT, 0);
        goto error;
    }

    export->export.fsal = FSALModule;
    export->export.up_ops = upcallOperations;

    export->isDSEnabled = export->export.exp_ops.fs_supports(
                &export->export, fso_pnfs_ds_supported);

    if (export->isDSEnabled) {
        export->fileinfoCache = liz_create_fileinfo_cache(
                    export->cacheMaximumSize,
                    export->cacheTimeout * 1000);

        if (export->fileinfoCache == NULL) {
            LogCrit(COMPONENT_FSAL, "Unable to create fileinfo cache for %s.",
                    CTX_FULLPATH(op_ctx));

            status = fsalstat(ERR_FSAL_SERVERFAULT, 0);
            goto error;
        }

        status = FSALModule->m_ops.create_fsal_pnfs_ds(FSALModule, parseNode,
                                                       &pnfsDataServer);

        if (status.major != ERR_FSAL_NO_ERROR) {
            goto error;
        }

        // special case: server_id matches export_id
        pnfsDataServer->id_servers = op_ctx->ctx_export->export_id;
        pnfsDataServer->mds_export = op_ctx->ctx_export;
        pnfsDataServer->mds_fsal_export = &export->export;

        if (!pnfs_ds_insert(pnfsDataServer)) {
            LogCrit(COMPONENT_CONFIG, "Server id %d already in use.",
                    pnfsDataServer->id_servers);

            status.major = ERR_FSAL_EXIST;

            // Return the ref taken by create_fsal_pnfs_ds
            pnfs_ds_put(pnfsDataServer);
            goto error;
        }

        LogDebug(COMPONENT_PNFS, "pnfs ds was enabled for [%s]",
                 CTX_FULLPATH(op_ctx));
    }

    export->isMDSEnabled = export->export.exp_ops.fs_supports(
                &export->export, fso_pnfs_mds_supported);

    if (export->isMDSEnabled) {
        LogDebug(COMPONENT_PNFS, "pnfs mds was enabled for [%s]",
                 CTX_FULLPATH(op_ctx));

        initializePnfsExportOperations(&export->export.exp_ops);
    }

    // get attributes for root inode
    liz_attr_reply_t reply;
    rc = fs_getattr(export->fsInstance, &op_ctx->creds,
                    SPECIAL_INODE_ROOT, &reply);

    if (rc < 0) {
        status = fsalLastError();

        if (pnfsDataServer != NULL) {
            // Remove and destroy the fsal_pnfs_ds
            pnfs_ds_remove(pnfsDataServer->id_servers);
        }

        goto error_pds;
    }

    export->rootHandle = allocateNewHandle(&reply.attr, export);
    op_ctx->fsal_export = &export->export;

    LogDebug(COMPONENT_FSAL, "LizardFS module export %s.",
             CTX_FULLPATH(op_ctx));

    return fsalstat(ERR_FSAL_NO_ERROR, 0);

error_pds:
    if (pnfsDataServer != NULL)
        // Return the ref taken by create_fsal_pnfs_ds
        pnfs_ds_put(pnfsDataServer);

error:
    if (export) {
        if (export->fsInstance) {
            liz_destroy(export->fsInstance);
        }

        if (export->fileinfoCache) {
            liz_destroy_fileinfo_cache(export->fileinfoCache);
        }

        gsh_free(export);
    }

    return status;
}

// Module methods
// init_config must be called with a reference taken (via lookup_fsal)
static fsal_status_t init_config(struct fsal_module *FSALModule,
                                 config_file_t configFile,
                                 struct config_error_type *errorType)
{
    struct FSModule *myself;
    myself = container_of(FSALModule, struct FSModule, module);

    (void) load_config_from_parse(configFile, &export_param,
                                  &myself->filesystemInfo, true, errorType);

    if (!config_error_is_harmless(errorType)) {
        LogDebug(COMPONENT_FSAL, "config_error_is_harmless failed.");
        return fsalstat(ERR_FSAL_INVAL, 0);
    }

    display_fsinfo(&myself->module);

    LogDebug(COMPONENT_FSAL,
             "ALLI: FSAL INIT: Supported attributes mask = 0x%" PRIx64,
             myself->module.fs_info.supported_attrs);

    return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

// Internal LizardFS method linkage to export object

/**
 * @brief Initialize and register the FSAL
 *
 * Module initialization.
 * Called by dlopen() to register the module
 * keep a private pointer to me in myself
 */

// Linkage to the exports and handle operations initializers

MODULE_INIT void lizardfs_init(void)
{
    struct fsal_module *myself = &LizardFS.module;

    int retval = register_fsal(myself, module_name, FSAL_MAJOR_VERSION,
                               FSAL_MINOR_VERSION, FSAL_ID_LIZARDFS);

    if (retval) {
        LogCrit(COMPONENT_FSAL, "LizardFS module failed to register.");
        return;
    }

    // Set up module operations
    myself->m_ops.create_export = create_export;
    myself->m_ops.init_config = init_config;
    myself->m_ops.fsal_pnfs_ds_ops = initializeDataServerOperations;
    initializePnfsOperations(&myself->m_ops);

    // Initialize fsal_obj_handle ops for FSAL LizardFS
    initializeFilesystemOperations(&LizardFS.operations);
}

/**
 * @brief Release FSAL resources
 *
 * This function unregisters the FSAL and frees its module handle.
 * The FSAL has no other resources to release on the per-FSAL level.
 */

MODULE_FINI void finish(void)
{
    int retval = unregister_fsal(&LizardFS.module);

    if (retval != 0) {
        LogCrit(COMPONENT_FSAL, "Unable to unload Fuse FSAL. Dying with "
                                "extreme prejudice.");
        abort();
    }
}
