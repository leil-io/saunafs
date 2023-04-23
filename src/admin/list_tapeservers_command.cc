/*
   Copyright 2013-2014 EditShare
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

#include "common/platform.h"
#include "admin/list_tapeservers_command.h"

#include <iomanip>
#include <iostream>

#include "protocol/cltoma.h"
#include "common/saunafs_version.h"
#include "protocol/matocl.h"
#include "common/server_connection.h"

std::string ListTapeserversCommand::name() const {
	return "list-tapeservers";
}

void ListTapeserversCommand::usage() const {
	std::cerr << name() << " <master ip> <master port>" << std::endl;
	std::cerr << "    Prints status of active tapeservers" << std::endl;
}

SaunaFsProbeCommand::SupportedOptions ListTapeserversCommand::supportedOptions() const {
	return { {kPorcelainMode, kPorcelainModeDescription} };
}

void ListTapeserversCommand::run(const Options& options) const {
	if (options.arguments().size() != 2) {
		throw WrongUsageException("Expected <master ip> and <master port> for " + name());
	}

	ServerConnection connection(options.argument(0), options.argument(1));
	auto request = cltoma::listTapeservers::build();
	auto response = connection.sendAndReceive(request, SAU_MATOCL_LIST_TAPESERVERS);

	std::vector<TapeserverListEntry> tapeservers;
	matocl::listTapeservers::deserialize(response, tapeservers);

	for (const auto& t : tapeservers) {
		if (options.isSet(kPorcelainMode)) {
			std::cout << t.address.toString() << ' ' << t.server << ' '
					<< saunafsVersionToString(t.version) << ' ' << t.label << std::endl;
		} else {
			std::cout << "Server " << t.server << ":"
					<< "\n\tversion: " << saunafsVersionToString(t.version)
					<< "\n\taddress: " << t.address.toString()
					<< "\n\tlabel: " << t.label << std::endl;
		}
	}
}
