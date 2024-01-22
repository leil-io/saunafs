/**
 * @file   saunafs_internal.c
 * @author Crash <crash@leil.io>
 *
 * @brief Function definitions for SaunaFS FSAL
 */
#include "fsal_convert.h"
#include "pnfs_utils.h"

#include "saunafs_internal.h"

sau_context_t *createContext(sau_t *instance, struct user_cred *cred) {
	if (cred == NULL) {
		return sau_create_user_context(0, 0, 0, 0);
	}

	uid_t uid = (cred->caller_uid == op_ctx->export_perms.anonymous_uid)
	        ? 0 : cred->caller_uid;
	gid_t gid = (cred->caller_gid == op_ctx->export_perms.anonymous_gid)
	        ? 0 : cred->caller_gid;

	sau_context_t *ctx = sau_create_user_context(uid, gid, 0, 0);
	if (!ctx) {
		return NULL;
	}

	if (cred->caller_glen > 0) {
		gid_t *garray = malloc((cred->caller_glen + 1) * sizeof(gid_t));

		if (garray != NULL) {
			garray[0] = gid;
			size_t size = sizeof(gid_t) * cred->caller_glen;
			memcpy(garray + 1, cred->caller_garray, size);
			sau_update_groups(instance, ctx, garray, cred->caller_glen + 1);
			free(garray);
		}
	}

	return ctx;
}

nfsstat4 saunafsToNfs4Error(int errorCode) {
	if (!errorCode) {
		LogWarn(COMPONENT_FSAL, "appropriate errno not set");
		errorCode = EINVAL;
	}

	return posix2nfs4_error(sau_error_conv(errorCode));
}

fsal_status_t saunafsToFsalError(int errorCode) {
	fsal_status_t status;

	if (!errorCode) {
		LogWarn(COMPONENT_FSAL, "appropriate errno not set");
		errorCode = EINVAL;
	}

	status.minor = errorCode;
	status.major = posix2fsal_error(sau_error_conv(errorCode));

	return status;
}

fsal_status_t fsalLastError(void) {
	return saunafsToFsalError(sau_last_err());
}

nfsstat4 nfs4LastError(void) {
	return saunafsToNfs4Error(sau_last_err());
}
