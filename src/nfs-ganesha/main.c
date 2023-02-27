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

struct lzfs_fsal_module LizardFS = {
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
            .acl_support = FSAL_ACLSUPPORT_ALLOW | FSAL_ACLSUPPORT_DENY,
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
    .only_one_user = false
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
    CONF_ITEM_BOOL("only_one_user", false, lzfs_fsal_module, only_one_user),
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
    CONF_MAND_STR("hostname", 1, MAXPATHLEN, NULL, lzfs_fsal_export,
                  lzfs_params.host),
    CONF_ITEM_STR("port", 1, MAXPATHLEN, "9421", lzfs_fsal_export,
                  lzfs_params.port),
    CONF_ITEM_STR("mountpoint", 1, MAXPATHLEN, "nfs-ganesha",
                  lzfs_fsal_export, lzfs_params.mountpoint),
    CONF_ITEM_STR("subfolder", 1, MAXPATHLEN, "/", lzfs_fsal_export,
                  lzfs_params.subfolder),
    CONF_ITEM_BOOL("delayed_init", false, lzfs_fsal_export,
                   lzfs_params.delayed_init),
    CONF_ITEM_UI32("io_retries", 0, 1024, 30, lzfs_fsal_export,
                   lzfs_params.io_retries),
    CONF_ITEM_UI32("chunkserver_round_time_ms", 0, 65536, 200,
                   lzfs_fsal_export, lzfs_params.chunkserver_round_time_ms),
    CONF_ITEM_UI32("chunkserver_connect_timeout_ms", 0, 65536, 2000,
                   lzfs_fsal_export,
                   lzfs_params.chunkserver_connect_timeout_ms),
    CONF_ITEM_UI32("chunkserver_wave_read_timeout_ms", 0, 65536, 500,
                   lzfs_fsal_export,
                   lzfs_params.chunkserver_wave_read_timeout_ms),
    CONF_ITEM_UI32("total_read_timeout_ms", 0, 65536, 2000,
                   lzfs_fsal_export, lzfs_params.total_read_timeout_ms),
    CONF_ITEM_UI32("cache_expiration_time_ms", 0, 65536, 1000,
                   lzfs_fsal_export, lzfs_params.cache_expiration_time_ms),
    CONF_ITEM_UI32("readahead_max_window_size_kB", 0, 65536, 16384,
                   lzfs_fsal_export, lzfs_params.readahead_max_window_size_kB),
    CONF_ITEM_UI32("write_cache_size", 0, 1024, 64, lzfs_fsal_export,
                   lzfs_params.write_cache_size),
    CONF_ITEM_UI32("write_workers", 0, 32, 10, lzfs_fsal_export,
                   lzfs_params.write_workers),
    CONF_ITEM_UI32("write_window_size", 0, 256, 32, lzfs_fsal_export,
                   lzfs_params.write_window_size),
    CONF_ITEM_UI32("chunkserver_write_timeout_ms", 0, 60000, 5000,
                   lzfs_fsal_export, lzfs_params.chunkserver_write_timeout_ms),
    CONF_ITEM_UI32("cache_per_inode_percentage", 0, 80, 25,
                   lzfs_fsal_export, lzfs_params.cache_per_inode_percentage),
    CONF_ITEM_UI32("symlink_cache_timeout_s", 0, 60000, 3600,
                   lzfs_fsal_export, lzfs_params.symlink_cache_timeout_s),
    CONF_ITEM_BOOL("debug_mode", false, lzfs_fsal_export,
                   lzfs_params.debug_mode),
    CONF_ITEM_I32("keep_cache", 0, 2, 0, lzfs_fsal_export,
                  lzfs_params.keep_cache),
    CONF_ITEM_BOOL("verbose", false, lzfs_fsal_export,
                   lzfs_params.verbose),
    CONF_ITEM_UI32("fileinfo_cache_timeout", 1, 3600, 60, lzfs_fsal_export,
                   fileinfo_cache_timeout),
    CONF_ITEM_UI32("fileinfo_cache_max_size", 100, 1000000, 1000,
                   lzfs_fsal_export, fileinfo_cache_max_size),
    CONF_ITEM_STR("password", 1, 128, NULL, lzfs_fsal_export,
                  lzfs_params.password),
    CONF_ITEM_STR("md5_pass", 32, 32, NULL, lzfs_fsal_export,
                  lzfs_params.md5_pass),
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

static fsal_status_t create_export(struct fsal_module *fsal_hdl,
                                   void *parse_node,
                                   struct config_error_type *err_type,
                                   const struct fsal_up_vector *up_ops)
{
    struct lzfs_fsal_export *export;
    fsal_status_t status;
    int rc;
    struct fsal_pnfs_ds *pds = NULL;

    export = gsh_calloc(1, sizeof(struct lzfs_fsal_export));

    fsal_export_init(&export->export);
    lzfs_export_ops_init(&export->export.exp_ops);

    // parse params for this export
    liz_set_default_init_params(&export->lzfs_params, "", "", "");
    if (parse_node) {
        rc = load_config_from_node(parse_node, &fsal_export_param_block,
                                   export, true, err_type);
        if (rc != 0) {
            LogCrit(COMPONENT_FSAL,
                    "Failed to parse export configuration for %s",
                    CTX_FULLPATH(op_ctx));

            status = fsalstat(ERR_FSAL_INVAL, 0);
            goto error;
        }
    }

    export->lzfs_params.subfolder = gsh_strdup(CTX_FULLPATH(op_ctx));
    export->lzfs_instance = liz_init_with_params(&export->lzfs_params);

    if (export->lzfs_instance == NULL) {
        LogCrit(COMPONENT_FSAL, "Unable to mount LizardFS cluster for %s.",
                CTX_FULLPATH(op_ctx));
        status = fsalstat(ERR_FSAL_SERVERFAULT, 0);
        goto error;
    }

    if (fsal_attach_export(fsal_hdl, &export->export.exports) != 0) {
        LogCrit(COMPONENT_FSAL, "Unable to attach export for %s.",
                CTX_FULLPATH(op_ctx));
        status = fsalstat(ERR_FSAL_SERVERFAULT, 0);
        goto error;
    }

    export->export.fsal = fsal_hdl;
    export->export.up_ops = up_ops;

    export->pnfs_ds_enabled =
            export->export.exp_ops.fs_supports(&export->export,
                                               fso_pnfs_ds_supported);
    if (export->pnfs_ds_enabled) {
        export->fileinfo_cache = liz_create_fileinfo_cache(
                    export->fileinfo_cache_max_size,
                    export->fileinfo_cache_timeout * 1000);
        if (export->fileinfo_cache == NULL) {
            LogCrit(COMPONENT_FSAL, "Unable to create fileinfo cache for %s.",
                    CTX_FULLPATH(op_ctx));
            status = fsalstat(ERR_FSAL_SERVERFAULT, 0);
            goto error;
        }

        status = fsal_hdl->m_ops.create_fsal_pnfs_ds(fsal_hdl,
                                                     parse_node, &pds);
        if (status.major != ERR_FSAL_NO_ERROR) {
            goto error;
        }

        /* special case: server_id matches export_id */
        pds->id_servers = op_ctx->ctx_export->export_id;
        pds->mds_export = op_ctx->ctx_export;
        pds->mds_fsal_export = &export->export;

        if (!pnfs_ds_insert(pds)) {
            LogCrit(COMPONENT_CONFIG, "Server id %d already in use.",
                    pds->id_servers);
            status.major = ERR_FSAL_EXIST;

            /* Return the ref taken by create_fsal_pnfs_ds */
            pnfs_ds_put(pds);
            goto error;
        }

        LogDebug(COMPONENT_PNFS, "pnfs ds was enabled for [%s]",
                 CTX_FULLPATH(op_ctx));
    }

    export->pnfs_mds_enabled =
            export->export.exp_ops.fs_supports(
                &export->export, fso_pnfs_mds_supported);
    if (export->pnfs_mds_enabled) {
        LogDebug(COMPONENT_PNFS, "pnfs mds was enabled for [%s]",
                 CTX_FULLPATH(op_ctx));
        lzfs_export_ops_pnfs(&export->export.exp_ops);
    }

    // get attributes for root inode
    liz_attr_reply_t ret;
    rc = liz_cred_getattr(export->lzfs_instance, &op_ctx->creds,
                          SPECIAL_INODE_ROOT, &ret);
    if (rc < 0) {
        status = lzfs_fsal_last_err();

        if (pds != NULL) {
            /* Remove and destroy the fsal_pnfs_ds */
            pnfs_ds_remove(pds->id_servers);
        }
        goto error_pds;
    }

    export->root = allocate_new_handle(&ret.attr, export);
    op_ctx->fsal_export = &export->export;

    LogDebug(COMPONENT_FSAL, "LizardFS module export %s.",
             CTX_FULLPATH(op_ctx));

    return fsalstat(ERR_FSAL_NO_ERROR, 0);

error_pds:
    if (pds != NULL)
        /* Return the ref taken by create_fsal_pnfs_ds */
        pnfs_ds_put(pds);

error:
    if (export) {
        if (export->lzfs_instance) {
            liz_destroy(export->lzfs_instance);
        }
        if (export->fileinfo_cache) {
            liz_destroy_fileinfo_cache(export->fileinfo_cache);
        }
        gsh_free(export);
    }

    return status;
}

/* Module methods */
/* init_config must be called with a reference taken (via lookup_fsal) */
static fsal_status_t init_config(struct fsal_module *module_in,
                                 config_file_t config_struct,
                                 struct config_error_type *err_type)
{
    struct lzfs_fsal_module *myself;
    myself = container_of(module_in, struct lzfs_fsal_module, module);

    (void) load_config_from_parse(config_struct, &export_param,
                                  &myself->fs_info, true, err_type);

    if (!config_error_is_harmless(err_type)) {
        LogDebug(COMPONENT_FSAL, "config_error_is_harmless failed.");
        return fsalstat(ERR_FSAL_INVAL, 0);
    }

    display_fsinfo(&myself->module);
    LogDebug(COMPONENT_FSAL,
             "ALLI: FSAL INIT: Supported attributes mask = 0x%" PRIx64,
             myself->module.fs_info.supported_attrs);

    return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/* Internal LizardFS method linkage to export object */

/**
 * @brief Initialize and register the FSAL
 *
 * Module initialization.
 * Called by dlopen() to register the module
 * keep a private pointer to me in myself
 */

/* linkage to the exports and handle ops initializers */

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
    myself->m_ops.fsal_pnfs_ds_ops = lzfs_fsal_ds_handle_ops_init;
    lzfs_fsal_ops_pnfs(&myself->m_ops);

    /* Initialize the fsal_obj_handle ops for FSAL LizardFS */
    lzfs_handle_ops_init(&LizardFS.handle_ops);
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
