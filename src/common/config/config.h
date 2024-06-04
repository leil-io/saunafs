#pragma once

#include <cassert>
#include <charconv>
#include <cstdint>
#include <exception>
#include <iostream>
#include <sstream>
#include <map>
#include <string>
#include <system_error>
#include <utility>


struct ConfigValue {
	std::string value;
	std::string defaultValue;
};

// Helper to check if `operator<<` is defined for type T
template <typename T, typename = void>
struct has_insertion_operator : std::false_type {};
template <typename T>
struct has_insertion_operator<T, std::void_t<decltype(std::declval<std::ostream&>() << std::declval<T>())>> : std::true_type {};

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
	/// WARNING: This doesn't work as expected for uint8/int8 types, use the
	/// getOptionInt8/getOptionUint8 for that
	template<typename T>
	T getOption(const std::string &name) {
		auto ite = options_.find(name);
		if (ite != options_.end()) {
			T result;
			std::istringstream iss(ite->second.value);
			if (!(iss >> result)) {
				std::cerr << "Type conversion failed for key: " + name + '\n';
				std::cerr << "Using default value for this key: " + name + '\n';
				std::istringstream iss(ite->second.defaultValue);
				assert(iss >> result);
			}
			return result;
		}
		throw std::runtime_error("Key not found: " + name);
	};

	int8_t getOptionInt8(const std::string &name) {
		return getOption8_<int8_t>(name);
	}

	uint8_t getOptionUint8(const std::string &name) {
		return getOption8_<uint8_t>(name);
	}

	/// Adds an option to the map. All options should be added at start time.
	/// \param option The option to add.
	template <typename T>
	void addOption(const std::pair<std::string, T> &option) {
		// Check if you are actually declaring the correct value
		static_assert(has_insertion_operator<T>::value,
		              "Type T must support output to std::ostream "
		              "(operator<< must be defined)");

		// TODO(Urmas): Replace cerr with safs::log_warn
		if (options_.find(option.first) != options_.end()) {
			std::cerr << "Option " << option.first.c_str()
			          << "already exists in the configuration";
			return;
		}

		std::ostringstream oss;
		oss << option.second;
		std::string valueAsString = oss.str();
		ConfigValue value(valueAsString, valueAsString);

		options_.insert({option.first, value});
	};

	// May throw std::runtime_error
	void readConfig(const std::string& filename, bool logUndefined);

private:
	/// Private constructor for the singleton pattern
	Config() = default;

	template <typename T>
	T getOption8_(const std::string& name) {
		auto ite = options_.find(name);
		if (ite != options_.end()) {
			auto value = ite->second;
			try {
				auto result = std::stoi(value.value, nullptr);
				return static_cast<T>(result);
			} catch (std::exception const &ex) {
				std::cerr << "Type conversion failed for key: " + name + '\n';
				std::cerr << "Type conversion error: " << ex.what() << '\n';
				std::cerr << "Using default value for this key: " + name + '\n';
				// If this fails, we're fucked
				size_t result{};
				std::stoi(value.defaultValue, &result);
				return static_cast<uint8_t>(result);
			}
		}
		throw std::runtime_error("Key not found: " + name);
	};

	/// Map with all the configuration options.
	std::map<std::string, ConfigValue> options_{};
};

