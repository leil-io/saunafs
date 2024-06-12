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
#include "common/rotate_files.h"

#include <string>

#include "common/cwrap.h"
#include "common/exceptions.h"
#include "common/massert.h"
#include "common/slogger/slogger.h"

namespace {

void rotateFile(bool ifExistsOnly, const std::string& from, const std::string& to) {
	try {
		if (ifExistsOnly) {
			if (!fs::exists(from)) {
				return;
			}
		}
		fs::rename(from, to);
	} catch (const FilesystemException& ex) {
		safs_pretty_syslog(LOG_WARNING,
				"rename backup file %s to %s failed (%s)",
				from.c_str(), to.c_str(), ex.what());
	}
}

} // anonymous namespace

void rotateFiles(const std::string& file, int storedPreviousCopies, int byNumber) {
	sassert(byNumber > 0);
	// rename previous backups
	if (storedPreviousCopies > 0) {
		for (int n = storedPreviousCopies; n > 1; n--) {
			rotateFile(true,
					file + "." + std::to_string(n - byNumber),
					file + "." + std::to_string(n));
		}
		rotateFile(true, file, file + "." + std::to_string(byNumber));
	}
}
