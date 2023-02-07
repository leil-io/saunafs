/**
 * @file   lzfs_fsal_methods.h
 * @author Rubén Alcolea Núñez <ruben.crash@gmail.com>
 * @date Tue Aug 9 14:25 2022
 *
 * @brief Declaration methods for the FSAL
 *
 */

#ifndef LZFS_FSAL_METHODS
#define LZFS_FSAL_METHODS

#include "fsal_handle_syscalls.h"
#include "fsal_api.h"
#include "FSAL/fsal_commonlib.h"
#include "FSAL/fsal_localfs.h"
#include "FSAL/access_check.h"

#include "lzfs_fsal_types.h"

/* FSAL methods */

liz_context_t *lzfs_fsal_create_context(liz_t *instance, struct user_cred *cred);

fsal_staticfsinfo_t *lzfs_fsal_staticfsinfo(struct fsal_module *module_hdl);

void lzfs_export_ops_init(struct export_ops *ops);

void lzfs_handle_ops_init(struct fsal_obj_ops *ops);

static inline int root_fd(struct fsal_filesystem *fs)
{
    int fd = (long) fs->private_data;
    return fd;
}

static inline int get_root_fd(struct fsal_export *exp_hdl)
{
    return root_fd(exp_hdl->root_fs);
}

bool set_credentials(const struct user_cred *creds,
                     const struct fsal_module *fsal_module);

void restore_ganesha_credentials(const struct fsal_module *fsal_module);

// Methods for allocating/deleting handles

struct lzfs_fsal_handle *allocate_new_handle(
        const struct stat *attr, struct lzfs_fsal_export *export);

void delete_handle(struct lzfs_fsal_handle *obj);

// Methods for support ACL
fsal_status_t lzfs_int_getacl(struct lzfs_fsal_export *lzfs_export,
                              uint32_t inode, uint32_t owner,
                              fsal_acl_t **fsal_acl);

fsal_status_t lzfs_int_setacl(struct lzfs_fsal_export *lzfs_export,
                              uint32_t inode, const fsal_acl_t *fsal_acl,
                              unsigned int mode);

// Methods for handling errors
fsal_status_t lizardfs2fsal_error(liz_err_t err);

fsal_status_t lzfs_fsal_last_err(void);

nfsstat4 lzfs_nfs4_last_err(void);

// Methods for handling pNFS
void lzfs_fsal_ops_pnfs(struct fsal_ops *ops);

void lzfs_export_ops_pnfs(struct export_ops *ops);

void lzfs_handle_ops_pnfs(struct fsal_obj_ops *ops);

#endif  /* LZFS_FSAL_METHODS */
