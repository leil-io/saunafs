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

#include "chunkserver-common/disk_interface.h"

/**
 * \brief Interface for disk energy management.
 *
 * This class is responsible for managing the energy of the disks.
 *
 * The most basic feature is to decide which disk to use for a new chunk.
 *
 * Concrete implementations will take care of the details of the energy
 * management.
 */
class IDiskEnergyManager {
public:
	/// Default constructor
	IDiskEnergyManager() = default;

	// No need to copy or move them so far

	IDiskEnergyManager(const IDiskEnergyManager &) = delete;
	IDiskEnergyManager(IDiskEnergyManager &&) = delete;
	IDiskEnergyManager &operator=(const IDiskEnergyManager &) = delete;
	IDiskEnergyManager &operator=(IDiskEnergyManager &&) = delete;

	/// Virtual destructor needed for correct polymorphism
	virtual ~IDiskEnergyManager() = default;

	/// Get the disk to use for a new chunk according to the energy management
	/// policy.
	virtual IDisk* getDiskForNewChunk() = 0;
};
