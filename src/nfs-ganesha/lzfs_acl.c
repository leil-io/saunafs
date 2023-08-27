/*
   Copyright 2017 Skytechnology sp. z o.o.

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

#include "context_wrap.h"
#include "lzfs_fsal_methods.h"

/**
 * @brief Convert an FSAL ACL to a LizardFS ACL.
 *
 * @param[in] fsalACL     FSAL ACL
 * @param[in] mode        Mode used to create the acl and set POSIX permission flags
 *
 * @returns: Converted LizardFS ACL
 */

const uint ByteMaxValue = 0xFF;

liz_acl_t *convertFsalACLToNativeACL(const fsal_acl_t *fsalACL,
                                     unsigned int mode) {
	liz_acl_t *nativeACL = NULL;

	if (!fsalACL || (!fsalACL->aces && fsalACL->naces > 0)) {
		return NULL;
	}

	nativeACL = liz_create_acl_from_mode(mode);
	if (!nativeACL) {
		return NULL;
	}

	for (unsigned int i = 0; i < fsalACL->naces; ++i) {
		fsal_ace_t *fsalACE = fsalACL->aces + i;

		if (!(IS_FSAL_ACE_ALLOW(*fsalACE) ||
		      IS_FSAL_ACE_DENY(*fsalACE))) {
			continue;
		}

		liz_acl_ace_t ace;
		ace.flags = fsalACE->flag & ByteMaxValue;
		ace.mask  = fsalACE->perm;
		ace.type  = fsalACE->type;

		if (IS_FSAL_ACE_GROUP_ID(*fsalACE)) {
			ace.id = GET_FSAL_ACE_GROUP(*fsalACE);
		}
		else {
			ace.id = GET_FSAL_ACE_USER(*fsalACE);
		}

		if (IS_FSAL_ACE_SPECIAL_ID(*fsalACE)) {
			ace.flags |= (uint)LIZ_ACL_SPECIAL_WHO;
			switch (GET_FSAL_ACE_USER(*fsalACE)) {
			case FSAL_ACE_SPECIAL_OWNER:
				ace.id = LIZ_ACL_OWNER_SPECIAL_ID;
				break;
			case FSAL_ACE_SPECIAL_GROUP:
				ace.id = LIZ_ACL_GROUP_SPECIAL_ID;
				break;
			case FSAL_ACE_SPECIAL_EVERYONE:
				ace.id = LIZ_ACL_EVERYONE_SPECIAL_ID;
				break;
			default:
				LogFullDebug(COMPONENT_FSAL,
				             "Invalid FSAL ACE special id type (%d)",
				             (int)GET_FSAL_ACE_USER(*fsalACE));
				continue;
			}
		}

		liz_add_acl_entry(nativeACL, &ace);
	}

	return nativeACL;
}

/**
 * @brief Convert a LizardFS ACL to an FSAL ACL.
 *
 * @param[in] nativeACL     LizardFS ACL
 *
 * @returns: Converted FSAL ACL
 */
fsal_acl_t *convertNativeACLToFsalACL(const liz_acl_t *nativeACL) {
	fsal_acl_data_t ACLData;
	fsal_acl_status_t ACLStatus = 0;
	fsal_acl_t *fsalACL = NULL;

	if (!nativeACL) {
		return NULL;
	}

	ACLData.naces = liz_get_acl_size(nativeACL);
	ACLData.aces = nfs4_ace_alloc(ACLData.naces);

	if (!ACLData.aces) {
		return NULL;
	}

	for (size_t aceIterator = 0; aceIterator < ACLData.naces; ++aceIterator) {
		fsal_ace_t *fsalACE = ACLData.aces + aceIterator;
		liz_acl_ace_t nativeACE;

		int retvalue = liz_get_acl_entry(nativeACL, aceIterator, &nativeACE);

		(void) retvalue;
		assert(retvalue == 0);

		fsalACE->type  = nativeACE.type;
		fsalACE->flag  = nativeACE.flags & ByteMaxValue;
		fsalACE->iflag = (nativeACE.flags & (unsigned)LIZ_ACL_SPECIAL_WHO) ?
		            FSAL_ACE_IFLAG_SPECIAL_ID : 0;
		fsalACE->perm = nativeACE.mask;

		if (IS_FSAL_ACE_GROUP_ID(*fsalACE)) {
			fsalACE->who.gid = nativeACE.id;
		}
		else {
			fsalACE->who.uid = nativeACE.id;
		}

		if (IS_FSAL_ACE_SPECIAL_ID(*fsalACE)) {
			switch (nativeACE.id) {
			case LIZ_ACL_OWNER_SPECIAL_ID:
				fsalACE->who.uid = FSAL_ACE_SPECIAL_OWNER;
				break;
			case LIZ_ACL_GROUP_SPECIAL_ID:
				fsalACE->who.uid = FSAL_ACE_SPECIAL_GROUP;
				break;
			case LIZ_ACL_EVERYONE_SPECIAL_ID:
				fsalACE->who.uid = FSAL_ACE_SPECIAL_EVERYONE;
				break;
			default:
				fsalACE->who.uid = FSAL_ACE_NORMAL_WHO;
				LogWarn(COMPONENT_FSAL,
				        "Invalid LizardFS ACE special id type (%u)",
				        (unsigned int)nativeACE.id);
			}
		}
	}

	fsalACL = nfs4_acl_new_entry(&ACLData, &ACLStatus);
	return fsalACL;
}

/**
 * @brief Get ACL from a file.
 *
 * This function returns the FSAL ACL of the file.
 *
 * @param[in] export       LizardFS export instance
 * @param[in] inode        inode of the file
 * @param[in] ownerId      ownerId of the file
 * @param[in] fsalACL      Buffer to fill with information
 *
 * @returns: FSAL status.
 */
fsal_status_t getACL(struct FSExport *export, uint32_t inode, uint32_t ownerId,
                     fsal_acl_t **fsalACL) {
	if (*fsalACL) {
		nfs4_acl_release_entry(*fsalACL);
		*fsalACL = NULL;
	}

	liz_acl_t *nativeACL = NULL;
	int ret = fs_getacl(export->fsInstance, &op_ctx->creds, inode, &nativeACL);

	if (ret < 0) {
		LogFullDebug(COMPONENT_FSAL,
		             "getacl status = %s export=%" PRIu16 " inode=%" PRIu32,
		             liz_error_string(liz_last_err()), export->export.export_id,
		             inode);

		return fsalLastError();
	}

	liz_acl_apply_masks(nativeACL, ownerId);

	*fsalACL = convertNativeACLToFsalACL(nativeACL);
	liz_destroy_acl(nativeACL);

	if (*fsalACL == NULL) {
		LogFullDebug(COMPONENT_FSAL,
		             "Failed to convert lzfs acl to nfs4 acl, export=%"
		             PRIu16 " inode=%" PRIu32,
		             export->export.export_id, inode);
		return fsalstat(ERR_FSAL_FAULT, 0);
	}

	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/**
 * @brief Set ACL to a file.
 *
 * This function sets the native ACL to a file from the FSAL ACL.
 *
 * @param[in] export      LizardFS export instance
 * @param[in] inode       inode of the file
 * @param[in] fsalACL     FSAL ACL to set
 * @param[in] mode        Mode used to create the acl and set POSIX permission flags
 *
 * @returns: FSAL status.
 */
fsal_status_t setACL(struct FSExport *export, uint32_t inode,
                     const fsal_acl_t *fsalACL, unsigned int mode) {
	if (!fsalACL) {
		return fsalstat(ERR_FSAL_NO_ERROR, 0);
	}

	liz_acl_t *nativeACL = convertFsalACLToNativeACL(fsalACL, mode);

	if (!nativeACL) {
		LogFullDebug(COMPONENT_FSAL, "Failed to convert acl");
		return fsalstat(ERR_FSAL_FAULT, 0);
	}

	int ret = fs_setacl(export->fsInstance, &op_ctx->creds, inode, nativeACL);
	liz_destroy_acl(nativeACL);

	if (ret < 0) {
		return fsalLastError();
	}

	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}
