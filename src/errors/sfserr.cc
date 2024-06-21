/*
   Copyright 2013-2014 EditShare
   Copyright 2013-2015 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ

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

#include "common/platform.h"
#include "sfserr.h"

#include <cerrno>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#include "saunafs_error_codes.h"

#ifndef EDQUOT
# define EDQUOT ENOSPC
#endif
#ifndef ENOATTR
# ifdef ENODATA
#  define ENOATTR ENODATA
# else
#  define ENOATTR ENOENT
# endif
#endif

int saunafs_error_conv(uint8_t status) {
	switch (status) {
		case SAUNAFS_STATUS_OK:
			return 0;
		case SAUNAFS_ERROR_EPERM:
			return EPERM;
		case SAUNAFS_ERROR_ENOTDIR:
			return ENOTDIR;
		case SAUNAFS_ERROR_ENOENT:
			return ENOENT;
		case SAUNAFS_ERROR_EACCES:
			return EACCES;
		case SAUNAFS_ERROR_EEXIST:
			return EEXIST;
		case SAUNAFS_ERROR_EINVAL:
			return EINVAL;
		case SAUNAFS_ERROR_ENOTEMPTY:
			return ENOTEMPTY;
		case SAUNAFS_ERROR_IO:
			return EIO;
		case SAUNAFS_ERROR_EROFS:
			return EROFS;
		case SAUNAFS_ERROR_QUOTA:
			return EDQUOT;
		case SAUNAFS_ERROR_ENOATTR:
			return ENOATTR;
		case SAUNAFS_ERROR_ENOTSUP:
			return ENOTSUP;
		case SAUNAFS_ERROR_ERANGE:
			return ERANGE;
		case SAUNAFS_ERROR_ENAMETOOLONG:
			return ENAMETOOLONG;
		case SAUNAFS_ERROR_EFBIG:
			return EFBIG;
		case SAUNAFS_ERROR_EBADF:
			return EBADF;
		case SAUNAFS_ERROR_ENODATA:
			return ENODATA;
		case SAUNAFS_ERROR_OUTOFMEMORY:
			return ENOMEM;
		case SAUNAFS_ERROR_E2BIG:
			return E2BIG;
		default:
			return EINVAL;
	}
}

const char *strerr(int error_code) {
	static std::unordered_map<int, std::string> error_description;
	static std::mutex error_description_mutex;

	std::lock_guard<std::mutex> guard(error_description_mutex);
	auto it = error_description.find(error_code);
	if (it != error_description.end()) {
		return it->second.c_str();
	}

	const char *error_string = strerror(error_code);
	auto insert_it = error_description.insert({error_code, std::string(error_string)}).first;
	return insert_it->second.c_str();
}
