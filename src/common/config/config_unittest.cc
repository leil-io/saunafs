#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <fstream>
#include <stdexcept>
#include "config.cc"

#include <gtest/gtest.h>

TEST(INIConfigParsing, ParseSimpleConfig) {
	const auto *test_config = "PERSONALITY = master";
	auto temp = boost::filesystem::unique_path();
	std::ofstream file(temp.native());
	file << test_config << "\n";

	file.close();

	auto config = loadINIConfigFile(temp.native().data());
	ASSERT_EQ(config.count("PERSONALITY"), 1);
	EXPECT_EQ(config["PERSONALITY"], "master");
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

	auto config = loadINIConfigFile(temp.native().data());

	ASSERT_EQ(config.count("SYSLOG_IDENT"), 1);
	EXPECT_EQ(config["SYSLOG_IDENT"], "sfsmaster");

	ASSERT_EQ(config.count("LOCK_MEMORY"), 1);
	EXPECT_EQ(config["LOCK_MEMORY"], "0");

	ASSERT_EQ(config.count("NICE_LEVEL"), 1);
	EXPECT_EQ(config["NICE_LEVEL"], "-19");

	ASSERT_EQ(config.count("PREFER_LOCAL_CHUNKSERVER"), 0);
}

TEST(INIConfigParsing, NoConfigFile) {
	try {
	auto config = loadINIConfigFile("non-existant");
	} catch (std::runtime_error& e) {
		std::cout << e.what() << '\n';
		return;
	}
	FAIL() << "Successfully loaded a non-existant config file";
}
