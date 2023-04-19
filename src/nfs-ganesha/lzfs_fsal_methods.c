#include "fsal_convert.h"
#include "pnfs_utils.h"

#include "lzfs_fsal_methods.h"

liz_context_t *createFSALContext(liz_t *instance, struct user_cred *cred) {
	if (cred == NULL) {
		return liz_create_user_context(0, 0, 0, 0);
	}

	uid_t uid = (cred->caller_uid == op_ctx->export_perms.anonymous_uid)
	        ? 0 : cred->caller_uid;
	gid_t gid = (cred->caller_gid == op_ctx->export_perms.anonymous_gid)
	        ? 0 : cred->caller_gid;

	liz_context_t *ctx = liz_create_user_context(uid, gid, 0, 0);
	if (!ctx) {
		return NULL;
	}

	if (cred->caller_glen > 0) {
		gid_t *garray = malloc((cred->caller_glen + 1) * sizeof(gid_t));

		if (garray != NULL) {
			garray[0] = gid;
			size_t size = sizeof(gid_t) * cred->caller_glen;
			memcpy(garray + 1, cred->caller_garray, size);
			liz_update_groups(instance, ctx, garray, cred->caller_glen + 1);
			free(garray);
		}
	}

	return ctx;
}

bool setCredentials(const struct user_cred *creds,
                    const struct fsal_module *fsal_module) {
	bool onlyOneUser = container_of(fsal_module, struct FSModule,
	                                module)->onlyOneUser;

	if (onlyOneUser)
		return fsal_set_credentials_only_one_user(creds);

	fsal_set_credentials(creds);
	return true;
}

void restoreGaneshaCredentials(const struct fsal_module *fsal_module) {
	bool onlyOneUser = container_of(fsal_module, struct FSModule,
	                                module)->onlyOneUser;

	if (!onlyOneUser) {
		fsal_restore_ganesha_credentials();
	}
}

nfsstat4 lizardfsToNfs4Error(int ec) {
	if (!ec) {
		LogWarn(COMPONENT_FSAL, "appropriate errno not set");
		ec = EINVAL;
	}

	return posix2nfs4_error(liz_error_conv(ec));
}

fsal_status_t lizardfsToFsalError(int ec) {
	fsal_status_t status;

	if (!ec) {
		LogWarn(COMPONENT_FSAL, "appropriate errno not set");
		ec = EINVAL;
	}

	status.minor = ec;
	status.major = posix2fsal_error(liz_error_conv(ec));

	return status;
}

fsal_status_t fsalLastError(void) {
	return lizardfsToFsalError(liz_last_err());
}

nfsstat4 Nfs4LastError(void) {
	return lizardfsToNfs4Error(liz_last_err());
}
