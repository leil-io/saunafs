/*
   Copyright 2023 Leil Storage OÜ

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

#include "common/platform.h"

#include <unistd.h>
#include <iostream>

#include "admin/dump_config_command.h"
#include "admin/registered_admin_connection.h"
#include "admin/saunafs_admin_command.h"
#include "protocol/cltoma.h"
#include "protocol/matocl.h"

std::string DumpConfigurationCommand::name() const { return "dump-config"; }

void DumpConfigurationCommand::usage() const {
	std::cerr << name() << " <master ip> <master port>"
	          << "\n";
	std::cerr << "    Dumps the configuration files of the master server\n";
	std::cerr << "    Authentication with the admin password is required\n";
}

SaunaFsProbeCommand::SupportedOptions DumpConfigurationCommand::supportedOptions() const {
	return {{defaultsMode, "Return default values as well."}};
}

void DumpConfigurationCommand::run(const Options &options) const {
	if (options.arguments().size() != 2) {
		throw WrongUsageException(
		    "Expected <master ip> and <master port> for " + name());
	}

	auto connection = RegisteredAdminConnection::create(options.argument(0),
	                                                    options.argument(1));
	auto adminResponse = connection->sendAndReceive(
	    cltoma::adminDumpConfiguration::build(options.isSet(defaultsMode)), SAU_MATOCL_ADMIN_DUMP_CONFIG);

	std::string configs;
	matocl::adminDumpConfiguration::deserialize(adminResponse, configs);
	std::cout << configs << "\n";
}
