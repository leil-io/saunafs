/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA
   Copyright 2013-2014 EditShare
   Copyright 2013-2015 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ


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
#include "master/filesystem_checksum_background_updater.h"

#include "common/saunafs_version.h"
#include "master/filesystem_checksum.h"
#include "master/filesystem_metadata.h"
#include "master/filesystem_operations.h"
#include "master/personality.h"

ChecksumBackgroundUpdater::ChecksumBackgroundUpdater()
	: speedLimit_(0) {  // Not important, redefined in fs_read_config_file()
	reset();
}

bool ChecksumBackgroundUpdater::start() {
	safs::log_trace("master.fs.checksum.updater_start");
	if (step_ == ChecksumRecalculatingStep::kNone) {
		++step_;
		return true;
	} else {
		return false;
	}
}

void ChecksumBackgroundUpdater::end() {
	updateChecksum();
	reset();
	safs::log_trace("master.fs.checksum.updater_end");
}

bool ChecksumBackgroundUpdater::inProgress() {
	return step_ != ChecksumRecalculatingStep::kNone;
}

ChecksumRecalculatingStep ChecksumBackgroundUpdater::getStep() {
	return step_;
}

void ChecksumBackgroundUpdater::incStep() {
	++step_;
	position_ = 0;
}

int32_t ChecksumBackgroundUpdater::getPosition() {
	return position_;
}

void ChecksumBackgroundUpdater::incPosition() {
	++position_;
}

bool ChecksumBackgroundUpdater::isNodeIncluded(FSNode *node) {
	auto ret = false;
	if (step_ > ChecksumRecalculatingStep::kNodes) {
		ret = true;
	}
	if (step_ == ChecksumRecalculatingStep::kNodes && NODEHASHPOS(node->id) < position_) {
		ret = true;
	}
	if (ret) {
		safs::log_trace("master.fs.checksum.changing_recalculated_node");
	} else {
		safs::log_trace("master.fs.checksum.changing_not_recalculated_node");
	}
	return ret;
}

bool ChecksumBackgroundUpdater::isXattrIncluded(xattr_data_entry *xde) {
	auto ret = false;
	if (step_ > ChecksumRecalculatingStep::kXattrs) {
		ret = true;
	}
	if (step_ == ChecksumRecalculatingStep::kXattrs &&
	    xattr_data_hash_fn(xde->inode, xde->anleng, xde->attrname) < position_) {
		ret = true;
	}
	if (ret) {
		safs::log_trace("master.fs.checksum.changing_recalculated_xattr");
	} else {
		safs::log_trace("master.fs.checksum.changing_not_recalculated_xattr");
	}
	return ret;
}

void ChecksumBackgroundUpdater::setSpeedLimit(uint32_t value) {
	speedLimit_ = value;
}

uint32_t ChecksumBackgroundUpdater::getSpeedLimit() {
	return speedLimit_;
}

void ChecksumBackgroundUpdater::updateChecksum() {
	if (fsNodesChecksum != gMetadata->fsNodesChecksum) {
		safs_pretty_syslog(LOG_WARNING, "FsNodes checksum mismatch found, replacing with a new value.");
		gMetadata->fsNodesChecksum = fsNodesChecksum;
		safs::log_trace("master.fs.checksum.mismatch");
	}
	if (xattrChecksum != gMetadata->xattrChecksum) {
		safs_pretty_syslog(LOG_WARNING, "Xattr checksum mismatch found, replacing with a new value.");
		gMetadata->xattrChecksum = xattrChecksum;
		safs::log_trace("master.fs.checksum.mismatch");
	}
}

void ChecksumBackgroundUpdater::reset() {
	position_ = 0;
	step_ = ChecksumRecalculatingStep::kNone;
	fsNodesChecksum = NODECHECKSUMSEED;
	xattrChecksum = XATTRCHECKSUMSEED;
}
