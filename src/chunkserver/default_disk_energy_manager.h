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

#pragma once

#include "common/platform.h"

#include "chunkserver-common/disk_energy_manager_interface.h"

/**
 * \brief Default implementation of the disk energy manager.
 *
 * This class implements a strategy for assigning chunks across all available
 * drives. The strategy aims to maintain a balanced distribution of chunks,
 * ensuring that each drive is utilized effectively.
 *
 * This was the approach used so far.
 */
class DefaultDiskEnergyManager : public IDiskEnergyManager {
public:
	/// Default constructor
	DefaultDiskEnergyManager() = default;

	// No need to copy or move them so far

	DefaultDiskEnergyManager(const DefaultDiskEnergyManager &) = delete;
	DefaultDiskEnergyManager(DefaultDiskEnergyManager &&) = delete;
	DefaultDiskEnergyManager &operator=(const DefaultDiskEnergyManager &) =
	    delete;
	DefaultDiskEnergyManager &operator=(DefaultDiskEnergyManager &&) = delete;

	/// Virtual destructor needed for correct polymorphism
	virtual ~DefaultDiskEnergyManager() = default;

	/**
	 * \brief Get the disk to store a new chunk.
	 *
	 * This concrete implementation obtains a balanced distribution using all
	 * available disks.
	 */
	IDisk* getDiskForNewChunk() override;
};
