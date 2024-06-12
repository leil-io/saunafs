/*
   Copyright 2017 Skytechnology sp. z o.o.
   Copyright 2023 Leil Storage OÜ

   This file is part of SaunaFS.

   SaunaFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   SaunaFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with SaunaFS. If not, see <http://www.gnu.org/licenses/>.
 */
#include "config.h"
#include "fsal_types.h"

#ifdef LINUX
#include <linux/falloc.h>  /* for fallocate  */
#include <sys/sysmacros.h> /* for makedev(3) */
#endif

#include "fsal.h"
#include "fsal_convert.h"
#include "FSAL/fsal_commonlib.h"

#include "errors/saunafs_error_codes.h"
#include "context_wrap.h"
#include "saunafs_internal.h"

/**
 * @brief Clean up a filehandle.
 *
 * This function cleans up private resources associated with a filehandle and
 * deallocates it.
 *
 * Implement this method or you will leak. Refcount (if used) should be 1.
 *
 * @param[in] objectHandle     Handle to release
 */
static void release(struct fsal_obj_handle *objectHandle) {
	struct SaunaFSHandle *handle = NULL;
	handle = container_of(objectHandle, struct SaunaFSHandle, handle);

	if (handle != handle->export->root) {
		deleteHandle(handle);
	}
}

/**
 * @brief Look up a filename.
 *
 * Directory operations.
 * This function looks up the given name in the supplied directory.
 *
 * @param [in]     dirHandle        Directory to search
 * @param [in]     path             Name to look up
 * @param [out]    objectHandle     Object found
 * @param [in,out] attributes       Optional attributes for newly created object
 *
 * \see fsal_api.h for more information
 *
 * @returns: FSAL status
 */
static fsal_status_t lookup(struct fsal_obj_handle *dirHandle, const char *path,
                            struct fsal_obj_handle **objectHandle,
                            struct fsal_attrlist *attributes) {
	struct SaunaFSExport *export = NULL;
	struct SaunaFSHandle *directory = NULL;
	struct sau_entry node;

	export = container_of(op_ctx->fsal_export, struct SaunaFSExport, export);
	directory = container_of(dirHandle, struct SaunaFSHandle, handle);

	int status = saunafs_lookup(export->fsInstance, &op_ctx->creds,
	                            directory->inode, path, &node);

	if (status < 0) {
		return fsalLastError();
	}

	if (attributes != NULL) {
		posix2fsal_attributes_all(&node.attr, attributes);
	}

	struct SaunaFSHandle *handle = allocateHandle(&node.attr, export);
	*objectHandle = &handle->handle;

	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/**
 * @brief Read a directory.
 *
 * This function reads directory entries from the FSAL and supplies them to a callback.
 *
 * @param [in]  dirHandle          Directory to read
 * @param [in]  whence             Point at which to start reading. NULL to start at beginning
 * @param [in]  dirState           Opaque pointer to be passed to callback
 * @param [in]  readdirCb          Callback to receive names
 * @param [in]  attributesMask     Indicate which attributes the caller is interested in
 * @param [out] eof                true if the last entry was reached
 *
 * @returns: FSAL status
 */
static fsal_status_t readdir_(struct fsal_obj_handle *dirHandle,
                              fsal_cookie_t *whence, void *dirState,
                              fsal_readdir_cb readdirCb,
                              attrmask_t attributesMask, bool *eof) {
	static const int batchSize = 100;
	struct sau_direntry buffer[batchSize];

	struct SaunaFSExport *export = NULL;
	struct SaunaFSHandle *directory = NULL;
	struct SaunaFSHandle *handle = NULL;

	struct fsal_attrlist attributes;
	off_t direntryOffset = 0;

	enum fsal_dir_result result = DIR_CONTINUE;
	int status = 0;

	export = container_of(op_ctx->fsal_export, struct SaunaFSExport, export);
	directory = container_of(dirHandle, struct SaunaFSHandle, handle);

	sau_context_t *context __attribute__((cleanup(sau_destroy_context))) = NULL;
	context = createContext(export->fsInstance, &op_ctx->creds);

	struct sau_fileinfo *saunafsFd = NULL;
	saunafsFd = sau_opendir(export->fsInstance, context, directory->inode);

	if (!saunafsFd) {
		return fsalLastError();
	}

	if (whence != NULL) {
		direntryOffset = *whence;
	}

	while (true) {
		size_t totalEntries = 0;
		size_t entry = 0;

		status = sau_readdir(export->fsInstance, context, saunafsFd,
		                     direntryOffset, batchSize, buffer, &totalEntries);

		if (status < 0) {
			return fsalLastError();
		}

		result = DIR_CONTINUE;
		for (entry = 0; entry < totalEntries && result != DIR_TERMINATE;
		     ++entry) {
			if (strcmp(buffer[entry].name, ".")  == 0 ||
			    strcmp(buffer[entry].name, "..") == 0) {
				continue;
			}

			handle = allocateHandle(&buffer[entry].attr, export);

			fsal_prepare_attrs(&attributes, attributesMask);
			posix2fsal_attributes_all(&buffer[entry].attr, &attributes);

			direntryOffset = buffer[entry].next_entry_offset;

			result = readdirCb(buffer[entry].name, &handle->handle,
			                   &attributes, dirState, direntryOffset + 1);

			fsal_release_attrs(&attributes);
		}

		sau_destroy_direntry(buffer, totalEntries);
		*eof = (totalEntries < batchSize) && (entry == totalEntries);

		if (result != DIR_CONTINUE || totalEntries < batchSize) {
			break;
		}
	}

	status = sau_releasedir(export->fsInstance, saunafsFd);

	if (status < 0) {
		return fsalLastError();
	}

	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/**
 * @brief Get attributes.
 *
 * This function fetches the attributes for the object.
 * The attributes requested in the mask are copied out
 * (though other attributes might be copied out).
 *
 * @param [in]  objectHandle       Object to query
 * @param [out] attributes         Attribute list for file
 *
 * \see fsal_api.h for more information
 *
 * @returns: FSAL status
 */
static fsal_status_t getattrs(struct fsal_obj_handle *objectHandle,
                              struct fsal_attrlist *attributes) {
	struct SaunaFSExport *export = NULL;
	struct SaunaFSHandle *handle = NULL;
	struct sau_attr_reply posixAttributes;

	export = container_of(op_ctx->fsal_export, struct SaunaFSExport, export);
	handle = container_of(objectHandle, struct SaunaFSHandle, handle);

	LogFullDebug(COMPONENT_FSAL, " export = %" PRIu16 " inode = %" PRIu32,
	             export->export.export_id, handle->inode);

	int status = saunafs_getattr(export->fsInstance, &op_ctx->creds,
	                             handle->inode, &posixAttributes);

	if (status < 0) {
		if (attributes->request_mask & ATTR_RDATTR_ERR) {
			attributes->valid_mask = ATTR_RDATTR_ERR;
		}
		return fsalLastError();
	}

	posix2fsal_attributes_all(&posixAttributes.attr, attributes);

#ifdef ENABLE_NFS_ACL_SUPPORT
	if (attributes->request_mask & (attrmask_t)ATTR_ACL) {
		fsal_status_t status =
		    getACL(export, handle->inode, posixAttributes.attr.st_uid,
		           &attributes->acl);

		if (!FSAL_IS_ERROR(status)) {
			attributes->valid_mask |= (attrmask_t)ATTR_ACL;
		}
	}
#endif

	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/**
 * @brief Write wire handle.
 *
 * Handle operations.
 * This function writes a "wire" handle or file ID to the given buffer.
 *
 * @param [in]     objectHandle       The handle to digest
 * @param [in]     outputType         The type of digest to write
 * @param [in,out] buffer             Buffer descriptor to which to write digest.
 *                                    Set fh_desc->len to final output length.
 *
 * @returns: FSAL status
 */
static fsal_status_t handle_to_wire(const struct fsal_obj_handle *objectHandle,
                                    uint32_t outputType,
                                    struct gsh_buffdesc *buffer) {
	(void)outputType;

	struct SaunaFSHandle *handle = NULL;
	handle = container_of(objectHandle, struct SaunaFSHandle, handle);

	sau_inode_t inode = handle->inode;
	if (buffer->len < sizeof(sau_inode_t)) {
		LogMajor(COMPONENT_FSAL,
		         "Space too small for handle. Need  %zu, have %zu",
		         sizeof(sau_inode_t), buffer->len);
		return fsalstat(ERR_FSAL_TOOSMALL, 0);
	}

	memcpy(buffer->addr, &inode, sizeof(sau_inode_t));
	buffer->len = sizeof(inode);

	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/**
 * @brief Get key for handle.
 *
 * Indicate the unique part of the handle that should be used for hashing.
 *
 * @param [in]  objectHandle     Handle whose key is to be got
 * @param [out] buffer           Address and length giving sub-region of handle
 *                               to be used as key.
 *
 * @returns: FSAL status
 */
static void handle_to_key(struct fsal_obj_handle *objectHandle,
                          struct gsh_buffdesc *buffer) {
	struct SaunaFSHandle *handle = NULL;
	handle = container_of(objectHandle, struct SaunaFSHandle, handle);

	buffer->addr = &handle->key;
	buffer->len = sizeof(struct SaunaFSHandleKey);
}

/**
 * @brief Open a SaunaFS file descriptor.
 *
 * @param [in] handle            SaunaFS internal object handle
 * @param [in] openflags         Mode for open
 * @param [in] saunafsFd         SaunaFS file descriptor to open
 * @param [in] noAccessCheck     Whether the caller checked access or not
 *
 * @returns: FSAL status
 */
static fsal_status_t openFileDescriptor(struct SaunaFSHandle *handle,
                                        fsal_openflags_t openflags,
                                        struct SaunaFSFd *saunafsFd,
                                        bool noAccessCheck) {
	struct SaunaFSExport *export = NULL;
	int posixFlags = 0;

	fsal2posix_openflags(openflags, &posixFlags);
	if (noAccessCheck) {
		posixFlags |= O_CREAT;
	}

	export = container_of(op_ctx->fsal_export, struct SaunaFSExport, export);

	LogFullDebug(COMPONENT_FSAL,
	             "fd = %p fd->fd = %p openflags = %x, posix_flags = %x",
	             saunafsFd, saunafsFd->fd, openflags, posixFlags);

	assert(saunafsFd->fd == NULL && saunafsFd->openflags == FSAL_O_CLOSED &&
	       openflags != 0);

	saunafsFd->fd = saunafs_open(export->fsInstance, &op_ctx->creds,
	                             handle->inode, posixFlags);

	if (!saunafsFd->fd) {
		LogFullDebug(COMPONENT_FSAL, "open failed with %s",
		             sau_error_string(sau_last_err()));
		return fsalLastError();
	}

	saunafsFd->openflags = openflags;
	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/**
 * @brief Close a SaunaFS file descriptor.
 *
 * @param [in] handle         SaunaFS internal object handle
 * @param [in] saunafsFd      SaunaFS file descriptor to open
 *
 * @returns: FSAL status
 */
static fsal_status_t closeFileDescriptor(struct SaunaFSHandle *handle,
                                         struct SaunaFSFd *saunafsFd) {
	if (saunafsFd->fd != NULL && saunafsFd->openflags != FSAL_O_CLOSED) {
		int status = sau_release(handle->export->fsInstance, saunafsFd->fd);

		saunafsFd->fd = NULL;
		saunafsFd->openflags = FSAL_O_CLOSED;

		if (status < 0) {
			return fsalLastError();
		}
	}

	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/**
 * @brief Open a file using its handle.
 *
 * @param [in] objectHandle                  File handle to open
 * @param [in] state                         state_t to use for this operation
 * @param [in] openflags                     Mode for open
 * @param [in] createmode                    Mode for create
 * @param [in] verifier                      Verifier to use for exclusive create
 * @param [in] attributes                    Attributes to set on created file
 * @param [in,out] callerPermissionCheck     The caller must do a permission check
 * @param [in] afterMknode                   The file is opened after performing
 *                                           a makenode operation
 *
 * @returns: FSAL status
 */
static fsal_status_t openByHandle(struct fsal_obj_handle *objectHandle,
                                  struct state_t *state,
                                  fsal_openflags_t openflags,
                                  enum fsal_create_mode createmode,
                                  fsal_verifier_t verifier,
                                  struct fsal_attrlist *attributes,
                                  bool *callerPermissionCheck,
                                  bool afterMknode) {
	struct SaunaFSExport *export = NULL;
	struct SaunaFSHandle *handle = NULL;
	struct SaunaFSFd *saunafsFd = NULL;
	fsal_status_t status = fsalstat(ERR_FSAL_NO_ERROR, 0);
	int posixFlags = 0;

	handle = container_of(objectHandle, struct SaunaFSHandle, handle);
	export = container_of(op_ctx->fsal_export, struct SaunaFSExport, export);

	PTHREAD_RWLOCK_wrlock(&objectHandle->obj_lock);

	if (state != NULL) {
		saunafsFd =
		    &container_of(state, struct SaunaFSStateFd, state)->saunafsFd;

		// Prepare to take the share reservation, but only if we
		// are called with a valid state

		// Check share reservation conflicts
		status = check_share_conflict(&handle->share, openflags, false);

		if (FSAL_IS_ERROR(status)) {
			PTHREAD_RWLOCK_unlock(&objectHandle->obj_lock);
			return status;
		}

		// Take the share reservation now by updating the counters
		update_share_counters(&handle->share, FSAL_O_CLOSED, openflags);
		PTHREAD_RWLOCK_unlock(&objectHandle->obj_lock);
	}
	else {
		// We need to use the global file descriptor to continue
		saunafsFd = &handle->fd;
	}

	status = openFileDescriptor(handle, openflags, saunafsFd, afterMknode);
	if (FSAL_IS_ERROR(status)) {
		if (state != NULL) {
			// On error we need to release our share reservation
			// and undo the update of the share counters
			PTHREAD_RWLOCK_wrlock(&objectHandle->obj_lock);
			update_share_counters(&handle->share, openflags, FSAL_O_CLOSED);
			PTHREAD_RWLOCK_unlock(&objectHandle->obj_lock);

			return status;
		}

		PTHREAD_RWLOCK_unlock(&objectHandle->obj_lock);
		return status;
	}

	fsal2posix_openflags(openflags, &posixFlags);
	bool truncated = (posixFlags & O_TRUNC) != 0;

	bool validFile = (createmode >= FSAL_EXCLUSIVE || truncated || attributes);

	if (validFile) {
		struct sau_attr_reply safs_attrs;

		int retvalue = saunafs_getattr(export->fsInstance, &op_ctx->creds,
		                               handle->inode, &safs_attrs);

		if (retvalue == 0) {
			LogFullDebug(COMPONENT_FSAL, "New size = %" PRIx64,
			             (int64_t)safs_attrs.attr.st_size);
		}
		else {
			status = fsalLastError();
		}

		if (!FSAL_IS_ERROR(status)) {
			// Now check verifier for exclusive
			if (createmode >= FSAL_EXCLUSIVE &&
			    createmode != FSAL_EXCLUSIVE_9P &&
			    !check_verifier_stat(&safs_attrs.attr, verifier, false)) {
				// Verifier didn't match, return EEXIST
				status = fsalstat(posix2fsal_error(EEXIST), EEXIST);
			}
		}

		if (!FSAL_IS_ERROR(status) && attributes) {
			posix2fsal_attributes_all(&safs_attrs.attr, attributes);
		}
	}

	if (state == NULL) {
		PTHREAD_RWLOCK_unlock(&objectHandle->obj_lock);

		// If success, we haven't done any permission check so ask the
		// caller to do so
		*callerPermissionCheck = !FSAL_IS_ERROR(status);

		// If no state, return status
		return status;
	}

	if (!FSAL_IS_ERROR(status)) {
		// Return success. We haven't done any permission check so ask
		// the caller to do so.
		*callerPermissionCheck = true;
		return status;
	}

	closeFileDescriptor(handle, saunafsFd);

	// Release our share reservation and undo the update of the share counters
	PTHREAD_RWLOCK_wrlock(&objectHandle->obj_lock);
	update_share_counters(&handle->share, openflags, FSAL_O_CLOSED);
	PTHREAD_RWLOCK_unlock(&objectHandle->obj_lock);

	return status;
}

/**
 * @brief Open a file using its name.
 *
 * @param [in] objectHandle                  File handle to open
 * @param [in] state                         state_t to use for this operation
 * @param [in] openflags                     Mode for open
 * @param [in] name                          Name of the file
 * @param [in] verifier                      Verifier to use for exclusive create
 * @param [in] attributes                    Attributes to set on created file
 * @param [in,out] callerPermissionCheck     The caller must do a permission check
 * @param [in] afterMknode                   The file is opened after performing
 *                                           a makenode operation
 *
 * @returns: FSAL status
 */
static fsal_status_t openByName(struct fsal_obj_handle *objectHandle,
                                struct state_t *state,
                                fsal_openflags_t openflags,
                                const char *name,
                                fsal_verifier_t verifier,
                                struct fsal_attrlist *attributes,
                                bool *callerPermissionCheck) {
	struct fsal_obj_handle *temp = NULL;
	fsal_status_t status;

	// Ganesha doesn't has open by name, so we need to get the name with lookup
	status = objectHandle->obj_ops->lookup(objectHandle, name, &temp, NULL);

	if (FSAL_IS_ERROR(status)) {
		LogFullDebug(COMPONENT_FSAL, "lookup returned %s",
		             fsal_err_txt(status));
		return status;
	}

	status = openByHandle(temp, state, openflags, FSAL_NO_CREATE, verifier,
	                      attributes, callerPermissionCheck, false);

	if (FSAL_IS_ERROR(status)) {
		temp->obj_ops->release(temp);
		LogFullDebug(COMPONENT_FSAL, "open returned %s",
		             fsal_err_txt(status));
	}

	return status;
}

/**
 * @brief Open a file descriptor for read or write and possibly create.
 *
 * Extended API functions.
 * With these new operations, the FSAL becomes responsible for managing share
 * reservations. The FSAL is also granted more control over the state of a "file
 * descriptor" and has more control of what a "file descriptor" even is.
 * Ultimately, it is whatever the FSAL needs in order to manage the share
 * reservations and lock state.

 * The open2 method also allows atomic create/setattr/open. This function opens
 * a file for read or write, possibly creating it. If the caller is passing a
 * state, it must hold the state_lock exclusive.
 *
 * @param [in]  objectHandle                 File to open or parent directory
 * @param [in]  state                        state_t to use for this operation
 * @param [in]  openflags                    Mode for open
 * @param [in]  createmode                   Mode for create
 * @param [in]  name                         Name for file if being created or
 *                                           opened
 * @param [in]  attributesToSet              Attributes to set on created file
 * @param [in]  verifier                     Verifier to use for exclusive
 *                                           create
 * @param [in,out] createdObject             Newly created object
 * @param [in,out] attributes                Optional attributes for newly
                                             created object
 * @param [in,out] callerPermissionCheck     The caller must do a permission
 *                                           check
 *
 * @returns: FSAL status
 */
static fsal_status_t open2(struct fsal_obj_handle *objectHandle,
                           struct state_t *state, fsal_openflags_t openflags,
                           enum fsal_create_mode createmode, const char *name,
                           struct fsal_attrlist *attributesToSet,
                           fsal_verifier_t verifier,
                           struct fsal_obj_handle **createdObject,
                           struct fsal_attrlist *attributes,
                           bool *callerPermissionCheck) {
	struct SaunaFSExport *export = NULL;
	struct SaunaFSHandle *handle = NULL;
	fsal_status_t status = fsalstat(ERR_FSAL_NO_ERROR, 0);

	LogAttrlist(COMPONENT_FSAL, NIV_FULL_DEBUG, "attrs ", attributesToSet,
	            false);

	if (createmode >= FSAL_EXCLUSIVE) {
		// Now fixup attrs for verifier if exclusive create
		set_common_verifier(attributesToSet, verifier, false);
	}

	if (name == NULL) {
		return openByHandle(objectHandle, state, openflags, createmode,
		                    verifier, attributes, callerPermissionCheck, false);
	}

	if (createmode == FSAL_NO_CREATE) {
		return openByName(objectHandle, state, openflags, name, verifier,
		                  attributes, callerPermissionCheck);
	}

	// Create file
	export = container_of(op_ctx->fsal_export, struct SaunaFSExport, export);
	handle = container_of(objectHandle, struct SaunaFSHandle, handle);

	// Fetch the mode attribute to use in the openat system call
	mode_t unix_mode = fsal2unix_mode(attributesToSet->mode) &
	        ~op_ctx->fsal_export->exp_ops.fs_umask(op_ctx->fsal_export);

	// Don't set the mode if we later set the attributes
	FSAL_UNSET_MASK(attributesToSet->valid_mask, (attrmask_t)ATTR_MODE);

	struct sau_entry posixAttributes;
	int retval =
	    saunafs_mknode(export->fsInstance, &op_ctx->creds, handle->inode, name,
	                   unix_mode, 0, &posixAttributes);

	if (retval < 0 && sau_last_err() == SAUNAFS_ERROR_EEXIST &&
	    createmode == FSAL_UNCHECKED) {
		return openByName(objectHandle, state, openflags, name, verifier,
		                  attributes, callerPermissionCheck);
	}

	if (retval < 0) { return fsalLastError(); }

	// File has been created by us.
	*callerPermissionCheck = false;

	struct SaunaFSHandle *newHandle =
	    allocateHandle(&posixAttributes.attr, export);

	if (newHandle == NULL) {
		(*createdObject)->obj_ops->release(*createdObject);
		*createdObject = NULL;

		retval = saunafs_unlink(export->fsInstance, &op_ctx->creds,
		                        handle->inode, name);

		if (retval < 0) { return fsalLastError(); }

		return fsalstat(posix2fsal_error(ENOMEM), ENOMEM);
	}

	*createdObject = &newHandle->handle;

	if (attributesToSet->valid_mask != 0) {
		status = (*createdObject)->obj_ops->setattr2(*createdObject, false,
		                                             state, attributesToSet);
		if (FSAL_IS_ERROR(status)) {
			(*createdObject)->obj_ops->release(*createdObject);
			*createdObject = NULL;

			retval = saunafs_unlink(export->fsInstance, &op_ctx->creds,
			                        handle->inode, name);

			if (retval < 0) { return fsalLastError(); }

			return status;
		}

		if (attributes != NULL) {
			status =
			    (*createdObject)->obj_ops->getattrs(*createdObject, attributes);

			if (FSAL_IS_ERROR(status) &&
			    (attributes->request_mask & ATTR_RDATTR_ERR) == 0) {
				(*createdObject)->obj_ops->release(*createdObject);
				*createdObject = NULL;

				retval = saunafs_unlink(export->fsInstance, &op_ctx->creds,
				                        handle->inode, name);

				if (retval < 0) { return fsalLastError(); }

				return status;
			}

			attributes = NULL;
		}
	}

	if (attributes != NULL) {
		posix2fsal_attributes_all(&posixAttributes.attr, attributes);
	}

	return openByHandle(*createdObject, state, openflags, createmode, verifier,
	                    NULL, callerPermissionCheck, true);
}

/**
 * @brief Function to open an fsal_obj_handle's global file descriptor.
 *
 * @param[in]  objectHandle       File on which to operate
 * @param[in]  openflags          Mode for open
 * @param[out] fileDescriptor     File descriptor that is to be used
 *
 * @return FSAL status.
 */
static fsal_status_t openFunction(struct fsal_obj_handle *objectHandle,
                                  fsal_openflags_t openflags,
                                  struct fsal_fd *fileDescriptor) {
	struct SaunaFSHandle *handle = NULL;
	handle = container_of(objectHandle, struct SaunaFSHandle, handle);
	return openFileDescriptor(handle, openflags,
	                          (struct SaunaFSFd *)fileDescriptor, true);
}

/**
 * @brief Function to close an fsal_obj_handle's global file descriptor.
 *
 * @param[in] objectHandle       File on which to operate
 * @param[in] fileDescriptor     File handle to close
 *
 * @return FSAL status.
 */
static fsal_status_t closeFunction(struct fsal_obj_handle *objectHandle,
                                   struct fsal_fd *fileDescriptor) {
	struct SaunaFSHandle *handle = NULL;
	handle = container_of(objectHandle, struct SaunaFSHandle, handle);
	return closeFileDescriptor(handle, (struct SaunaFSFd *)fileDescriptor);
}

/**
 * @brief Find a usable file descriptor for a regular file.
 *
 * This function is a wrapper that initializes the needed variables before calling
 * fsal_find_fd function passing the expected attributes.
 *
 * @param[in,out] fileDescriptor       Usable file descriptor found
 * @param[in] objectHandle             File on which to operate
 * @param[in] bypass                   If state doesn't indicate a share reservation, bypass any deny read
 * @param[in] state                    state_t to use for this operation
 * @param[in] openflags                Mode for open
 * @param[out] hasLock                 Indicates that objectHandle->obj_lock is held read
 * @param[out] closeFileDescriptor     Indicates that file descriptor must be closed
 * @param[out] openForLocks            Indicates file is open for locks
 *
 * @return FSAL status.
 */
static fsal_status_t findFileDescriptor(struct SaunaFSFd *saunafsFd,
                                        struct fsal_obj_handle *objectHandle,
                                        bool bypass, struct state_t *state,
                                        fsal_openflags_t openflags,
                                        bool *hasLock,
                                        bool *closeFileDescriptor,
                                        bool openForLocks) {
	struct SaunaFSHandle *handle = NULL;
	handle = container_of(objectHandle, struct SaunaFSHandle, handle);

	struct SaunaFSFd emptyFd = {0, NULL};
	struct SaunaFSFd *usableFd = &emptyFd;

	fsal_status_t status;

	bool canReuseOpenedFd = false;
	status =
	    fsal_find_fd((struct fsal_fd **)&usableFd, objectHandle,
	                 (struct fsal_fd *)&handle->fd, &handle->share, bypass,
	                 state, openflags, openFunction, closeFunction, hasLock,
	                 closeFileDescriptor, openForLocks, &canReuseOpenedFd);

	*saunafsFd = *usableFd;
	return status;
}

/**
 * @brief Read data from a file.
 *
 * This function reads data from the given file. The FSAL must be able to perform the read
 * whether a state is presented or not.
 *
 * This function also is expected to handle properly bypassing or not share reservations.
 * This is an (optionally) asynchronous call. When the I/O is complete, the done callback
 * is called with the results.
 *
 * @param [in]     objectHandle     File on which to operate
 * @param [in]     bypass           If state doesn't indicate a share reservation, bypass any deny read
 * @param [in,out] doneCb           Callback to call when I/O is done
 * @param [in,out] readArg          Info about read, passed back in callback
 * @param [in,out] callerArg        Opaque arg from the caller for callback
 */
static void read2(struct fsal_obj_handle *objectHandle, bool bypass,
                  fsal_async_cb doneCb, struct fsal_io_arg *readArg,
                  void *callerArg) {
	struct SaunaFSExport *export = NULL;
	struct SaunaFSHandle *handle = NULL;
	struct SaunaFSFd saunafsFd;
	fsal_status_t status = fsalstat(ERR_FSAL_NO_ERROR, 0);

	bool hasLock = false;
	bool closeFd = false;
	ssize_t bytes = 0;
	uint64_t offset = readArg->offset;

	export = container_of(op_ctx->fsal_export, struct SaunaFSExport, export);
	handle = container_of(objectHandle, struct SaunaFSHandle, handle);

	LogFullDebug(COMPONENT_FSAL,
	             "export = %" PRIu16 " inode = %" PRIu32 " offset=%" PRIu64,
	             export->export.export_id, handle->inode, offset);

	if (readArg->info != NULL) {
		// Currently we don't support READ_PLUS
		doneCb(objectHandle, fsalstat(ERR_FSAL_NOTSUPP, 0), readArg, callerArg);
		return;
	}

	status =
	    findFileDescriptor(&saunafsFd, objectHandle, bypass, readArg->state,
	                       FSAL_O_READ, &hasLock, &closeFd, false);

	if (FSAL_IS_ERROR(status)) {
		if (closeFd) { closeFileDescriptor(handle, &saunafsFd); }

		if (hasLock) { PTHREAD_RWLOCK_unlock(&objectHandle->obj_lock); }

		doneCb(objectHandle, status, readArg, callerArg);
		return;
	}

	readArg->io_amount = 0;
	for (int i = 0; i < readArg->iov_count; i++) {
		bytes = saunafs_read(export->fsInstance, &op_ctx->creds, saunafsFd.fd,
		                     offset, readArg->iov[i].iov_len,
		                     readArg->iov[i].iov_base);

		if (bytes == 0) {
			readArg->end_of_file = true;
			break;
		}
		if (bytes < 0) {
			status = fsalLastError();

			if (closeFd) { closeFileDescriptor(handle, &saunafsFd); }

			if (hasLock) { PTHREAD_RWLOCK_unlock(&objectHandle->obj_lock); }

			doneCb(objectHandle, status, readArg, callerArg);
			return;
		}

		readArg->io_amount += bytes;
		offset += bytes;
	}

	if (closeFd) { closeFileDescriptor(handle, &saunafsFd); }

	if (hasLock) { PTHREAD_RWLOCK_unlock(&objectHandle->obj_lock); }

	doneCb(objectHandle, status, readArg, callerArg);
}

/** @brief Create a directory.
 *
 * Creation operations. This function creates a new directory.
 *
 * @param [in]     directoryHandle     Directory in which to create the directory
 * @param [in]     name                Name of directory to create
 * @param [in]     attributesToSet     Attributes to set on newly created object
 * @param [out]    createdObject       Newly created object
 * @param [in,out] attributes          Optional attributes for newly created object
 *
 * \see fsal_api.h for more information
 *
 * @returns: FSAL status
 */
static fsal_status_t mkdir_(struct fsal_obj_handle *directoryHandle,
                            const char *name,
                            struct fsal_attrlist *attributesToSet,
                            struct fsal_obj_handle **createdObject,
                            struct fsal_attrlist *attributes) {
	struct SaunaFSExport *export = NULL;
	struct SaunaFSHandle *directory = NULL;
	struct SaunaFSHandle *handle = NULL;
	struct sau_entry directoryEntry;
	fsal_status_t status;

	export = container_of(op_ctx->fsal_export, struct SaunaFSExport, export);
	directory = container_of(directoryHandle, struct SaunaFSHandle, handle);

	LogFullDebug(COMPONENT_FSAL, "export = %" PRIu16 " parent_inode = %"
	             PRIu32 " mode = %" PRIo32 " name = %s",
	             export->export.export_id, directory->inode,
	             attributesToSet->mode, name);

	mode_t unix_mode = fsal2unix_mode(attributesToSet->mode) &
	        ~op_ctx->fsal_export->exp_ops.fs_umask(op_ctx->fsal_export);

	int retvalue =
	    saunafs_mkdir(export->fsInstance, &op_ctx->creds, directory->inode,
	                  name, unix_mode, &directoryEntry);

	if (retvalue < 0) {
		return fsalLastError();
	}

	handle = allocateHandle(&directoryEntry.attr, export);
	*createdObject = &handle->handle;

	FSAL_UNSET_MASK(attributesToSet->valid_mask, (attrmask_t)ATTR_MODE);

	if (attributesToSet->valid_mask) {
		status = (*createdObject)->obj_ops->setattr2(*createdObject, false,
		                                             NULL, attributesToSet);

		if (FSAL_IS_ERROR(status)) {
			LogFullDebug(COMPONENT_FSAL, "setattr2 status=%s",
			             fsal_err_txt(status));

			// Release the handle we just allocate
			(*createdObject)->obj_ops->release(*createdObject);
			*createdObject = NULL;
		}
		else if (attributes != NULL) {
			// We ignore errors here. The mkdir and setattr succeeded,
			// so we don't want to return error if the getattrs fails.
			// We'll just return no attributes in that case.
			(*createdObject)->obj_ops->getattrs(*createdObject, attributes);
		}
	}
	else if (attributes != NULL) {
		// Since we haven't set any attributes other than what
		// was set on create, just use the stat results we used
		// to create the fsal_obj_handle.
		posix2fsal_attributes_all(&directoryEntry.attr, attributes);
	}

	FSAL_SET_MASK(attributesToSet->valid_mask, (attrmask_t)ATTR_MODE);
	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/**
 * @brief Create a new link.
 *
 * This function creates a new name for an existing object.
 *
 * @param [in] objectHandle             Object to be linked to
 * @param [in] destinationDirHandle     Directory in which to create the link
 * @param [in] name                     Name for link
 *
 * @returns: FSAL status
 */
static fsal_status_t link_(struct fsal_obj_handle *objectHandle,
                           struct fsal_obj_handle *destinationDirHandle,
                           const char *name) {
	struct SaunaFSExport *export = NULL;
	struct SaunaFSHandle *handle = NULL;
	struct SaunaFSHandle *destinationHandle = NULL;

	export = container_of(op_ctx->fsal_export, struct SaunaFSExport, export);
	handle = container_of(objectHandle, struct SaunaFSHandle, handle);

	destinationHandle =
	    container_of(destinationDirHandle, struct SaunaFSHandle, handle);

	LogFullDebug(COMPONENT_FSAL, "export = %"
	             PRIu16 " inode = %" PRIu32 " dest_inode = %" PRIu32
	             " name = %s", export->export.export_id,
	             handle->inode, destinationHandle->inode, name);

	sau_entry_t entry;
	int retvalue =
	    saunafs_link(export->fsInstance, &op_ctx->creds, handle->inode,
	                 destinationHandle->inode, name, &entry);

	if (retvalue < 0) {
		return fsalLastError();
	}

	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/**
 * @brief Rename a file.
 *
 * This function renames a file (technically it changes the name of one link,
 * which may be the only link to the file.)
 *
 * @param [in] oldParentHandle     Source directory
 * @param [in] oldName             Original name
 * @param [in] newParentHandle     Destination directory
 * @param [in] newName             New name
 *
 * @returns: FSAL status
 */
static fsal_status_t rename_(struct fsal_obj_handle *objectHandle,
                             struct fsal_obj_handle *oldParentHandle,
                             const char *oldName,
                             struct fsal_obj_handle *newParentHandle,
                             const char *newName) {
	(void)objectHandle;
	struct SaunaFSExport *export = NULL;
	struct SaunaFSHandle *oldDir = NULL;
	struct SaunaFSHandle *newDir = NULL;

	export = container_of(op_ctx->fsal_export, struct SaunaFSExport, export);
	oldDir = container_of(oldParentHandle, struct SaunaFSHandle, handle);
	newDir = container_of(newParentHandle, struct SaunaFSHandle, handle);

	LogFullDebug(COMPONENT_FSAL, "export=%" PRIu16 " old_inode=%" PRIu32
	             " new_inode=%" PRIu32 " old_name=%s new_name=%s",
	             export->export.export_id, oldDir->inode,
	             newDir->inode, oldName, newName);

	int status = saunafs_rename(export->fsInstance, &op_ctx->creds,
	                            oldDir->inode, oldName, newDir->inode, newName);

	if (status < 0) {
		return fsalLastError();
	}

	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/**
 * @brief Remove a name from a directory.
 *
 * This function removes a name from a directory and possibly deletes the file
 * so named.
 *
 * @param [in] directoryHandle     The directory from which to remove the name
 * @param [in] objectHandle        The object being removed
 * @param [in] name                The name to remove
 *
 * @returns: FSAL status
 */
static fsal_status_t unlink_(struct fsal_obj_handle *directoryHandle,
                             struct fsal_obj_handle *objectHandle,
                             const char *name) {
	struct SaunaFSExport *export = NULL;
	struct SaunaFSHandle *directory = NULL;
	int status = 0;

	export = container_of(op_ctx->fsal_export, struct SaunaFSExport, export);
	directory = container_of(directoryHandle, struct SaunaFSHandle, handle);

	LogFullDebug(COMPONENT_FSAL, "export = %" PRIu16 " parent_inode = %"
	             PRIu32 " name = %s type = %s", export->export.export_id,
	             directory->inode, name,
	             object_file_type_to_str(objectHandle->type));

	if (objectHandle->type != DIRECTORY) {
		status = saunafs_unlink(export->fsInstance, &op_ctx->creds,
		                        directory->inode, name);
	}
	else {
		status = saunafs_rmdir(export->fsInstance, &op_ctx->creds,
		                       directory->inode, name);
	}

	if (status < 0) {
		return fsalLastError();
	}

	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/**
 * @brief Close a file.
 *
 * This function closes a file. This should return ERR_FSAL_NOT_OPENED if the
 * global FD for this obj was not open.
 *
 * @param [in] objectHandle     File to close
 *
 * @returns: FSAL status
 */
static fsal_status_t close_(struct fsal_obj_handle *objectHandle) {
	fsal_status_t status;
	struct SaunaFSHandle *handle = NULL;
	handle = container_of(objectHandle, struct SaunaFSHandle, handle);

	LogFullDebug(COMPONENT_FSAL, "export=%" PRIu16 " inode=%" PRIu32,
	             handle->key.exportId, handle->inode);

	PTHREAD_RWLOCK_wrlock(&objectHandle->obj_lock);

	if (handle->fd.openflags == FSAL_O_CLOSED) {
		status = fsalstat(ERR_FSAL_NOT_OPENED, 0);
	}
	else {
		status = closeFileDescriptor(handle, &handle->fd);
	}

	PTHREAD_RWLOCK_unlock(&objectHandle->obj_lock);

	return status;
}

/**
 * @brief Write data to a file.
 *
 * This function writes data to a file. The FSAL must be able to perform the
 * write whether a state is presented or not. This function also is expected
 * to handle properly bypassing or not share reservations.
 * Even with bypass == true, it will enforce a mandatory (NFSv4) deny_write if
 * an appropriate state is not passed).

 * The FSAL is expected to enforce sync if necessary. This is an (optionally)
 * asynchronous call. When the I/O is complete, the done_cb callback is called.
 *
 * @param [in]     objectHandle     File on which to operate
 * @param [in]     bypass           If state doesn't indicate a share reservation,
 *                                  bypass any non-mandatory deny write
 * @param [in,out] doneCb           Callback to call when I/O is done
 * @param [in,out] writeArg         Info about write, passed back in callback
 * @param [in,out] callerArg        Opaque arg from the caller for callback
 */
static void write2(struct fsal_obj_handle *objectHandle, bool bypass,
                   fsal_async_cb doneCb, struct fsal_io_arg *writeArg,
                   void *callerArg) {
	struct SaunaFSExport *export = NULL;
	struct SaunaFSHandle *handle = NULL;
	struct SaunaFSFd saunafsFd;

	fsal_status_t status;
	bool hasLock = false;
	bool closeFd = false;

	ssize_t bytes = 0;
	uint64_t offset = writeArg->offset;

	export = container_of(op_ctx->fsal_export, struct SaunaFSExport, export);
	handle = container_of(objectHandle, struct SaunaFSHandle, handle);

	LogFullDebug(COMPONENT_FSAL, "export=%" PRIu16 " inode=%" PRIu32
	             " offset=%" PRIu64, export->export.export_id,
	             handle->inode, offset);

	if (writeArg->info) {
		doneCb(objectHandle, fsalstat(ERR_FSAL_NOTSUPP, 0), writeArg,
		       callerArg);
		return;
	}

	status =
	    findFileDescriptor(&saunafsFd, objectHandle, bypass, writeArg->state,
	                       FSAL_O_WRITE, &hasLock, &closeFd, false);

	if (FSAL_IS_ERROR(status)) {
		if (closeFd) { closeFileDescriptor(handle, &saunafsFd); }

		if (hasLock) { PTHREAD_RWLOCK_unlock(&objectHandle->obj_lock); }

		doneCb(objectHandle, status, writeArg, callerArg);
		return;
	}

	for (int i = 0; i < writeArg->iov_count; i++) {
		bytes = saunafs_write(export->fsInstance, &op_ctx->creds, saunafsFd.fd,
		                      offset, writeArg->iov[i].iov_len,
		                      writeArg->iov[i].iov_base);

		if (bytes == 0) {
			break;
		}
		if (bytes < 0) {
			status = fsalLastError();
			if (closeFd) { closeFileDescriptor(handle, &saunafsFd); }

			if (hasLock) { PTHREAD_RWLOCK_unlock(&objectHandle->obj_lock); }

			doneCb(objectHandle, status, writeArg, callerArg);
			return;
		}

		writeArg->io_amount += bytes;
		offset += bytes;
	}

	if (writeArg->fsal_stable) {
		int retvalue =
		    saunafs_fsync(export->fsInstance, &op_ctx->creds, saunafsFd.fd);

		if (retvalue < 0) {
			status = fsalLastError();
			writeArg->fsal_stable = false;
		}
	}

	if (closeFd) { closeFileDescriptor(handle, &saunafsFd); }

	if (hasLock) { PTHREAD_RWLOCK_unlock(&objectHandle->obj_lock); }

	doneCb(objectHandle, status, writeArg, callerArg);
}

/**
 * @brief Commit written data.
 *
 * This function flushes possibly buffered data to a file. This method differs
 * from commit due to the need to interact with share reservations and the fact
 * that the FSAL manages the state of "file descriptors". The FSAL must be able
 * to perform this operation without being passed a specific state.
 *
 * @param [in] objectHandle     File on which to operate
 * @param [in] offset           Start of range to commit
 * @param [in] length           Length of range to commit
 *
 * @returns: FSAL status
 */
static fsal_status_t commit2(struct fsal_obj_handle *objectHandle, off_t offset,
                             size_t length) {
	struct SaunaFSExport *export = NULL;
	struct SaunaFSHandle *handle = NULL;

	struct SaunaFSFd emptyFd = {0, NULL};
	struct SaunaFSFd *saunafsFd = &emptyFd;

	fsal_status_t status;
	bool hasLock = false;
	bool closeFd = false;

	export = container_of(op_ctx->fsal_export, struct SaunaFSExport, export);
	handle = container_of(objectHandle, struct SaunaFSHandle, handle);

	LogFullDebug(COMPONENT_FSAL, "export = %" PRIu16 " inode = %" PRIu32
	             " offset = %lli len = %zu", export->export.export_id,
	             handle->inode, (long long)offset, length);

	status = fsal_reopen_obj(objectHandle, false, false, FSAL_O_WRITE,
	                         (struct fsal_fd *)&handle->fd, &handle->share,
	                         openFunction, closeFunction,
	                         (struct fsal_fd **)&saunafsFd, &hasLock, &closeFd);

	if (!FSAL_IS_ERROR(status)) {
		int retvalue =
		    saunafs_fsync(export->fsInstance, &op_ctx->creds, saunafsFd->fd);

		if (retvalue < 0) {
			status = fsalLastError();
		}
	}

	if (closeFd) {
		int retvalue = sau_release(export->fsInstance, saunafsFd->fd);

		if (retvalue < 0) {
			status = fsalLastError();
		}
	}

	if (hasLock) {
		PTHREAD_RWLOCK_unlock(&objectHandle->obj_lock);
	}

	return status;
}

/**
 * @brief Set attributes on an object.
 *
 * This function sets attributes on an object. Which attributes are set is determined
 * by attrib_set->mask. The FSAL must manage bypass or not of share reservations, and
 * a state may be passed.

 * The caller is expected to invoke fsal_release_attrs to release any resources held
 * by the set attributes. The FSAL layer MAY have added an inherited ACL.
 *
 * @param [in] objectHandle     File on which to operate
 * @param [in] bypass           If state doesn't indicate a share reservation,
 *                              bypass any non-mandatory deny write
 * @param [in] state            state_t to use for this operation
 * @param [in] attributes       Attributes to set
 *
 * @returns: FSAL status
 */
static fsal_status_t setattr2(struct fsal_obj_handle *objectHandle, bool bypass,
                              struct state_t *state,
                              struct fsal_attrlist *attributes) {
	struct SaunaFSExport *export = NULL;
	struct SaunaFSHandle *handle = NULL;

	bool hasLock = false;
	bool closeFd = false;
	fsal_status_t status = fsalstat(ERR_FSAL_NO_ERROR, 0);

	export = container_of(op_ctx->fsal_export, struct SaunaFSExport, export);
	handle = container_of(objectHandle, struct SaunaFSHandle, handle);

	LogAttrlist(COMPONENT_FSAL, NIV_FULL_DEBUG, "attrs ", attributes, false);

	if (FSAL_TEST_MASK(attributes->valid_mask, (attrmask_t)ATTR_MODE)) {
		attributes->mode &=
		        ~op_ctx->fsal_export->exp_ops.fs_umask(op_ctx->fsal_export);
	}

	if (FSAL_TEST_MASK(attributes->valid_mask, (attrmask_t)ATTR_SIZE)) {
		if (objectHandle->type != REGULAR_FILE) {
			LogFullDebug(COMPONENT_FSAL,
			             "Setting size on non-regular file");
			return fsalstat(ERR_FSAL_INVAL, EINVAL);
		}

		bool canReuseOpenedFd = false;
		status = fsal_find_fd(NULL, objectHandle, NULL, &handle->share,
		                      bypass, state, FSAL_O_RDWR, NULL, NULL,
		                      &hasLock, &closeFd, false,
		                      &canReuseOpenedFd);

		if (FSAL_IS_ERROR(status)) {
			LogFullDebug(COMPONENT_FSAL, "fsal_find_fd status = %s",
			             fsal_err_txt(status));

			if (hasLock) { PTHREAD_RWLOCK_unlock(&objectHandle->obj_lock); }

			return status;
		}
	}

	struct stat posixAttributes;
	uint mask = 0;
	memset(&posixAttributes, 0, sizeof(posixAttributes));

	if (FSAL_TEST_MASK(attributes->valid_mask, (attrmask_t)ATTR_SIZE)) {
		mask |= SAU_SET_ATTR_SIZE;
		posixAttributes.st_size = (__off_t)(attributes->filesize);
		LogFullDebug(COMPONENT_FSAL, "setting size to %lld",
		             (long long)posixAttributes.st_size);
	}

	if (FSAL_TEST_MASK(attributes->valid_mask, (attrmask_t)ATTR_MODE)) {
		mask |= SAU_SET_ATTR_MODE;
		posixAttributes.st_mode = fsal2unix_mode(attributes->mode);
	}

	if (FSAL_TEST_MASK(attributes->valid_mask, (attrmask_t)ATTR_OWNER)) {
		mask |= SAU_SET_ATTR_UID;
		posixAttributes.st_uid = attributes->owner;
	}

	if (FSAL_TEST_MASK(attributes->valid_mask, (attrmask_t)ATTR_GROUP)) {
		mask |= SAU_SET_ATTR_GID;
		posixAttributes.st_gid = attributes->group;
	}

	if (FSAL_TEST_MASK(attributes->valid_mask, (attrmask_t)ATTR_ATIME)) {
		mask |= SAU_SET_ATTR_ATIME;
		posixAttributes.st_atim = attributes->atime;
	}

	if (FSAL_TEST_MASK(attributes->valid_mask, (attrmask_t)ATTR_ATIME_SERVER)) {
		mask |= SAU_SET_ATTR_ATIME_NOW;
	}

	if (FSAL_TEST_MASK(attributes->valid_mask, (attrmask_t)ATTR_MTIME)) {
		mask |= SAU_SET_ATTR_MTIME;
		posixAttributes.st_mtim = attributes->mtime;
	}

	if (FSAL_TEST_MASK(attributes->valid_mask, (attrmask_t)ATTR_MTIME_SERVER)) {
		mask |= SAU_SET_ATTR_MTIME_NOW;
	}

	sau_attr_reply_t reply;
	int retvalue =
	    saunafs_setattr(export->fsInstance, &op_ctx->creds, handle->inode,
	                    &posixAttributes, mask, &reply);

	if (retvalue < 0) {
		status = fsalLastError();

		if (hasLock) { PTHREAD_RWLOCK_unlock(&objectHandle->obj_lock); }

		return status;
	}

#ifdef ENABLE_NFS_ACL_SUPPORT
	if (FSAL_TEST_MASK(attributes->valid_mask, (attrmask_t)ATTR_ACL)) {
		status =
		    setACL(export, handle->inode, attributes->acl, reply.attr.st_mode);
	}
#endif

	if (hasLock) {
		PTHREAD_RWLOCK_unlock(&objectHandle->obj_lock);
	}

	return status;
}

/**
 * @brief Manage closing a file when a state is no longer needed.
 *
 * When the upper layers are ready to dispense with a state, this method is called
 * to allow the FSAL to close any file descriptors or release any other resources
 * associated with the state. A call to free_state should be assumed to follow soon.
 *
 * @param [in] objectHandle     File on which to operate
 * @param [in] state            state_t to use for this operation
 *
 * @returns: FSAL status
 */
static fsal_status_t close2(struct fsal_obj_handle *objectHandle,
                            struct state_t *state) {
	struct SaunaFSHandle *handle = NULL;
	handle = container_of(objectHandle, struct SaunaFSHandle, handle);

	LogFullDebug(COMPONENT_FSAL, "export = %" PRIu16 " inode = %" PRIu32,
	             handle->key.exportId, handle->inode);

	if (state->state_type == STATE_TYPE_SHARE ||
	    state->state_type == STATE_TYPE_NLM_SHARE ||
	    state->state_type == STATE_TYPE_9P_FID) {
		// This is a share state, we must update the share counters
		// This can block over an I/O operation
		PTHREAD_RWLOCK_wrlock(&objectHandle->obj_lock);

		update_share_counters(&handle->share, handle->fd.openflags,
		                      FSAL_O_CLOSED);

		PTHREAD_RWLOCK_unlock(&objectHandle->obj_lock);
	}

	return closeFileDescriptor(handle, &handle->fd);
}

/**
 * @brief Create a symbolic link.
 *
 * This function creates a new symbolic link.
 *
 * @param [in] directoryHandle      Directory in which to create the object
 * @param [in] name                 Name of object to create
 * @param [in] symbolicLinkPath     Content of symbolic link
 * @param [in] attributesToSet      Attributes to set on newly created object
 * @param [out] createdObject       Newly created object
 * @param [in,out] attributes       Optional attributes for newly created object
 *
 * \see fsal_api.h for more information
 *
 * @returns: FSAL status
 */
static fsal_status_t symlink_(struct fsal_obj_handle *directoryHandle,
                              const char *name, const char *symbolicLinkPath,
                              struct fsal_attrlist *attributesToSet,
                              struct fsal_obj_handle **createdObject,
                              struct fsal_attrlist *attributes) {
	struct SaunaFSExport *export = NULL;
	struct SaunaFSHandle *directory = NULL;
	struct sau_entry entry;

	export = container_of(op_ctx->fsal_export, struct SaunaFSExport, export);
	directory = container_of(directoryHandle, struct SaunaFSHandle, handle);

	LogFullDebug(COMPONENT_FSAL, "export = %" PRIu16 " parent_inode = %"
	             PRIu32 " name = %s", export->export.export_id,
	             directory->inode, name);

	int retvalue =
	    saunafs_symlink(export->fsInstance, &op_ctx->creds, symbolicLinkPath,
	                    directory->inode, name, &entry);

	if (retvalue < 0) {
		return fsalLastError();
	}

	struct SaunaFSHandle *handle = allocateHandle(&entry.attr, export);
	*createdObject = &handle->handle;

	// We handled the mode above
	FSAL_UNSET_MASK(attributesToSet->valid_mask, (attrmask_t)ATTR_MODE);

	if (attributesToSet->valid_mask) {
		fsal_status_t status;

		// Now per support_ex API, if there are any other attributes set,
		// go ahead and get them set now
		status = (*createdObject)->obj_ops->setattr2(*createdObject,
		                                             false, NULL,
		                                             attributesToSet);

		if (FSAL_IS_ERROR(status)) {
			// Release the handle we just allocated
			LogFullDebug(COMPONENT_FSAL, "setattr2 status = %s",
			             fsal_err_txt(status));

			(*createdObject)->obj_ops->release(*createdObject);
			*createdObject = NULL;
		}
	}
	else if (attributes != NULL) {
		posix2fsal_attributes_all(&entry.attr, attributes);
	}

	FSAL_SET_MASK(attributesToSet->valid_mask, (attrmask_t)ATTR_MODE);
	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/**
 * @brief Perform a lock operation.
 *
 * This function performs a lock operation (lock, unlock, test) on a file.
 * This method assumes the FSAL is able to support lock owners, though it
 * need not support asynchronous blocking locks. Passing the lock state
 * allows the FSAL to associate information with a specific lock owner
 * for each file (which may include use of a "file descriptor".
 *
 * @param [in]  objectHandle        File on which to operate
 * @param [in]  state               state_t to use for this operation
 * @param [in]  owner               Lock owner
 * @param [in]  lockOperation       Operation to perform
 * @param [in]  requestedLock       Lock to take/release/test
 * @param [out] conflictingLock     Conflicting lock
 *
 * @returns: FSAL status
 */
fsal_status_t lock_op2(struct fsal_obj_handle *objectHandle,
                       struct state_t *state, void *owner,
                       fsal_lock_op_t lockOperation,
                       fsal_lock_param_t *requestedLock,
                       fsal_lock_param_t *conflictingLock) {
	struct SaunaFSExport *export = NULL;

	sau_err_t lastError = 0;
	fileinfo_t *fileinfo = NULL;
	sau_lock_info_t lockInfo;

	fsal_status_t status = {0, 0};
	int retval = 0;

	struct SaunaFSFd saunafsFd;
	fsal_openflags_t openflags = FSAL_O_RDWR;

	bool hasLock = false;
	bool closeFd = false;
	bool bypass = false;

	export = container_of(op_ctx->fsal_export, struct SaunaFSExport, export);

	LogFullDebug(COMPONENT_FSAL, "op:%d type:%d start:%" PRIu64 " length:%"
	             PRIu64 " ", lockOperation, requestedLock->lock_type,
	             requestedLock->lock_start, requestedLock->lock_length);

	if (objectHandle == NULL) {
		LogCrit(COMPONENT_FSAL, "objectHandle arg is NULL.");
		return fsalstat(ERR_FSAL_FAULT, 0);
	}

	if (owner == NULL) {
		LogCrit(COMPONENT_FSAL, "owner arg is NULL.");
		return fsalstat(ERR_FSAL_FAULT, 0);
	}

	if (lockOperation == FSAL_OP_LOCKT) {
		// We may end up using global fd, don't fail on a deny mode
		bypass = true;
		openflags = FSAL_O_ANY;
	}
	else if (lockOperation == FSAL_OP_LOCK) {
		if (requestedLock->lock_type == FSAL_LOCK_R) {
			openflags = FSAL_O_READ;
		}
		else if (requestedLock->lock_type == FSAL_LOCK_W) {
			openflags = FSAL_O_WRITE;
		}
	}
	else if (lockOperation == FSAL_OP_UNLOCK) {
		openflags = FSAL_O_ANY;
	}
	else {
		LogFullDebug(COMPONENT_FSAL, "ERROR: Lock operation requested "
		                             "was not TEST, READ, or WRITE.");

		return fsalstat(ERR_FSAL_NOTSUPP, 0);
	}

	if (lockOperation != FSAL_OP_LOCKT && state == NULL) {
		LogCrit(COMPONENT_FSAL, "Non TEST operation with NULL state");
		return posix2fsal_status(EINVAL);
	}

	if (requestedLock->lock_type == FSAL_LOCK_R) {
		lockInfo.l_type = F_RDLCK;
	}
	else if (requestedLock->lock_type == FSAL_LOCK_W) {
		lockInfo.l_type = F_WRLCK;
	}
	else {
		LogFullDebug(COMPONENT_FSAL,
		             "ERROR: The requested lock type was not "
		             "read or write.");

		return fsalstat(ERR_FSAL_NOTSUPP, 0);
	}

	if (lockOperation == FSAL_OP_UNLOCK) {
		lockInfo.l_type = F_UNLCK;
	}

	lockInfo.l_pid = 0;
	lockInfo.l_len = requestedLock->lock_length;
	lockInfo.l_start = requestedLock->lock_start;

	// Get a usable file descriptor
	status = findFileDescriptor(&saunafsFd, objectHandle, bypass, state,
	                            openflags, &hasLock, &closeFd, true);

	// IF find_fd returned DELAY, then fd caching in mdcache is
	// turned off, which means that the consecutive attempt is very likely
	// to succeed immediately.
	if (status.major == ERR_FSAL_DELAY) {
		status = findFileDescriptor(&saunafsFd, objectHandle, bypass, state,
		                            openflags, &hasLock, &closeFd, true);
	}

	if (FSAL_IS_ERROR(status)) {
		LogCrit(COMPONENT_FSAL, "Unable to find fd for lock operation");

		return status;
	}

	fileinfo = saunafsFd.fd;
	sau_set_lock_owner(fileinfo, (uint64_t)owner);

	if (lockOperation == FSAL_OP_LOCKT) {
		retval = saunafs_getlock(export->fsInstance, &op_ctx->creds, fileinfo,
		                         &lockInfo);
	}
	else {
		retval = saunafs_setlock(export->fsInstance, &op_ctx->creds, fileinfo,
		                         &lockInfo);
	}

	if (retval < 0) {
		lastError = sau_last_err();

		if (closeFd) { sau_release(export->fsInstance, fileinfo); }

		if (hasLock) { PTHREAD_RWLOCK_unlock(&objectHandle->obj_lock); }

		LogFullDebug(COMPONENT_FSAL, "Returning error %d", lastError);
		return saunafsToFsalError(lastError);
	}

	// F_UNLCK is returned then the tested operation would be possible
	if (conflictingLock != NULL) {
		if (lockOperation == FSAL_OP_LOCKT && lockInfo.l_type != F_UNLCK) {
			conflictingLock->lock_length = lockInfo.l_len;
			conflictingLock->lock_start = lockInfo.l_start;
			conflictingLock->lock_type = lockInfo.l_type;
		}
		else {
			conflictingLock->lock_length = 0;
			conflictingLock->lock_start = 0;
			conflictingLock->lock_type = FSAL_NO_LOCK;
		}
	}

	if (closeFd) {
		sau_release(export->fsInstance, fileinfo);
	}

	if (hasLock) {
		PTHREAD_RWLOCK_unlock(&objectHandle->obj_lock);
	}

	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/**
 * @brief Re-open a file that may be already opened.
 *
 * This function supports changing the access mode of a share reservation and
 * thus should only be called with a share state. The st_lock must be held.
 *
 * This MAY be used to open a file the first time if there is no need for open
 * by name or create semantics. One example would be 9P lopen.
 *
 * @param [in] objectHandle     File on which to operate
 * @param [in] state            state_t to use for this operation
 * @param [in] openflags        Mode for re-open
 *
 * @returns: FSAL status
 */
static fsal_status_t reopen2(struct fsal_obj_handle *objectHandle,
                             struct state_t *state,
                             fsal_openflags_t openflags) {
	struct SaunaFSHandle *handle = NULL;
	struct SaunaFSFd *sharedFd = NULL;
	fsal_status_t status;

	handle = container_of(objectHandle, struct SaunaFSHandle, handle);
	sharedFd = &container_of(state, struct SaunaFSStateFd, state)->saunafsFd;

	LogFullDebug(COMPONENT_FSAL, "export = %" PRIu16 " inode = %" PRIu32,
	             handle->key.exportId, handle->inode);

	struct SaunaFSFd saunafsFd = { FSAL_O_CLOSED, NULL };

	// This can block over an I/O operation
	PTHREAD_RWLOCK_wrlock(&objectHandle->obj_lock);

	fsal_openflags_t oldOpenflags = sharedFd->openflags;

	// We can conflict with old share, so go ahead and check now
	status = check_share_conflict(&handle->share, openflags, false);

	if (FSAL_IS_ERROR(status)) {
		PTHREAD_RWLOCK_unlock(&objectHandle->obj_lock);
		return status;
	}

	// Set up the new share so we can drop the lock and not have a
	// conflicting share be asserted, updating the share counters
	update_share_counters(&handle->share, oldOpenflags, openflags);

	PTHREAD_RWLOCK_unlock(&objectHandle->obj_lock);

	status = openFileDescriptor(handle, openflags, &saunafsFd, true);

	if (!FSAL_IS_ERROR(status)) {
		closeFileDescriptor(handle, sharedFd);
		*sharedFd = saunafsFd;
	}
	else {
		// We had a failure on open - we need to revert the share.
		// This can block over an I/O operation
		PTHREAD_RWLOCK_wrlock(&objectHandle->obj_lock);

		update_share_counters(&handle->share, openflags, oldOpenflags);

		PTHREAD_RWLOCK_unlock(&objectHandle->obj_lock);
	}

	return status;
}

/**
 * @brief Create a special file.
 *
 * Create a special file. This function creates a new special file.
 *
 * @param [in] directoryHandle     Directory in which to create the object
 * @param [in] name                Name of object to create
 * @param [in] nodeType            Type of special file to create
 * @param [in] attributesToSet     Attributes to set on newly created object
 * @param [in] createdObject       Newly created object
 * @param [in] attributes          Optional attributes for newly created object
 *
 * \see fsal_api.h for more information
 *
 * @returns: FSAL status
 */
static fsal_status_t mknode(struct fsal_obj_handle *directoryHandle,
                            const char *name, object_file_type_t nodeType,
                            struct fsal_attrlist *attributesToSet,
                            struct fsal_obj_handle **createdObject,
                            struct fsal_attrlist *attributes) {
	struct SaunaFSExport *export = NULL;
	struct SaunaFSHandle *directory = NULL;
	struct SaunaFSHandle *handle = NULL;
	struct sau_entry entry;
	mode_t unixMode = 0;
	dev_t unixDev = 0;

	export = container_of(op_ctx->fsal_export, struct SaunaFSExport, export);
	directory = container_of(directoryHandle, struct SaunaFSHandle, handle);

	LogFullDebug(COMPONENT_FSAL,
	             "export = %" PRIu16 " parent_inode = %" PRIu32 " mode = %"
	             PRIo32 " name = %s", export->export.export_id,
	             directory->inode, attributesToSet->mode, name);

	unixMode = fsal2unix_mode(attributesToSet->mode) &
	        ~op_ctx->fsal_export->exp_ops.fs_umask(op_ctx->fsal_export);

	switch (nodeType) {
	case BLOCK_FILE:
		unixMode |= S_IFBLK;
		unixDev = makedev(attributesToSet->rawdev.major,
		                  attributesToSet->rawdev.minor);
		break;
	case CHARACTER_FILE:
		unixMode |= S_IFCHR;
		unixDev = makedev(attributesToSet->rawdev.major,
		                  attributesToSet->rawdev.minor);
		break;
	case FIFO_FILE:
		unixMode |= S_IFIFO;
		break;
	case SOCKET_FILE:
		unixMode |= S_IFSOCK;
		break;
	default:
		LogMajor(COMPONENT_FSAL, "Invalid node type in FSAL_mknode: %d",
		         nodeType);

		return fsalstat(ERR_FSAL_INVAL, EINVAL);
	}

	int retvalue =
	    saunafs_mknode(export->fsInstance, &op_ctx->creds, directory->inode,
	                   name, unixMode, unixDev, &entry);

	if (retvalue < 0) {
		return fsalLastError();
	}

	handle = allocateHandle(&entry.attr, export);
	*createdObject = &handle->handle;

	// We handled the mode above
	FSAL_UNSET_MASK(attributesToSet->valid_mask, (attrmask_t)ATTR_MODE);

	if (attributesToSet->valid_mask) {
		fsal_status_t status;
		// Setting attributes for the created object
		status = (*createdObject)->obj_ops->setattr2(*createdObject,
		                                             false, NULL,
		                                             attributesToSet);

		if (FSAL_IS_ERROR(status)) {
			LogFullDebug(COMPONENT_FSAL, "setattr2 status = %s",
			             fsal_err_txt(status));

			// Release the handle we just allocated
			(*createdObject)->obj_ops->release(*createdObject);
			*createdObject = NULL;
		}
	}
	else if (attributes != NULL) {
		// Since we haven't set any attributes other than what was set on create,
		// just use the stat results we used to create the fsal_obj_handle
		posix2fsal_attributes_all(&entry.attr, attributes);
	}

	FSAL_SET_MASK(attributesToSet->valid_mask, (attrmask_t)ATTR_MODE);
	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/**
 * @brief Read the content of a link.
 *
 * File object operations.
 * This function reads the content of a symbolic link. The FSAL will allocate a
 * buffer and store its address and the link length in the link_content
 * gsh_buffdesc. The caller must free this buffer with gsh_free.
 *
 * The symlink content passed back must be null terminated and the length
 * indicated in the buffer description must include the terminator.
 *
 * @param [in]  objectHandle  Link to read
 * @param [out] buffer        Buffer descriptor to which the FSAL will store the
 *                            address of the buffer holding the link and the
 *                            link length
 * @param [out] refresh       true if the content are to be retrieved from the
 *                            underlying filesystem rather than cache
 *
 * @returns: FSAL status
 */
static fsal_status_t readlink_(struct fsal_obj_handle *objectHandle,
                               struct gsh_buffdesc *buffer, bool refresh) {
	(void)refresh;
	struct SaunaFSExport *export = NULL;
	struct SaunaFSHandle *handle = NULL;
	char result[SAUNAFS_MAX_READLINK_LENGTH];

	if (objectHandle->type != SYMBOLIC_LINK) {
		return fsalstat(ERR_FSAL_FAULT, 0);
	}

	export = container_of(op_ctx->fsal_export, struct SaunaFSExport, export);
	handle = container_of(objectHandle, struct SaunaFSHandle, handle);

	LogFullDebug(COMPONENT_FSAL, "export = %" PRIu16 " inode = %" PRIu32,
	             export->export.export_id, handle->inode);

	int size =
	    saunafs_readlink(export->fsInstance, &op_ctx->creds, handle->inode,
	                     result, SAUNAFS_MAX_READLINK_LENGTH);

	// saunafs_readlink() returns the size of the read link if succeed.
	// Otherwise returns -1 to indicate an error occurred
	if (size < 0) {
		return fsalLastError();
	}

	size = MIN(size, SAUNAFS_MAX_READLINK_LENGTH);
	buffer->addr = gsh_strldup(result, size, &buffer->len);

	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/**
 * @brief Return open status of a state.
 *
 * This function returns open flags representing the current open status
 * for a state. The st_lock must be held.
 *
 * @param [in] objectHandle     File owning state
 * @param [in] state            File state to interrogate
 *
 * @returns: Flags representing current open status
 */
static fsal_openflags_t status2(struct fsal_obj_handle *objectHandle,
                                struct state_t *state) {
	(void)objectHandle;
	struct SaunaFSFd *saunafsFd = NULL;
	saunafsFd = &container_of(state, struct SaunaFSStateFd, state)->saunafsFd;
	return saunafsFd->openflags;
}

/**
 * @brief Merge a duplicate handle with an original handle.
 *
 * This function is used if an upper layer detects that a duplicate object
 * handle has been created. It allows the FSAL to merge anything from the
 * duplicate back into the original.
 *
 * The caller must release the object (the caller may have to close files
 * if the merge is unsuccessful).
 *
 * @param [in] originalHandle     Original handle
 * @param [in] toMergeHandle      Handle to merge into original
 *
 * @returns: FSAL status
 */
static fsal_status_t merge(struct fsal_obj_handle *originalHandle,
                           struct fsal_obj_handle *toMergeHandle) {
	fsal_status_t status = fsalstat(ERR_FSAL_NO_ERROR, 0);

	if (originalHandle->type == REGULAR_FILE &&
	    toMergeHandle->type == REGULAR_FILE) {
		// We need to merge the share reservations on this file.
		// This could result in ERR_FSAL_SHARE_DENIED.
		struct SaunaFSHandle *original = NULL;
		struct SaunaFSHandle *toMerge = NULL;

		original = container_of(originalHandle, struct SaunaFSHandle, handle);
		toMerge = container_of(toMergeHandle, struct SaunaFSHandle, handle);

		// This can block over an I/O operation
		status = merge_share(originalHandle, &original->share, &toMerge->share);
	}

	return status;
}

/**
 * @brief Reserve/Deallocate space in a region of a file.
 *
 * @param [in] objectHandle     File to which bytes should be allocated
 * @param [in] state            Open stateid under which to do the allocation
 * @param [in] offset           Offset at which to begin the allocation
 * @param [in] length           Length of the data to be allocated
 * @param [in] allocate         Should space be allocated or deallocated?
 *
 * @returns: FSAL status
 */
static fsal_status_t fallocate(struct fsal_obj_handle *objectHandle,
                               struct state_t *state, uint64_t offset,
                               uint64_t length, bool allocate) {
	struct SaunaFSExport *export = NULL;
	struct SaunaFSHandle *handle = NULL;
	struct SaunaFSFd saunafsFd;

	fsal_status_t status;
	bool hasLock = false;
	bool closeFd = false;

	export = container_of(op_ctx->fsal_export, struct SaunaFSExport, export);
	handle = container_of(objectHandle, struct SaunaFSHandle, handle);

	status = findFileDescriptor(&saunafsFd, objectHandle, false, state,
	                            FSAL_O_WRITE, &hasLock, &closeFd, false);

	if (FSAL_IS_ERROR(status)) {
		if (closeFd) { closeFileDescriptor(handle, &saunafsFd); }

		if (hasLock) { PTHREAD_RWLOCK_unlock(&objectHandle->obj_lock); }

		return status;
	}

	struct stat posixAttributes;
	memset(&posixAttributes, 0, sizeof(posixAttributes));

	posixAttributes.st_mode =
	    allocate ? 0 : FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE;

	// Get stat to obtain the current size
	sau_attr_reply_t currentStats;
	sau_attr_reply_t reply;

	int retvalue = saunafs_getattr(export->fsInstance, &op_ctx->creds,
	                               handle->inode, &currentStats);

	if (retvalue < 0) {
		if (closeFd) { closeFileDescriptor(handle, &saunafsFd); }

		if (hasLock) { PTHREAD_RWLOCK_unlock(&objectHandle->obj_lock); }

		return fsalLastError();
	}

	if (allocate) {
		if (offset + length > currentStats.attr.st_size) {
			posixAttributes.st_size = offset + length;

			retvalue = saunafs_setattr(export->fsInstance, &op_ctx->creds,
			                           handle->inode, &posixAttributes,
			                           SAU_SET_ATTR_SIZE, &reply);

			if (retvalue < 0) {
				if (closeFd) { closeFileDescriptor(handle, &saunafsFd); }

				if (hasLock) { PTHREAD_RWLOCK_unlock(&objectHandle->obj_lock); }

				return fsalLastError();
			}

			retvalue =
			    saunafs_fsync(export->fsInstance, &op_ctx->creds, saunafsFd.fd);

			if (retvalue < 0) { status = fsalLastError(); }
		}
	}
	else if (allocate == false) { // Deallocate
		// Initialize the zero-buffer
		void *buffer = malloc(length);

		memset(buffer, 0, length);

		// Write the interval [offset..offset + length] with zeros
		ssize_t bytes = saunafs_write(export->fsInstance, &op_ctx->creds,
		                              saunafsFd.fd, offset, length, buffer);

		free(buffer);

		if (bytes < 0) {
			if (closeFd) { closeFileDescriptor(handle, &saunafsFd); }

			if (hasLock) { PTHREAD_RWLOCK_unlock(&objectHandle->obj_lock); }

			return fsalLastError();
		}

		// Set the original size because deallocation must not change file size
		posixAttributes.st_size = currentStats.attr.st_size;

		retvalue =
		    saunafs_setattr(export->fsInstance, &op_ctx->creds, handle->inode,
		                    &posixAttributes, SAU_SET_ATTR_SIZE, &reply);

		if (retvalue < 0) {
			if (closeFd) { closeFileDescriptor(handle, &saunafsFd); }

			if (hasLock) { PTHREAD_RWLOCK_unlock(&objectHandle->obj_lock); }

			return fsalLastError();
		}

		retvalue =
		    saunafs_fsync(export->fsInstance, &op_ctx->creds, saunafsFd.fd);

		if (retvalue < 0) { status = fsalLastError(); }
	}

	if (closeFd) { closeFileDescriptor(handle, &saunafsFd); }

	if (hasLock) { PTHREAD_RWLOCK_unlock(&objectHandle->obj_lock); }

	return status;
}

/**
 * @brief Get extended attribute.
 *
 * This function gets an extended attribute of an object.
 *
 * @param [in]  objectHandle        Input object to query
 * @param [in]  xattributeName      Input extended attribute name
 * @param [out] xattributeValue     Output extended attribute value
 *
 * @returns: FSAL status
 */
static fsal_status_t getxattrs(struct fsal_obj_handle *objectHandle,
                               xattrkey4 *xattributeName,
                               xattrvalue4 *xattributeValue) {
	struct SaunaFSExport *export = NULL;
	export = container_of(op_ctx->fsal_export, struct SaunaFSExport, export);

	struct SaunaFSHandle *handle = NULL;
	handle = container_of(objectHandle, struct SaunaFSHandle, handle);

	size_t curr_size = 0;
	int retvalue = saunafs_getxattr(
	    export->fsInstance, &op_ctx->creds, handle->inode,
	    xattributeName->utf8string_val, xattributeValue->utf8string_len,
	    &curr_size, (uint8_t *)xattributeValue->utf8string_val);

	if (retvalue < 0) {
		LogFullDebug(COMPONENT_FSAL, "GETXATTRS failed returned rc = %d ",
		             retvalue);
		return saunafsToFsalError(retvalue);
	}

	if (curr_size && curr_size <= xattributeValue->utf8string_len) {
		// Updating the real size
		xattributeValue->utf8string_len = curr_size;
		// Make sure utf8string is NUL terminated
		xattributeValue->utf8string_val[curr_size] = '\0';
	}

	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/**
 * @brief Set extended attribute.
 *
 * This function sets an extended attribute of an object.
 *
 * @param [in] objectHandle        Input object to set
 * @param [in] option              Input extended attribute type
 * @param [in] xattributeName      Input extended attribute name to set
 * @param [in] xattributeValue     Input extended attribute value to set
 *
 * @returns: FSAL status
 */
static fsal_status_t setxattrs(struct fsal_obj_handle *objectHandle,
                               setxattr_option4 option,
                               xattrkey4 *xattributeName,
                               xattrvalue4 *xattributeValue) {
	struct SaunaFSExport *export = NULL;
	export = container_of(op_ctx->fsal_export, struct SaunaFSExport, export);

	struct SaunaFSHandle *handle = NULL;
	handle = container_of(objectHandle, struct SaunaFSHandle, handle);

	int retvalue =
	    saunafs_setxattr(export->fsInstance, &op_ctx->creds, handle->inode,
	                     xattributeName->utf8string_val,
	                     (const uint8_t *)xattributeValue->utf8string_val,
	                     xattributeValue->utf8string_len, option);

	if (retvalue < 0) {
		LogDebug(COMPONENT_FSAL, "SETXATTRS returned rc %d", retvalue);
		return saunafsToFsalError(retvalue);
	}

	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/**
 * @brief List extended attributes.
 *
 * This function lists the extended attributes of an object.
 *
 * @param [in]     objectHandle         Input object to list
 * @param [in]     maximumNameSize      Input maximum number of bytes for names
 * @param [in,out] cookie               In/out cookie
 * @param [out]    eof                  Output eof set if no more extended attributes
 * @param [out]    xattributesNames     Output list of extended attribute names this
 *                                      buffer size is double the size of maximumNameSize
 *                                      to allow for component4 overhead
 *
 * @returns: FSAL status
 */
static fsal_status_t listxattrs(struct fsal_obj_handle *objectHandle,
                                count4 maximumNameSize, nfs_cookie4 *cookie,
                                bool_t *eof, xattrlist4 *xattributesNames) {
	char *buffer = NULL;
	fsal_status_t status = fsalstat(ERR_FSAL_NO_ERROR, 0);

	struct SaunaFSExport *export = NULL;
	export = container_of(op_ctx->fsal_export, struct SaunaFSExport, export);

	struct SaunaFSHandle *handle = NULL;
	handle = container_of(objectHandle, struct SaunaFSHandle, handle);

	LogFullDebug(COMPONENT_FSAL, "in cookie %llu length %d",
	             (unsigned long long)*cookie, maximumNameSize);

	// Size of list of extended attributes
	size_t curr_size = 0;

	// First time, the function is called to get the size of xattr list
	int retvalue = saunafs_listxattr(export->fsInstance, &op_ctx->creds,
	                                 handle->inode, 0, &curr_size, NULL);

	if (retvalue < 0) {
		LogDebug(COMPONENT_FSAL, "LISTXATTRS returned rc %d", retvalue);
		return saunafsToFsalError(retvalue);
	}

	// If xattr were retrieved and they can be allocated
	if (curr_size && curr_size < maximumNameSize) {
		buffer = gsh_malloc(curr_size);

		// Second time, the function is called to retrieve the xattr list
		retvalue =
		    saunafs_listxattr(export->fsInstance, &op_ctx->creds, handle->inode,
		                      curr_size, &curr_size, buffer);

		if (retvalue < 0) {
			LogDebug(COMPONENT_FSAL, "LISTXATTRS returned rc %d", retvalue);
			gsh_free(buffer);
			return saunafsToFsalError(retvalue);
		}

		// Setting retrieved extended attributes to Ganesha
		status = fsal_listxattr_helper(buffer, curr_size, maximumNameSize,
		                               cookie, eof, xattributesNames);

		// Releasing allocated buffer
		gsh_free(buffer);
	}

	return status;
}

/**
 * @brief Remove extended attribute.
 *
 * This function removes an extended attribute of an object.
 *
 * @param [in] objectHandle       Input object to set
 * @param [in] xattributeName     Input xattr name to remove
 *
 * @returns: FSAL status
 */
static fsal_status_t removexattrs(struct fsal_obj_handle *objectHandle,
                                  xattrkey4 *xattributeName) {
	struct SaunaFSExport *export = NULL;
	export = container_of(op_ctx->fsal_export, struct SaunaFSExport, export);

	struct SaunaFSHandle *handle = NULL;
	handle = container_of(objectHandle, struct SaunaFSHandle, handle);

	int retvalue =
	    saunafs_removexattr(export->fsInstance, &op_ctx->creds, handle->inode,
	                        xattributeName->utf8string_val);

	if (retvalue < 0) {
		LogFullDebug(COMPONENT_FSAL, "REMOVEXATTR returned rc %d", retvalue);
		return saunafsToFsalError(retvalue);
	}

	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

void handleOperationsInit(struct fsal_obj_ops *ops) {
	fsal_default_obj_ops_init(ops);
	ops->release = release;
	ops->lookup = lookup;
	ops->readdir = readdir_;
	ops->getattrs = getattrs;
	ops->handle_to_wire = handle_to_wire;
	ops->handle_to_key = handle_to_key;
	ops->open2 = open2;
	ops->read2 = read2;
	ops->mkdir = mkdir_;
	ops->link = link_;
	ops->rename = rename_;
	ops->unlink = unlink_;
	ops->close = close_;
	ops->write2 = write2;
	ops->commit2 = commit2;
	ops->setattr2 = setattr2;
	ops->close2 = close2;
	ops->symlink = symlink_;
	ops->lock_op2 = lock_op2;
	ops->reopen2 = reopen2;
	ops->mknode = mknode;
	ops->readlink = readlink_;
	ops->status2 = status2;
	ops->merge = merge;
	ops->fallocate = fallocate;
	ops->getxattrs = getxattrs;
	ops->setxattrs = setxattrs;
	ops->listxattrs = listxattrs;
	ops->removexattrs = removexattrs;
}

/**
 * @brief Allocate a new file handle.
 *
 * This function constructs a new SaunaFS FSAL object handle and attaches
 * it to the export. After this call the attributes have been filled
 * in and the handdle is up-to-date and usable.
 *
 * @param[in] attribute     stat attributes for the handle
 * @param[in] export        The export on which the object lives
 *
 * @returns: saunafs_handle instance or NULL.
 */
struct SaunaFSHandle *allocateHandle(const struct stat *attribute,
                                     struct SaunaFSExport *export) {
	struct SaunaFSHandle *result = NULL;
	result = gsh_calloc(1, sizeof(struct SaunaFSHandle));

	result->inode = attribute->st_ino;
	result->key.moduleId = FSAL_ID_EXPERIMENTAL;
	result->key.exportId = export->export.export_id;
	result->key.inode = attribute->st_ino;

	fsal_obj_handle_init(&result->handle, &export->export,
	                     posix2fsal_type(attribute->st_mode));

	result->handle.obj_ops = &SaunaFS.handleOperations;
	result->handle.fsid = posix2fsal_fsid(attribute->st_dev);
	result->handle.fileid = attribute->st_ino;
	result->export = export;
	return result;
}

/**
 * @brief Release all resources for a handle.
 *
 * @param[in] object     Handle to release
 */
void deleteHandle(struct SaunaFSHandle *object) {
	fsal_obj_handle_fini(&object->handle);
	gsh_free(object);
}
