/*


   Copyright 2017 Skytechnology sp. z o.o.
   Copyright 2023 Leil Storage OÜ

   This file is part of SaunaFS.

   SaunaFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   SaunaFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with SaunaFS  If not, see <http://www.gnu.org/licenses/>.
 */

#include "common/platform.h"

#include "saunafs_error_codes.h"

const char *saunafs_error_string(uint8_t status) {
	static const char * const error_strings[SAUNAFS_ERROR_MAX + 1] = {
		"OK",
		"Operation not permitted",
		"Not a directory",
		"No such file or directory",
		"Permission denied",
		"File exists",
		"Invalid argument",
		"Directory not empty",
		"Chunk lost",
		"Out of memory",
		"Index too big",
		"Chunk locked",
		"No chunk servers",
		"No such chunk",
		"Chunk is busy",
		"Incorrect register BLOB",
		"Requested operation not completed",
		"Group info is not registered in master server",
		"Write not started",
		"Wrong chunk version",
		"Chunk already exists",
		"No space left",
		"IO error",
		"Incorrect block number",
		"Incorrect size",
		"Incorrect offset",
		"Can't connect",
		"Incorrect chunk id",
		"Disconnected",
		"CRC error",
		"Operation delayed",
		"Can't create path",
		"Data mismatch",
		"Read-only file system",
		"Quota exceeded",
		"Bad session id",
		"Password is needed",
		"Incorrect password",
		"Attribute not found",
		"Operation not supported",
		"Result too large",
		"Timeout",
		"Metadata checksum not matching",
		"Changelog inconsistent",
		"Parsing unsuccessful",
		"Metadata version mismatch",
		"No such lock",
		"Wrong lock id",
		"Operation not possible",
		"Operation temporarily not possible",
		"Waiting for operation completion",
		"Unknown SaunaFS error",
		"Name too long",
		"File too large",
		"Bad file number",
		"No data available",
		"Argument list too long",
		"Unknown SaunaFS error"
	};

	status = (status <= SAUNAFS_ERROR_MAX) ? status : (uint8_t)SAUNAFS_ERROR_MAX;
	return error_strings[status];
}
