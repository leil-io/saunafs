/*
   Copyright 2024      Leil Storage OÜ

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

#include "helpers.h"

#include <boost/asio/io_context.hpp>
#include <boost/process/environment.hpp>
#include <boost/process/search_path.hpp>
#include <boost/process/v2/process.hpp>
#include <iostream>
#include <string>
#include <vector>
#include "uraftcontroller.h"

/*
*
* Various function mappings to the helper scripts
*
*/

using namespace boost::process::v2;

int executeHelperCommand(const std::vector<std::string> &args) {
	boost::asio::io_context ctx;
	process assign(ctx, boost::process::search_path("saunafs-uraft-helper"),
	               args, boost::this_process::environment());
	return assign.wait();
}

void helpers::assign_ip() {
	executeHelperCommand({"assign-ip"});
}

void helpers::drop_ip() {
	executeHelperCommand({"drop-ip"});
}

void helpers::quick_stop(const std::string &host, const std::string &port) {
	executeHelperCommand({"quick-stop", host, port});
}

void helpers::stop() {
	auto args = {"stop"};
	boost::asio::io_context ctx;
	process assign(ctx, boost::process::search_path("sfsmaster"),
	               args, boost::this_process::environment());
	assign.wait();
}
