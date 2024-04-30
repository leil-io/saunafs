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

using DiskPlugin_t = boost::shared_ptr<DiskPlugin>();

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
	bool checkVersion(boost::shared_ptr<DiskPlugin> &plugin);

private:
	/// Creators should never be out of scope or the plugin will be unloaded
	std::map<std::string, boost::function<DiskPlugin_t>> creators_;
	/// Container for the loaded plugins
	std::map<std::string, boost::shared_ptr<DiskPlugin>> plugins_;
};
