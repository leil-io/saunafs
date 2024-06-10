/*
   Copyright 2023 Leil Storage

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

#include "chunkserver/default_disk_manager.h"
#include "chunkserver-common/global_shared_resources.h"
#include "devtools/TracePrinter.h"

IDisk* DefaultDiskManager::getDiskForNewChunk() {
	TRACETHIS();
	IDisk *bestDisk = DiskNotFound;
	double maxCarry = 1.0;
	double minPercentAvail = std::numeric_limits<double>::max();
	double maxPercentAvail = 0.0;
	double s,d;
	double percentAvail;

	if (gDisks.empty()) {
		return DiskNotFound;
	}

	for (const auto &disk : gDisks) {
		if (!disk->isSelectableForNewChunk()) {
			continue;
		}

		if (disk->carry() >= maxCarry) {
			maxCarry = disk->carry();
			bestDisk = disk.get();
		}

		percentAvail = static_cast<double>(disk->availableSpace()) /
		               static_cast<double>(disk->totalSpace());
		minPercentAvail = std::min(minPercentAvail, percentAvail);
		maxPercentAvail = std::max(maxPercentAvail, percentAvail);
	}

	if (bestDisk != DiskNotFound) {
		// Lower the probability of being choosen again
		bestDisk->setCarry(bestDisk->carry() - 1.0);
		return bestDisk;
	}

	if (maxPercentAvail == 0.0) {  // no space
		return DiskNotFound;
	}

	if (maxPercentAvail < 0.01) {
		s = 0.0;
	} else {
		s = minPercentAvail * 0.8;
		if (s < 0.01) {
			s = 0.01;
		}
	}

	d = maxPercentAvail - s;
	maxCarry = 1.0;

	for (auto &disk : gDisks) {
		if (!disk->isSelectableForNewChunk()) {
			continue;
		}

		percentAvail = static_cast<double>(disk->availableSpace()) /
		               static_cast<double>(disk->totalSpace());

		if (percentAvail > s) {
			disk->setCarry(disk->carry() + ((percentAvail - s) / d));
		}

		if (disk->carry() >= maxCarry) {
			maxCarry = disk->carry();
			bestDisk = disk.get();
		}
	}

	if (bestDisk != DiskNotFound) {  // should be always true here
		bestDisk->setCarry(bestDisk->carry() - 1.0);
	}

	return bestDisk;
}
