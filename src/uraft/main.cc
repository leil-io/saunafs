#include "common/platform.h"
#include "slogger/slogger.h"
#include "uraftcontroller.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <boost/asio/io_service.hpp>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>

#define BOOST_BIND_GLOBAL_PLACEHOLDERS
#include <boost/bind.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/program_options.hpp>
#include <boost/version.hpp>

static constexpr int DEFAULT_PORT = 9427;
static constexpr int DEFAULT_STATUS_PORT = 9428;
static constexpr int DEFAULT_ELECTION_MIN_TIMEOUT_MS = 400;
static constexpr int DEFAULT_ELECTION_MAX_TIMEOUT_MS = 600;
static constexpr int DEFAULT_HEARTBEAT_MS = 20;
static constexpr std::string DEFAULT_MASTER_ADDRESS = "localhost";
static constexpr int DEFAULT_MASTER_TO_CLIENT_LISTEN_PORT = 9421;
static constexpr int DEFAULT_LOCAL_MASTER_CHECK_PERIOD_MS = 250;
static constexpr bool DEFAULT_ELECTOR_MODE = false;
static constexpr int DEFAULT_GETVERSION_TIMEOUT_MS = 50;
static constexpr int DEFAULT_PROMOTE_TIMEOUT_MS = 1000000000;
static constexpr int DEFAULT_DEMOTE_TIMEOUT_MS = 1000000000;
static constexpr int DEFAULT_DEAD_HANDLER_TIMEOUT_MS = 1000000000;
static constexpr int DEFAULT_CHECK_CMD_PERIOD_MS = 100;

void parseOptions(int argc, char **argv, uRaftController::Options &opt,
                  bool &make_daemon, std::string &pidfile) {
	namespace po = boost::program_options;

	// clang-format off
	po::options_description generic("options");
	generic.add_options()
		(
		"help,h",
		"produce help message"
		)
		(
		"config,c",
		po::value<std::string>()->default_value(ETC_PATH "/saunafs-uraft.cfg"),
		"configuration file"
	);

	po::options_description config("Configuration");
	config.add_options()
	("id", po::value<int>(), "server id")
	("start-daemon,d", po::bool_switch()->default_value(false), "start in daemon mode")
	("pidfile,p", po::value<std::string>(), "pidfile name");

	po::options_description hidden;
	hidden.add_options()
		(
		"URAFT_ID",
		po::value<int>()->default_value(-1),
		"node id"
		)
		(
		"URAFT_PORT",
		po::value<int>()->default_value(DEFAULT_PORT),
		"node port"
		)
		(
		"URAFT_NODE_ADDRESS",
		po::value<std::vector<std::string>>(),
		"node address"
		)
		(
		"ELECTION_TIMEOUT_MIN",
		po::value<int>()->default_value(DEFAULT_ELECTION_MIN_TIMEOUT_MS),
		"election min timeout (ms)"
		)
		(
		"ELECTION_TIMEOUT_MAX",
		po::value<int>()->default_value(DEFAULT_ELECTION_MAX_TIMEOUT_MS),
		"election max timeout (ms)"
		)
		(
		"HEARTBEAT_PERIOD",
		po::value<int>()->default_value(DEFAULT_HEARTBEAT_MS),
		"heartbeat period (ms)"
		)
		(
		"LOCAL_MASTER_ADDRESS",
		po::value<std::string>()->default_value("localhost"),
		"local master address"
		)
		(
		"LOCAL_MASTER_MATOCL_PORT",
		po::value<int>()->default_value(DEFAULT_MASTER_TO_CLIENT_LISTEN_PORT),
		"local master-to-client port"
		)
		(
		"LOCAL_MASTER_CHECK_PERIOD",
		po::value<int>()->default_value(DEFAULT_LOCAL_MASTER_CHECK_PERIOD_MS),
		"local master check status period"
		)
		(
		"URAFT_ELECTOR_MODE",
		po::value<bool>()->default_value(DEFAULT_ELECTOR_MODE),
		"run in elector mode"
		)
		(
		"URAFT_GETVERSION_TIMEOUT",
		po::value<int>()->default_value(DEFAULT_GETVERSION_TIMEOUT_MS),
		"getversion timeout (ms)"
		)
		(
		"URAFT_PROMOTE_TIMEOUT",
		po::value<int>()->default_value(DEFAULT_PROMOTE_TIMEOUT_MS),
		"promote timeout (ms)"
		)
		(
		"URAFT_DEMOTE_TIMEOUT",
		po::value<int>()->default_value(DEFAULT_DEMOTE_TIMEOUT_MS),
		"demote timeout (ms)"
		)
		(
		"URAFT_DEAD_HANDLER_TIMEOUT",
		po::value<int>()->default_value(DEFAULT_DEAD_HANDLER_TIMEOUT_MS),
		"metadata server dead handler timeout (ms)"
		)
		(
		"URAFT_CHECK_CMD_PERIOD",
		po::value<int>()->default_value(DEFAULT_CHECK_CMD_PERIOD_MS),
		"check command status period(ms)"
		)
		(
		"URAFT_STATUS_PORT",
		po::value<int>()->default_value(DEFAULT_STATUS_PORT),
		"node status port"
		)
		(
		"URAFT_FLOATING_IP",
		po::value<std::string>(),
		"uraft floating ip address"
	);
	// clang-format on

	po::options_description cmdline_options;
	cmdline_options.add(generic).add(config).add(hidden);

	po::options_description config_file_options;
	config_file_options.add(hidden);

	po::options_description visible("Allowed options");
	visible.add(generic).add(config);

	po::positional_options_description positional_options;
	positional_options.add("URAFT_NODE_ADDRESS", -1);

	po::variables_map variable_map;
	po::store(po::command_line_parser(argc, argv)
	              .options(cmdline_options)
	              .positional(positional_options)
	              .run(),
	          variable_map);
	po::notify(variable_map);

	if (variable_map.count("help") != 0) {
		std::cout << visible << "\n";
		exit(EXIT_SUCCESS);
	}

	if (variable_map.count("config") != 0) {
		std::string config_file;

		config_file = variable_map["config"].as<std::string>();
		std::ifstream ifs(config_file.c_str());

		if (!ifs) {
			safs::log_err("Can not open configuration file: {}",
			              strerror(errno));
			exit(EXIT_FAILURE);
		}

		po::store(po::parse_config_file(ifs, config_file_options, true),
		          variable_map);
		po::notify(variable_map);
	}

	if (variable_map.count("URAFT_NODE_ADDRESS") == 0) {
		safs::log_err("Missing node address list");
		std::cout << visible << "\n";
		exit(EXIT_FAILURE);
	}

	// clang-format off
	opt.id                        = variable_map["URAFT_ID"].as<int>();
	opt.port                      = variable_map["URAFT_PORT"].as<int>();
	opt.server                    = variable_map["URAFT_NODE_ADDRESS"].as< std::vector< std::string > >();
	opt.election_timeout_min      = variable_map["ELECTION_TIMEOUT_MIN"].as<int>();
	opt.election_timeout_max      = variable_map["ELECTION_TIMEOUT_MAX"].as<int>();
	opt.heartbeat_period          = variable_map["HEARTBEAT_PERIOD"].as<int>();
	opt.check_node_status_period  = variable_map["LOCAL_MASTER_CHECK_PERIOD"].as<int>();
	opt.status_port               = variable_map["URAFT_STATUS_PORT"].as<int>();
	opt.elector_mode              = variable_map["URAFT_ELECTOR_MODE"].as<bool>();
	opt.getversion_timeout        = variable_map["URAFT_GETVERSION_TIMEOUT"].as<int>();
	opt.promote_timeout           = variable_map["URAFT_PROMOTE_TIMEOUT"].as<int>();
	opt.demote_timeout            = variable_map["URAFT_DEMOTE_TIMEOUT"].as<int>();
	opt.dead_handler_timeout      = variable_map["URAFT_DEAD_HANDLER_TIMEOUT"].as<int>();
	opt.local_master_server       = variable_map["LOCAL_MASTER_ADDRESS"].as<std::string>();
	opt.local_master_port         = variable_map["LOCAL_MASTER_MATOCL_PORT"].as<int>();
	opt.check_cmd_status_period   = variable_map["URAFT_CHECK_CMD_PERIOD"].as<int>();
	if (opt.elector_mode == 0) {
		opt.floating_ip           = variable_map["URAFT_FLOATING_IP"].as<std::string>();
	}
	make_daemon                   = variable_map["start-daemon"].as<bool>();
	// clang-format on

	if (variable_map.count("id") != 0) {
		opt.id = variable_map["id"].as<int>();
	}

	if (opt.id >= (int)opt.server.size()) {
		safs::log_err("Invalid node id");
		std::cout << visible << "\n";
		exit(EXIT_FAILURE);
	}

	if (variable_map.count("pidfile") != 0) {
		pidfile = variable_map["pidfile"].as<std::string>();
	}
}

int getSeed() {
	ssize_t readAmount = 0;
	int fileDescriptor = 0;
	int result = 0;

	readAmount = -1;
	fileDescriptor = open("/dev/urandom", O_RDONLY);
	if (fileDescriptor >= 0) {
		readAmount = read(fileDescriptor, &result, sizeof(result));
	}

	if (readAmount != sizeof(result)) { result = (int)std::time(nullptr); }

	if (fileDescriptor >= 0) { close(fileDescriptor); }

	return result;
}

bool daemonize() {
	pid_t pid = 0;

	pid = fork();
	if (pid != 0) {
		if (pid > 0) { exit(0); }
		safs::log_critical("First fork failed: {}", strerror(errno));
		return false;
	}

	setsid();
	int success = chdir("/");
	if (success != 0) {
		safs::log_critical("Change directory failed: {}", strerror(errno));
	}
	umask(0);

	pid = fork();
	if (pid != 0) {
		if (pid > 0) { exit(0); }

		safs::log_critical("Second fork failed: {}", strerror(errno));
		return false;
	}

	close(0);
	close(1);
	close(2);

	if (open("/dev/null", O_RDONLY) < 0) {
		safs::log_critical("Unable to open /dev/null: {}", strerror(errno));
		return false;
	}
	if (open("/dev/null", O_WRONLY) < 0) {
		safs::log_critical("Unable to open /dev/null: {}", strerror(errno));
		return false;
	}
	if (dup(1) < 0) {
		safs::log_critical("Unable to duplicate stdout descriptor: {}",
		                   strerror(errno));
		return false;
	}

	return true;
}

void makePidFile(const std::string &name) {
	if (name.empty()) { return; }

	std::ofstream ofs(name, std::ios_base::out | std::ios_base::trunc);
	ofs << boost::lexical_cast<std::string>(getpid()) << '\n';
}

int main(int argc, char **argv) {
	uRaftController::Options opt;
	bool make_daemon = false;
	std::string pidfile;

#ifdef NDEBUG
	auto logLevel = safs::log_level::info;
#else
	auto logLevel = safs::log_level::debug;
#endif
	safs::add_log_stderr(logLevel);

	if (const char *logLevelChar = std::getenv("SAUNAFS_LOG_LEVEL")) {
		auto result = safs::log_level_from_string(std::string(logLevelChar));
		if (result) {
			safs::drop_all_logs();
			safs::add_log_stderr(result.value());
		} else {
			safs::log_critical("Invalid SAUNAFS_LOG_LEVEL variable: {}",
			                   result.error());
			return EXIT_FAILURE;
		}
	} else {
		// This is needed for the helper script
		setenv("SAUNAFS_LOG_LEVEL", safs::log_level_to_string(logLevel).data(),
		       1);
	}
	safs::add_log_syslog();
	safs::set_log_flush_on(safs::log_level::critical);

	srand(getSeed());
	parseOptions(argc, argv, opt, make_daemon, pidfile);

	if (make_daemon && !daemonize()) {
		safs::log_critical("Unable to switch to daemon mode");
		return EXIT_FAILURE;
	}

	if (!opt.floating_ip.empty()) {
		safs::log_info("Setting URAFT_FLOATING_IP to {}", opt.floating_ip);
		setenv("URAFT_FLOATING_IP", opt.floating_ip.c_str(), 0);
	}

	boost::asio::io_service io_service;
	uRaftController server(io_service);
	boost::asio::signal_set signals(io_service, SIGINT, SIGTERM);

	try {
		server.set_options(opt);
		makePidFile(pidfile);
		signals.async_wait(
		    [&io_service](const boost::system::error_code & /*error*/,
		                  int /*signal*/) { io_service.stop(); });
		server.init();

		io_service.run();
	} catch (std::exception &e) {
		safs::log_critical("Fatal error: {}", e.what());
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}
