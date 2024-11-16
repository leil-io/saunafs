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

#include "common/platform.h"

#include "chunkserver-common/plugin_manager.h"
#include <boost/filesystem/exception.hpp>

#include "chunkserver-common/disk_plugin.h"
#include "slogger/slogger.h"

bool PluginManager::loadPlugins(const std::string &directory) {
	try {
		if (!boost::filesystem::is_directory(directory) ||
		    boost::filesystem::is_empty(directory)) {
			// It is normal to not have any plugins in many scenarios
			safs_pretty_syslog(
			    LOG_NOTICE,
			    "PluginManager: Directory %s does not exist or is empty",
			    directory.c_str());
			return false;
		}
	} catch (boost::filesystem::filesystem_error &e) {
			safs::log_info(
			    "PluginManager: Directory {} cannot not be checked: {}",
			    directory.c_str(), e.what());
			return false;
	}

	boost::filesystem::path dir(directory);
	bool loadedPluginsSuccessfully = true;

	for (auto &dirEntry : boost::filesystem::directory_iterator(dir)) {
		if (boost::filesystem::is_regular_file(dirEntry)) {
			std::string filename = dirEntry.path().filename().string();

			if (filename.find("lib") == std::string::npos) { continue; }

			creators_[filename] = boost::dll::import_alias<IPlugin_t>(
			    dirEntry.path(), "createPlugin",
			    boost::dll::load_mode::append_decorations);

			boost::shared_ptr<IPlugin> plugin = creators_[filename]();

			if (plugin != nullptr) {
				plugin->initialize();

				if (!checkVersion(plugin.get())) { continue; }

				allPlugins_.insert(std::make_pair(plugin->name(), plugin));

				boost::shared_ptr<DiskPlugin> diskPlugin =
				    boost::dynamic_pointer_cast<DiskPlugin>(plugin);

				if (diskPlugin != nullptr) {
					diskPlugins_.insert(std::make_pair(diskPlugin->prefix(),
					                                   std::move(diskPlugin)));
				}

				loadedPluginsSuccessfully &= true;
			} else {
				safs_pretty_errlog(LOG_NOTICE, "Unable to load plugin %s ",
				                   filename.c_str());
				loadedPluginsSuccessfully = false;
			}
		}
	}

	return loadedPluginsSuccessfully;
}

IDisk *PluginManager::createDisk(const disk::Configuration &configuration) {
	if (diskPlugins_.find(configuration.prefix) != diskPlugins_.end()) {
		return diskPlugins_[configuration.prefix]->createDisk(configuration);
	}

	return nullptr;
}

void PluginManager::showLoadedPlugins() {
	if (allPlugins_.empty()) { return; }

	safs_pretty_syslog(LOG_NOTICE, "Available plugins:");
	for (auto &[name, plugin] : allPlugins_) {
		if (plugin) {
			safs_pretty_syslog(LOG_NOTICE, "  %s", plugin->toString().c_str());
		}
	}
}

bool PluginManager::checkVersion(IPlugin *plugin) {
	if (plugin->version() == SAUNAFS_VERSHEX) { return true; }

	safs_pretty_syslog(
	    LOG_WARNING,
	    "Ignoring plugin: %s, it does not match the current version: %d.%d.%d.",
	    plugin->toString().c_str(), SAUNAFS_PACKAGE_VERSION_MAJOR,
	    SAUNAFS_PACKAGE_VERSION_MINOR, SAUNAFS_PACKAGE_VERSION_MICRO);

	return false;
}
