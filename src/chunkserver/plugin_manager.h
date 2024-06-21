/*
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

#pragma once

// A fix for https://stackoverflow.com/q/77034039/10788155
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#include <map>
#pragma GCC diagnostic pop

#include <boost/dll/import.hpp>
#include <boost/filesystem.hpp>
#include <boost/function.hpp>

#include "chunkserver-common/disk_plugin.h"

using IPlugin_t = boost::shared_ptr<IPlugin>();

/// Responsible for loading and managing plugins and for creating the concrete
/// Disks for the given prefixes.
class PluginManager {
public:
	/// Function for loading plugins from a given directory.
	/// The function returns true if all plugins were loaded successfully.
	bool loadPlugins(const std::string &directory);

	/// Returns a concrete Disk using \p configuration if the PluginManager
	/// contains a handler for this \p configuration prefix.
	IDisk *createDisk(const disk::Configuration &configuration);

	/// Shows information about the loaded plugins.
	void showLoadedPlugins();

	/// True if this plugin manager can handle the plugin based on the version.
	bool checkVersion(IPlugin *plugin);

private:
	/// Creators should never be out of scope or the plugin will be unloaded
	std::map<std::string, boost::function<IPlugin_t>> creators_;

	/// Container for all the loaded plugins
	std::map<std::string, boost::shared_ptr<IPlugin>> allPlugins_;

	/// Container for the loaded disk plugins
	std::map<std::string, boost::shared_ptr<DiskPlugin>> diskPlugins_;
};
