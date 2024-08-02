/*
   Copyright 2013-2015 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ

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

#include <fstream>
#include <gtest/gtest.h>
#include <iostream>
#include <boost/filesystem.hpp>

#ifndef TEST_DATA_PATH
#       error "You have to define TEST_DATA_PATH to compile this file"
#endif

#define TO_STRING_AUX(x) #x
#define TO_STRING(x) TO_STRING_AUX(x)

namespace {

std::string tempDirForAllTests() {
	return "/tmp/Saunafs_bashtests_global_env";
}

// The file has to be available for all bash tests
// but it does not need to outlive the test environment
// it has to be in a separate folder due to
// sticky bit in /tmp
// It cannot be in TEMP_DIR, becaue the folder is cleaned
// after every test.
std::string testErrFilePath() {
	return tempDirForAllTests() + "/test_err";
}

}

class BashTestEnvironment : public ::testing::Environment {
	virtual void SetUp() {
		// remove prevoius temp dir
		std::string tempForAllTests = tempDirForAllTests();
		if (boost::filesystem::exists(tempForAllTests)) {
			boost::filesystem::remove_all(tempForAllTests);
		}
		// make temp dir
		boost::filesystem::create_directory(tempForAllTests);
		auto perms = boost::filesystem::all_all;
		boost::filesystem::permissions(tempForAllTests, perms);
	}
	virtual void TearDown() {
		// remove prevoius temp dir
		std::string tempForAllTests = tempDirForAllTests();
		if (boost::filesystem::exists(tempForAllTests)) {
			boost::filesystem::remove_all(tempForAllTests);
		}
	}
};

// Register global environment
const ::testing::Environment* gBashEnvironment = ::testing::AddGlobalTestEnvironment(new BashTestEnvironment);

class BashTestingSuite : public testing::Test {
protected:
	void run_test_case(std::string suite, std::string testCase) {
		std::string testDataPath = TO_STRING(TEST_DATA_PATH);
		std::string workspace = std::getenv("SFS_TEST_WORKSPACE");
		if (!workspace.empty()) {
			testDataPath = workspace + "/tests";
		}
		std::string runScript = testDataPath + "/run-test.sh";
		std::string testFile = testDataPath + "/test_suites/" + suite + "/" + testCase + ".sh";
		std::string errorFile = testErrFilePath();
		std::string environment = "ERROR_FILE=" + errorFile;
		environment += " TEST_SUITE_NAME=" + suite;
		environment += " TEST_CASE_NAME=" + testCase;
		std::string command = environment + " " + runScript + " " + testFile;
		make_error_file(errorFile);
		int ret = system(command.c_str());
		if (ret != 0) {
			std::string error;
			std::ifstream results(errorFile);
			if (!results) {
				error = "Script " + testFile + " crashed";
			} else {
				error.assign(
						std::istreambuf_iterator<char>(results),
						std::istreambuf_iterator<char>());
				if (!error.empty() && *error.rbegin() == '\n') {
					error.erase(error.size() - 1);
				}
			}
			FAIL() << error;
		}
	}
	void make_error_file(std::string errorFile) {
		unlink(errorFile.c_str());
		std::ofstream ofs(errorFile);
		ofs.close();
		auto perms = boost::filesystem::all_all;
		boost::filesystem::permissions(errorFile, perms);
	}
};

#define add_test_case(suite, testCase) \
		TEST_F(suite, testCase) { run_test_case(TO_STRING(suite), TO_STRING(testCase)); }

#include "test_suites.h"
#include "test_cases.h"
