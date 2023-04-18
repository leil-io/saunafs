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

#include "FSAL/fsal_localfs.h"
#include "FSAL/access_check.h"

#include "lzfs_fsal_types.h"

/// FSAL methods

liz_context_t *createFSALContext(liz_t *instance, struct user_cred *cred);

void initializeExportOperations(struct export_ops *ops);
void initializeFilesystemOperations(struct fsal_obj_ops *ops);

static inline int rootFileDescriptor(struct fsal_filesystem *fs)
{
    return (long) fs->private_data;
}

static inline int rootFileDescriptorFromExport(struct fsal_export *exp_hdl)
{
    return rootFileDescriptor(exp_hdl->root_fs);
}

bool setCredentials(const struct user_cred *creds,
                    const struct fsal_module *fsal_module);

void restoreGaneshaCredentials(const struct fsal_module *fsal_module);

/// Methods for allocating/deleting handles

struct FSHandle *allocateNewHandle(const struct stat *attr,
								   struct FSExport *export);

void deleteHandle(struct FSHandle *object);

// Methods for support ACL
fsal_status_t getACL(struct FSExport *export, uint32_t inode,
					 uint32_t owner, fsal_acl_t **fsal_acl);

fsal_status_t setACL(struct FSExport *export, uint32_t inode,
					 const fsal_acl_t *fsal_acl, unsigned int mode);

// Methods for handling errors
fsal_status_t lizardfsToFsalError(liz_err_t err);
fsal_status_t fsalLastError(void);
nfsstat4 Nfs4LastError(void);

// Methods for handling pNFS
void initializePnfsOperations(struct fsal_ops *ops);
void initializePnfsExportOperations(struct export_ops *ops);

void initializeDataServerOperations(struct fsal_pnfs_ds_ops *ops);
void initializeMetaDataServerOperations(struct fsal_obj_ops *ops);

#endif  /* LZFS_FSAL_METHODS */
