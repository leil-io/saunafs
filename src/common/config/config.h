#pragma once

#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <utility>

struct Config {
	/// Get the instance of the Configuration class.
	static Config& instance();

	// Not needed methods
	Config(const Config &) = delete;
	Config &operator=(const Config &) = delete;
	Config(Config &&) = delete;
	Config &operator=(Config &&) = delete;

	/// Default destructor
	~Config() = default;

	/// Retrieves an option from the configuration file as uint32_t.
	/// \param optionName The name of the option to retrieve.
	/// In the future, the option will be cached and only read from the file
	/// once, unless reaload is requested.
	template<typename T>
	auto getOption(const std::string &name);

	/// Adds an option to the map. All options should be added at start time.
	/// \param option The option to add.
	template <typename T>
	void addOption(const std::pair<std::string, T> &option);

	// May throw std::runtime_error
	void readConfig(const std::string&, bool logUndefined);
};

