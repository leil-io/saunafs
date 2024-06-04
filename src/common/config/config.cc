#include <sys/stat.h>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <map>
#include <string>

#include "config.h"

enum Types {
	UINT16,
	UINT32,
	UINT64,
	INT16,
	INT32,
	INT64,
	STRING,
};

std::shared_ptr<Config> configPtr;

std::map<std::string, std::string> loadINIConfigFile(const std::string& configFileName) {
	std::map<std::string, std::string> config;
	std::ifstream file(configFileName);

	if (file.fail()) {
		// TODO(Urmas): Replace cerr with safs::log_err
		std::string errMsg = "Failed to open configuration file: " + std::string(strerror(errno));
		throw std::runtime_error(errMsg);
	}

	std::string line;

    while (getline(file, line)) {
		size_t commentPos = line.find('#');
		if (commentPos != std::string::npos) {
			line = line.substr(0, commentPos);
		}

		// Trim whitespace and ignore comments or empty lines
        line.erase(remove_if(line.begin(), line.end(), isspace), line.end());

        if (line.empty()) {
            continue;
        }

        std::istringstream iss(line);
        std::string key;
        std::string value;

        if (getline(iss, key, '=') && getline(iss, value)) {
            config[key] = value;
        }
    }

	return config;
}

std::shared_ptr<Config> Config::instance() {
	if (!configPtr) {
		configPtr = std::make_shared<Config>();
	}
	return configPtr;
}

// May throw std::runtime_error
void Config::readConfig(const std::string &filename, bool logUndefined) {
	auto iniConfig = loadINIConfigFile(filename);
	filename_ = filename;
	for (auto const &[key, value] : iniConfig) {
		if (options_.contains(key)) { options_[key].value = value; }
	}
	if (logUndefined) {
		for (auto const &[key, value] : options_) {
			// TODO(Urmas): Convert to safs::log_notice
			if (!iniConfig.contains(key)) {
				std::cerr << "config: using default value for option '" + key +
				                 "' - '" + value.value + "'\n";
			}
		}
	}
}

void Config::reloadConfig() {
	auto iniConfig = loadINIConfigFile(filename_);
	for (auto &[key, value] : options_) {
		// TODO(Urmas): Convert to safs::log_notice
		if (!iniConfig.contains(key)) {
			value.value = value.defaultValue;
		} else {
			options_[key].value = iniConfig[key];
		}
	}
}
