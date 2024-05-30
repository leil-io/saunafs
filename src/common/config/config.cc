#include <sys/stat.h>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <map>
#include <string>

enum Types {
	UINT16,
	UINT32,
	UINT64,
	INT16,
	INT32,
	INT64,
	STRING,
};

std::map<std::string, std::string> loadINIConfigFile(const char* configFileName) {
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
	static Config& instance() {
		static Config instance;
		return instance;
	}

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
	auto getOption(const std::string &name) {
		auto ite = options_.find(name);
		if (ite != options_.end()) {
			T result;
			std::istringstream iss(ite->second.value);
			if (!(iss >> result)) {
				std::cerr << "Type conversion failed for key: " + name + '\n';
				std::cerr << "Using default value for this key: " + name + '\n';
				// This is most of the time a safe option, due to a compile time
				// check to make sure the initial addOption calls are correctly
				// setting the value. However it can still fail if the getOption
				// T differs (and if they do, shame on you!)
				return static_cast<T>(ite->second.defaultValue);
			}
			return result;
		}
		throw std::runtime_error("Key not found: " + name);
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
			std::cerr << "Option " << option.first.c_str() << "already exists in the configuration";
			return;
		}

		std::ostringstream oss;
		oss << option.second;
		std::string valueAsString = oss.str();
		ConfigValue value(valueAsString, valueAsString);

		options_.insert({option.first, value});
	}

	// May throw std::runtime_error
	void readConfig(const char *filename, const bool logUndefined) {
		auto iniConfig = loadINIConfigFile(filename);
		for (auto const& [key, value] : iniConfig) {
			if (options_.contains(key)) {
				options_[key].value = value;
			}
		}
		if (logUndefined) {
			for (auto const& [key, value] : options_) {
				// TODO(Urmas): Convert to safs::log_notice
				if (!iniConfig.contains(key)) {
					std::cerr << "config: using default value for option '" + key + "' - '" + value.value + "'";
				}
			}
		}
	}

private:
	/// Private constructor for the singleton pattern
	Config() = default;

	/// Map with all the configuration options.
	std::map<std::string, ConfigValue> options_{};
};
