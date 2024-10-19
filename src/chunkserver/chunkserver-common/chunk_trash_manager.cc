/*
   Copyright 2023-2024  Leil Storage OÜ

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

#include <memory>

#include "chunk_trash_manager.h"
#include "chunk_trash_manager_impl.h"

ChunkTrashManager::ChunkTrashManager() : pImpl(
		std::make_unique<ChunkTrashManagerImpl>()) {}

ChunkTrashManager &ChunkTrashManager::instance() {
	static ChunkTrashManager instance;
	return instance;
}

int ChunkTrashManager::moveToTrash(const std::filesystem::path &filePath,
                                   const std::filesystem::path &diskPath,
                                   const std::time_t &deletionTime) {
	return pImpl->moveToTrash(filePath, diskPath, deletionTime);
}

void ChunkTrashManager::init(const std::string &diskPath) {
	pImpl->init(diskPath);
}

void ChunkTrashManager::collectGarbage() {
	pImpl->collectGarbage();
}
