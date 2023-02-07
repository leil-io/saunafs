#include "fsal.h"
#include "fsal_convert.h"
#include "pnfs_utils.h"

#include "lzfs_fsal_methods.h"

fsal_staticfsinfo_t *lzfs_fsal_staticfsinfo(struct fsal_module *module_hdl)
{
    struct lzfs_fsal_module *module;
    module = container_of(module_hdl, struct lzfs_fsal_module, module);
    return &module->fs_info;
}

liz_context_t *lzfs_fsal_create_context(liz_t *instance,
                                        struct user_cred *cred)
{
    static const int kLocalGArraySize = 64;

    if (cred == NULL) {
        liz_context_t *ctx = liz_create_user_context(0, 0, 0, 0);
        return ctx;
    }

    liz_context_t *ctx;
    uid_t uid = (cred->caller_uid == op_ctx->export_perms.anonymous_uid) ?
                            0 : cred->caller_uid;
    gid_t gid = (cred->caller_gid == op_ctx->export_perms.anonymous_gid) ?
                            0 : cred->caller_gid;

    ctx = liz_create_user_context(uid, gid, 0, 0);
    if (!ctx) {
        return NULL;
    }

    if (cred->caller_glen > 0) {
        if (cred->caller_glen > kLocalGArraySize) {
            gid_t *garray = malloc((cred->caller_glen + 1) *
                            sizeof(gid_t));

            if (garray != NULL) {
                garray[0] = gid;
                memcpy(garray + 1, cred->caller_garray,
                       sizeof(gid_t) * cred->caller_glen);
                liz_update_groups(instance, ctx, garray,
                                  cred->caller_glen + 1);
                free(garray);
                return ctx;
            }
        }

        gid_t garray[kLocalGArraySize + 1];

        garray[0] = gid;
        int count = MIN(cred->caller_glen, kLocalGArraySize);

        memcpy(garray + 1, cred->caller_garray, sizeof(gid_t) * count);
        liz_update_groups(instance, ctx, garray, count + 1);
    }

    return ctx;
}

bool set_credentials(const struct user_cred *creds,
                     const struct fsal_module *fsal_module)
{
    bool only_one_user = container_of(fsal_module,
                                      struct lzfs_fsal_module,
                                      module)->only_one_user;

    if (only_one_user)
        return fsal_set_credentials_only_one_user(creds);
    else {
        fsal_set_credentials(creds);
        return true;
    }
}

void restore_ganesha_credentials(const struct fsal_module *fsal_module)
{
    bool only_one_user = container_of(fsal_module,
                                      struct lzfs_fsal_module,
                                      module)->only_one_user;

    if (!only_one_user) {
        fsal_restore_ganesha_credentials();
    }
}

nfsstat4 lizardfs2nfs4_error(int ec)
{
    if (!ec) {
        LogWarn(COMPONENT_FSAL, "appropriate errno not set");
        ec = EINVAL;
    }
    return posix2nfs4_error(liz_error_conv(ec));
}

fsal_status_t lizardfs2fsal_error(int ec)
{
    fsal_status_t status;

    if (!ec) {
        LogWarn(COMPONENT_FSAL, "appropriate errno not set");
        ec = EINVAL;
    }

    status.minor = ec;
    status.major = posix2fsal_error(liz_error_conv(ec));

    return status;
}

fsal_status_t lzfs_fsal_last_err(void)
{
    return lizardfs2fsal_error(liz_last_err());
}

nfsstat4 lzfs_nfs4_last_err(void)
{
    return lizardfs2nfs4_error(liz_last_err());
}
