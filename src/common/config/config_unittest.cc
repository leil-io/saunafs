#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <fstream>
#include <memory>
#include <stdexcept>
#include "config.h"

#include <gtest/gtest.h>

class ConfigTest : public testing::Test {
protected:
	ConfigTest() {
		conf_ = std::make_unique<Config>();
	}
	std::unique_ptr<Config> conf_;
};

std::string createConfigFile(const std::string &config) {
	auto temp = boost::filesystem::unique_path();
	std::ofstream file(temp.native());
	file << config << "\n";

	file.close();

	return temp.native();
}

TEST_F(ConfigTest, TestInstance) {
	auto path = createConfigFile("PERSONALITY = shadow");

	auto config = Config::instance();
	config->addOption<std::string>({"PERSONALITY", "master"});
	config->readConfig(path, false);

	ASSERT_EQ(config->getOption<std::string>("PERSONALITY"), "shadow");
	config.reset();
	config = Config::instance();
	ASSERT_EQ(config->getOption<std::string>("PERSONALITY"), "shadow");
}

TEST_F(ConfigTest, ParseSimpleConfig) {
	auto path = createConfigFile("PERSONALITY = shadow");

	conf_->addOption<std::string>({"PERSONALITY", "master"});
	conf_->readConfig(path, false);

	ASSERT_EQ(conf_->getOption<std::string>("PERSONALITY"), "shadow");
}

TEST_F(ConfigTest, ParseAdvancedConfig) {
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

	auto path = createConfigFile(test_config);

	// Default values, must be set otherwise readConfig won't add the values
	conf_->addOption<std::string>({"SYSLOG_IDENT", "unknown"});
	conf_->addOption<bool>({"LOCK_MEMORY", true});
	conf_->addOption<int8_t>({"NICE_LEVEL", 20});
	conf_->addOption<bool>({"PREFER_LOCAL_CHUNKSERVER", false});

	conf_->readConfig(path, false);

	EXPECT_EQ(conf_->getOption<std::string>("SYSLOG_IDENT"), "sfsmaster");
	EXPECT_EQ(conf_->getOption<bool>("LOCK_MEMORY"), false);
	EXPECT_EQ(conf_->getOptionInt8("NICE_LEVEL"), -19);
	EXPECT_EQ(conf_->getOption<bool>("PREFER_LOCAL_CHUNKSERVER"), false);
}

TEST_F(ConfigTest, NoConfigFile) {
	try {
	conf_->readConfig("non-existant", true);
	} catch (std::runtime_error& e) {
		std::cout << e.what() << '\n';
		return;
	}
	FAIL() << "Successfully loaded a non-existant config file";
}

TEST_F(ConfigTest, MinMaxValues) {
	auto config = Config::instance();
	const auto *test_config = R"V0G0N(
MAX256 = 512
MIN10 = 5
MIN10MAX50 = 5
)V0G0N";

	conf_->addOption<uint16_t>({"MAX256", 50});
	EXPECT_EQ(conf_->getMaxValue<uint16_t>("MAX256", 256), 50);
	conf_->addOption<uint16_t>({"MIN10", 15});
	EXPECT_EQ(conf_->getMinValue<uint16_t>("MIN10", 10), 15);
	conf_->addOption<uint16_t>({"MIN10MAX50", 15});
	EXPECT_EQ(conf_->getMinMaxValue<uint16_t>("MIN10MAX50", 10, 50), 15);

	auto path = createConfigFile(test_config);
	conf_->readConfig(path, false);

	EXPECT_EQ(conf_->getMaxValue<uint16_t>("MAX256", 256), 256);
	EXPECT_EQ(conf_->getMinValue<uint16_t>("MIN10", 10), 10);

	test_config = R"V0G0N(
MAX256 = 512
MIN10 = 5
MIN10MAX50 = 55
)V0G0N";

	path = createConfigFile(test_config);
	conf_->readConfig(path, false);

	EXPECT_EQ(conf_->getMinMaxValue<uint16_t>("MIN10MAX50", 10, 50), 50);
}

TEST_F(ConfigTest, ReloadConfig) {
	const auto *test_config = R"V0G0N(
## (Default: sfsmaster)
SYSLOG_IDENT = mymaster

## (Default: 0)
LOCK_MEMORY = 1

## (Default: -19)
NICE_LEVEL = 0


## (Default: 1)
PREFER_LOCAL_CHUNKSERVER = 0
)V0G0N";

	conf_->addOption<std::string>({"SYSLOG_IDENT", "sfsmaster"});
	conf_->addOption<bool>({"LOCK_MEMORY", false});
	conf_->addOption<int8_t>({"NICE_LEVEL", -19});
	conf_->addOption<bool>({"PREFER_LOCAL_CHUNKSERVER", true});

	auto path = createConfigFile(test_config);
	conf_->readConfig(path, false);

	EXPECT_EQ(conf_->getOption<std::string>("SYSLOG_IDENT"), "mymaster");
	EXPECT_EQ(conf_->getOption<bool>("LOCK_MEMORY"), true);
	EXPECT_EQ(conf_->getOptionInt8("NICE_LEVEL"), 0);
	EXPECT_EQ(conf_->getOption<bool>("PREFER_LOCAL_CHUNKSERVER"), false);

	test_config = R"V0G0N(
## (Default: -19)
NICE_LEVEL = 5
)V0G0N";

	std::ofstream file(path);
	file << test_config << "\n";
	file.close();
	conf_->reloadConfig();

	EXPECT_EQ(conf_->getOption<std::string>("SYSLOG_IDENT"), "sfsmaster");
	EXPECT_EQ(conf_->getOption<bool>("LOCK_MEMORY"), false);
	EXPECT_EQ(conf_->getOptionInt8("NICE_LEVEL"), 5);
	EXPECT_EQ(conf_->getOption<bool>("PREFER_LOCAL_CHUNKSERVER"), true);
}
