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
#include "mount/polonaise/options.h"

#include <iostream>
#include <boost/program_options.hpp>

#include "mount/sauna_client.h"
#include "mount/polonaise/setup.h"

std::istream& operator >> (std::istream& in, SugidClearMode& scm) {
	std::string token;
	in >> token;
	if (token == "never") {
		scm = SugidClearMode::kNever;
	} else if (token == "always") {
		scm = SugidClearMode::kAlways;
	} else if (token == "osx") {
		scm = SugidClearMode::kOsx;
	} else if (token == "bsd") {
		scm = SugidClearMode::kBsd;
	} else if (token == "ext") {
		scm = SugidClearMode::kExt;
	} else if (token == "sfs") {
		scm = SugidClearMode::kSfs;
	} else {
		namespace po = boost::program_options;
		throw po::validation_error(po::validation_error::invalid_option_value,
				"Invalid sugid-clear-mode");
	}
	return in;
}

std::ostream& operator << (std::ostream& out, SugidClearMode scm) {
	std::string s;
	switch (scm) {
		case SugidClearMode::kNever:
			s = "never";
			break;
		case SugidClearMode::kAlways:
			s = "always";
			break;
		case SugidClearMode::kOsx:
			s = "osx";
			break;
		case SugidClearMode::kBsd:
			s = "bsd";
			break;
		case SugidClearMode::kExt:
			s = "ext";
			break;
		case SugidClearMode::kSfs:
			s = "sfs";
			break;
	}
	out << s;
	return out;
}

void parse_command_line(int argc, char** argv, Setup& setup) {
	namespace po = boost::program_options;
	po::options_description desc("where OPTIONS are");
	try {
		desc.add_options()
			("help", "print help message")
			("master-host,H",
				po::value<std::string>(&setup.master_host)->default_value("sfsmaster"),
				"master host name")
			("master-port,P",
				po::value<std::string>(&setup.master_port)->default_value("9421"),
				"master port number")
			("bind-port,L",
#ifdef _WIN32
				po::value<int>(&setup.bind_port)->default_value(0),
#else
				po::value<int>(&setup.bind_port)->default_value(9423),
#endif
				"listen for incoming connections on given port")
			("mountpoint,M",
				po::value<std::string>(&setup.mountpoint)->default_value("/polonaise"),
				"mount point reported to master")
			("password",
				po::value<std::string>(&setup.password),
				"password for saunafs instance")
			("io-retries",
				po::value<uint32_t>(&setup.io_retries)->default_value((unsigned)SaunaClient::FsInitParams::kDefaultIoRetries),
				"number of retries for I/O failures")
			("write-buffer-size",
				po::value<uint32_t>(&setup.write_buffer_size)->default_value((unsigned)SaunaClient::FsInitParams::kDefaultWriteCacheSize),
				"size of global write buffer in MiB")
			("report-reserved-period",
				po::value<uint32_t>(&setup.report_reserved_period)->default_value((unsigned)SaunaClient::FsInitParams::kDefaultReportReservedPeriod),
				"period between reporting of reserved inodes expressed in seconds")
			("forget-password",
				po::bool_switch(&setup.forget_password)->default_value((bool)SaunaClient::FsInitParams::kDefaultDoNotRememberPassword),
				"forget password after successful registration")
			("subfolder,S",
				po::value<std::string>(&setup.subfolder)->default_value((const char *)SaunaClient::FsInitParams::kDefaultSubfolder),
				"mount only given subfolder of the file system")
			("debug",
				po::bool_switch(&setup.debug)->default_value((bool)SaunaClient::FsInitParams::kDefaultDebugMode),
				"enable debug mode")
			("direntry-cache-timeout",
				po::value<double>(&setup.direntry_cache_timeout)->default_value((unsigned)SaunaClient::FsInitParams::kDefaultDirentryCacheTimeout),
				"timeout for direntry cache")
			("direntry-cache-size",
				po::value<unsigned>(&setup.direntry_cache_size)->default_value((unsigned)SaunaClient::FsInitParams::kDefaultDirentryCacheSize),
				"size of direntry cache in number of elements")
			("entry-cache-timeout",
				po::value<double>(&setup.entry_cache_timeout)->default_value((unsigned)SaunaClient::FsInitParams::kDefaultEntryCacheTimeout),
				"timeout for entry cache")
			("attr-cache-timeout",
				po::value<double>(&setup.attr_cache_timeout)->default_value((unsigned)SaunaClient::FsInitParams::kDefaultAttrCacheTimeout),
				"timeout for attribute cache")
			("no-mkdir-copy-sgid",
				po::bool_switch(&setup.no_mkdir_copy_sgid)->default_value((bool)SaunaClient::FsInitParams::kDefaultMkdirCopySgid),
				"sgid bit should NOT be copied during mkdir operation")
			("sugid-clear-mode",
				po::value<SugidClearMode>(&setup.sugid_clear_mode)->default_value((SugidClearMode)SaunaClient::FsInitParams::kDefaultSugidClearMode),
				"set sugid clear mode")
			("daemonize",
				po::bool_switch(&setup.make_daemon)->default_value(false),
				"work in daemon mode")
			("enable-acl",
				po::bool_switch(&setup.enable_acl)->default_value(false),
				"DEPRECATED, used to enable/disable ACL support, ignored now")
#ifdef _WIN32
			("pipe-name,N",
			        po::value<std::string>(&setup.pipe_name)->default_value("polonaise-server-1"),
			        "name of pipe used for communication with client")
#endif
			;
		po::variables_map vm;
		po::store(po::parse_command_line(argc, argv, desc), vm);
		po::notify(vm);
		if (vm.count("help") > 0) {
			std::cout << "saunafs-polonaise-server OPTIONS" << std::endl;
			std::cout << desc << std::endl;
			exit(0);
		}
	} catch (const std::exception& e) {
		std::cerr << e.what() << std::endl;
		exit (1);
	}
}

