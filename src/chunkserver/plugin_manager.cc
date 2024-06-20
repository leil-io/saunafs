#include "plugin_manager.h"

#include <sys/syslog.h>

#include "slogger/slogger.h"

bool PluginManager::loadPlugins(const std::string &directory) {
	if (!boost::filesystem::is_directory(directory) ||
	    boost::filesystem::is_empty(directory)) {
		// It is normal to not have any plugins in many scenarios
		safs_pretty_syslog(
		    LOG_NOTICE,
		    "PluginManager: Directory %s does not exist or is empty",
		    directory.c_str());
		return false;
	}

	boost::filesystem::path dir(directory);
	bool loadedPluginsSuccessfully = true;

	for (auto &dirEntry : boost::filesystem::directory_iterator(dir)) {
		if (boost::filesystem::is_regular_file(dirEntry)) {
			std::string filename = dirEntry.path().filename().string();

			if (filename.find("lib") == std::string::npos) {
				continue;
			}

			creators_[filename] = boost::dll::import_alias<DiskPlugin_t>(
			    dirEntry.path(), "createPlugin",
			    boost::dll::load_mode::append_decorations);

			boost::shared_ptr<DiskPlugin> plugin = creators_[filename]();

			if (plugin != nullptr) {
				plugin->initialize();

				if (!checkVersion(plugin)) {
					continue;
				}

				plugins_.insert(
				    std::make_pair(plugin->prefix(), std::move(plugin)));
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
	if (plugins_.find(configuration.prefix) != plugins_.end()) {
		return plugins_[configuration.prefix]->createDisk(configuration);
	}

	return nullptr;
}

void PluginManager::showLoadedPlugins() {
	if (plugins_.empty()) {
		return;
	}

	safs_pretty_syslog(LOG_NOTICE, "Available plugins:");
	for (auto &[name, plugin] : plugins_) {
		if (plugin) {
			safs_pretty_syslog(LOG_NOTICE, "  %s",
			                   plugin->toString().c_str());
		}
	}
}

bool PluginManager::checkVersion(boost::shared_ptr<DiskPlugin> &plugin) {
	if (plugin->version() == SAUNAFS_VERSHEX) {
		return true;
	}

	safs_pretty_syslog(
	    LOG_WARNING,
	    "Ignoring plugin: %s, it does not match the current version: %d.%d.%d.",
	    plugin->toString().c_str(), SAUNAFS_PACKAGE_VERSION_MAJOR,
	    SAUNAFS_PACKAGE_VERSION_MINOR, SAUNAFS_PACKAGE_VERSION_MICRO);

	return false;
}
