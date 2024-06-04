#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <fstream>
#include <stdexcept>
#include "config.h"

#include <gtest/gtest.h>

TEST(INIConfigParsing, ParseSimpleConfig) {
	const auto *test_config = "PERSONALITY = shadow";
	auto temp = boost::filesystem::unique_path();
	std::ofstream file(temp.native());
	file << test_config << "\n";

	file.close();

	auto &config = Config::instance();
	config.addOption<std::string>({"PERSONALITY", "master"});
	config.readConfig(temp.native(), false);
	ASSERT_EQ(config.getOption<std::string>("PERSONALITY"), "shadow");
}

TEST(INIConfigParsing, ParseAdvancedConfig) {
	const auto *test_config = R"V0G0N(
## Name of process to place in syslog messages.
## (Default: sfsmaster)
SYSLOG_IDENT = sfsmaster

## Whether to perform "mlockall()" to avoid swapping out sfsmaster process,
## boolean value (0 or 1).
## (Default: 0)
LOCK_MEMORY = 0 # Inline comments

## Nice level to run daemon with, when possible to set.
## (Default: -19)
NICE_LEVEL = -19


## If set, servers with the same IP address will be treated
## as their topology distance is 0.
## (Default: 1)
# PREFER_LOCAL_CHUNKSERVER = 1

)V0G0N";

	auto temp = boost::filesystem::unique_path();
	std::ofstream file(temp.native());
	file << test_config << "\n";

	file.close();

	auto &config = Config::instance();
	// Default values, must be set otherwise readConfig won't add the values
	config.addOption<std::string>({"SYSLOG_IDENT", "unknown"});
	config.addOption<bool>({"LOCK_MEMORY", true});
	config.addOption<int8_t>({"NICE_LEVEL", 20});
	config.addOption<bool>({"PREFER_LOCAL_CHUNKSERVER", false});

	config.readConfig(temp.native(), false);

	EXPECT_EQ(config.getOption<std::string>("SYSLOG_IDENT"), "sfsmaster");

	EXPECT_EQ(config.getOption<bool>("LOCK_MEMORY"), false);

	EXPECT_EQ(config.getOptionInt8("NICE_LEVEL"), -19);

	EXPECT_EQ(config.getOption<bool>("PREFER_LOCAL_CHUNKSERVER"), false);
}

TEST(INIConfigParsing, NoConfigFile) {
	try {
	auto &config = Config::instance();
	config.readConfig("non-existant", true);
	} catch (std::runtime_error& e) {
		std::cout << e.what() << '\n';
		return;
	}
	FAIL() << "Successfully loaded a non-existant config file";
}
