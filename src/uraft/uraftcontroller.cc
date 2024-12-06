#include "uraftcontroller.h"

#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstdint>
#include <cstdio>

#define BOOST_BIND_GLOBAL_PLACEHOLDERS
#include <boost/bind.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/version.hpp>

#include "common/time_utils.h"
#include "slogger/slogger.h"

static constexpr int32_t DEFAULT_CHECK_NODE_STATUS_PERIOD = 250;
static constexpr int32_t DEFAULT_CMD_STATUS_PERIOD = 100;
static constexpr int32_t DEFAULT_GETVERSION_TIMEOUT = 50;
static constexpr int32_t DEFAULT_PROMOTE_TIMEOUT = 1000000;
static constexpr int32_t DEFAULT_DEMOTE_TIMEOUT = 1000000;
static constexpr int32_t DEFAULT_DEAD_HANDLER_TIMEOUT = 1000000;

uRaftController::uRaftController(boost::asio::io_service &ios)
    : uRaftStatus(ios),
      check_cmd_status_timer_(ios),
      check_node_status_timer_(ios),
      cmd_timeout_timer_(ios),
      command_pid_(-1),
      command_type_(kCmdNone),
      force_demote_(false),
      node_alive_(false) {

	opt_.check_node_status_period = DEFAULT_CHECK_NODE_STATUS_PERIOD;
	opt_.check_cmd_status_period  = DEFAULT_CMD_STATUS_PERIOD;
	opt_.getversion_timeout       = DEFAULT_GETVERSION_TIMEOUT;
	opt_.promote_timeout          = DEFAULT_PROMOTE_TIMEOUT;
	opt_.demote_timeout           = DEFAULT_DEMOTE_TIMEOUT;
	opt_.dead_handler_timeout     = DEFAULT_DEAD_HANDLER_TIMEOUT;
}

void uRaftController::init() {
	uRaftStatus::init();

	set_block_promotion(true);
	if (opt_.elector_mode) {
		return;
	}

	check_cmd_status_timer_.expires_from_now(boost::posix_time::millisec(opt_.check_cmd_status_period));
	check_cmd_status_timer_.async_wait([this](const boost::system::error_code & error) { checkCommandStatus(error); });

	check_node_status_timer_.expires_from_now(boost::posix_time::millisec(opt_.check_node_status_period));
	check_node_status_timer_.async_wait([this](const boost::system::error_code & error) { checkNodeStatus(error); });

	safs::log_info("Saunafs-uraft initialized properly");
}

void uRaftController::set_options(const uRaftController::Options &opt) {
	uRaftStatus::set_options(opt);
	opt_ = opt;
}

void uRaftController::nodePromote() {
	safs::log_info("Starting metadata server switch to master mode");

	if (command_pid_ >= 0 && command_type_ != kCmdPromote) {
		safs::log_err("Can not switch metadata server to master during switch to slave");
		demoteLeader();
		set_block_promotion(true);
		return;
	}
	if (command_pid_ >= 0) {
		return;
	}

	setSlowCommandTimeout(opt_.promote_timeout);
	if (runSlowCommand("saunafs-uraft-helper promote")) {
		command_type_ = kCmdPromote;
	}
}

void uRaftController::nodeDemote() {
	safs::log_info("Starting metadata server switch to slave mode");

	if (command_pid_ >= 0 && command_type_ != kCmdDemote) {
		safs::log_err("Can not switch metadata server to slave during switch to master");
		force_demote_ = true;
		set_block_promotion(true);
		return;
	}
	if (command_pid_ >= 0) {
		return;
	}

	setSlowCommandTimeout(opt_.demote_timeout);
	if (runSlowCommand("saunafs-uraft-helper demote")) {
		command_type_ = kCmdDemote;
		set_block_promotion(true);
	}
}

uint64_t uRaftController::nodeGetVersion() {
	if (opt_.elector_mode) {
		return 0;
	}

	uint64_t result = 0;

	try {
		std::string version;

		std::vector<std::string> params = {
			"saunafs-uraft-helper", "metadata-version", opt_.local_master_server,
			boost::lexical_cast<std::string>(opt_.local_master_port)
		};

		if (!runCommand(params, version, opt_.getversion_timeout)) {
			safs::log_warn("Get metadata version timeout");
			return state_.data_version;
		}

		result = boost::lexical_cast<uint64_t>(version.c_str());
	} catch (...) {
		safs::log_err("Invalid metadata version value");
		result = state_.data_version;
	}

	return result;
}

void uRaftController::nodeLeader(int id) {
	if (id < 0) {
		return;
	}

	std::string name = opt_.server[id];
	std::string::size_type pos = name.find(':');

	if (pos != std::string::npos) {
		name = name.substr(0, pos);
	}

	safs::log_info("Node '{}' is now a leader.", name);
}

/*! \brief Check promote/demote script status. */
void uRaftController::checkCommandStatus(const boost::system::error_code &error) {
	if (error) {
		safs::log_err("Error checking command status: {}", error.message());
		return;
	}

	int status = 0;
	if (checkSlowCommand(status)) {
		cmd_timeout_timer_.cancel();
		if (command_type_ == kCmdDemote) {
			safs::log_info("Metadata server switch to slave mode done");
			command_type_ = kCmdNone;
			command_pid_  = -1;
			set_block_promotion(false);
		} else if (command_type_ == kCmdPromote) {
			safs::log_info("Metadata server switch to master mode done");
			node_alive_ = true;
			command_type_ = kCmdNone;
			command_pid_  = -1;
			if (force_demote_) {
				safs::log_warn("Staring forced switch to slave mode");
				nodeDemote();
				force_demote_ = false;
			}
		} else if (command_type_ == kCmdStatusDead) {
			safs::log_info("Waiting for new metadata server instance to be available");
			command_type_ = kCmdNone;
			command_pid_  = -1;
		}
	}

	check_cmd_status_timer_.expires_from_now(boost::posix_time::millisec(opt_.check_cmd_status_period));
	check_cmd_status_timer_.async_wait([this](const boost::system::error_code & error) { checkCommandStatus(error); });
}

/*! \brief Check metadata server status. */
void uRaftController::checkNodeStatus(const boost::system::error_code &error) {
	if (error) {
		safs::log_err("Error checking node status: {}", error.message());
		return;
	}

	std::vector<std::string> params = { "saunafs-uraft-helper", "isalive" };
	std::string              result;
	bool                     is_alive = node_alive_;

	if (command_type_ == kCmdNone) {
		if (runCommand(params, result, opt_.getversion_timeout)) {
			if (result == "alive" || result == "dead") {
				is_alive = result == "alive";
			} else {
				safs::log_err("Invalid metadata server status");
			}
		} else {
			safs::log_warn("Isalive timeout");
		}

		if (is_alive != node_alive_) {
			if (is_alive) {
				safs::log_info("Metadata server is alive");
				set_block_promotion(false);
			} else {
				safs::log_info("Metadata server is dead");
				demoteLeader();
				set_block_promotion(true);
				setSlowCommandTimeout(opt_.dead_handler_timeout);
				if (runSlowCommand("saunafs-uraft-helper dead")) {
					command_type_ = kCmdStatusDead;
				}
			}
			node_alive_ = is_alive;
		}
	}

	check_node_status_timer_.expires_from_now(boost::posix_time::millisec(opt_.check_node_status_period));
	check_node_status_timer_.async_wait([this](const boost::system::error_code & error) { checkNodeStatus(error); });
}

void uRaftController::setSlowCommandTimeout(int timeout) {
	cmd_timeout_timer_.expires_from_now(boost::posix_time::millisec(timeout));
	cmd_timeout_timer_.async_wait([this](const boost::system::error_code & error) {
		if (!error) {
			safs::log_err("Metadata server mode switching timeout");
			stopSlowCommand();
		}
	});
}

//! Check if slow command stopped working.
bool uRaftController::checkSlowCommand(int &status) const {
	if (command_pid_ < 0) {
		return false;
	}
	return waitpid(command_pid_, &status, WNOHANG) > 0;
}

//! Kills slow command.
bool uRaftController::stopSlowCommand() {
	if (command_pid_ < 0) {
		return false;
	}

	int status = 0;
	kill(command_pid_, SIGKILL);
	waitpid(command_pid_, &status, 0);

	command_pid_  = -1;
	command_type_ = kCmdNone;

	return true;
}

/*! \brief Start new program.
 *
 * \param cmd String with name and parameters of program to run.
 * \return true if there was no error.
 */
bool uRaftController::runSlowCommand(const std::string &cmd) {
	command_timer_.reset();

	io_service_.notify_fork(boost::asio::io_service::fork_prepare);

	command_pid_ = fork();
	if (command_pid_ == -1) {
		return false;
	}
	if (command_pid_ == 0) {
		execlp("/bin/sh", "/bin/sh", "-c", cmd.c_str(), NULL);
		exit(1);
	}

	io_service_.notify_fork(boost::asio::io_service::fork_parent);

	return true;
}

/*! \brief Start new program.
 *
 * \param cmd vector of string with name and parameters of program to run.
 * \param result string with the data that was written to stdout by program.
 * \param timeout time in ms after which the program will be killed.
 * \return true if there was no error and program did finish in timeout time.
 */
bool uRaftController::runCommand(const std::vector<std::string> &cmd, std::string &result, int timeout) {
	pid_t pid = 0;
	std::array<int, 2> pipe_fd{};

	if (pipe(pipe_fd.data()) == -1) {
		return false;
	}

	io_service_.notify_fork(boost::asio::io_service::fork_prepare);

	pid = fork();
	if (pid == -1) {
		close(pipe_fd[0]);
		close(pipe_fd[1]);
		return false;
	}

	if (pid == 0) {
		close(pipe_fd[0]);
		dup2(pipe_fd[1], 1);

		std::vector<const char *> argv(cmd.size() + 1, nullptr);
		for (int i = 0; i < (int)cmd.size(); i++) {
			argv[i] = cmd[i].c_str();
		}

		execvp(argv[0], (char * const *)argv.data());
		exit(1);
	}

	io_service_.notify_fork(boost::asio::io_service::fork_parent);

	close(pipe_fd[1]);

	int retValue = readString(pipe_fd[0], result, timeout);

	close(pipe_fd[0]);

	if (retValue <= 0) {
		kill(pid, SIGKILL);
	}

	int status = 0;
	waitpid(pid, &status, 0);

	return retValue > 0;
}

/*! Read string from file descriptor
 *
 * Reads data from file descriptor and store them in string (with timeout).
 * \param fd file descriptor to read
 * \param result string with read data.
 * \param timoeut time (ms) after which we stop reading data.
 * \return -1 error
 *         0  timeout did occur
 *         1  no error
 */
int uRaftController::readString(int fileDescriptor, std::string &result, const int timeout) {
	static const int read_size = 128;

	Timeout time{std::chrono::milliseconds(timeout)};
	std::array<char, read_size + 1> buff{};
	pollfd pdata{};

	pdata.fd      = fileDescriptor;
	pdata.events  = POLLIN;
	pdata.revents = 0;

	result.clear();

	while (true) {
		int retValue = 0;

		if (time.expired()) {
			return 0;
		}

		retValue = poll(&pdata, 1, time.remaining_ms());
		if (retValue <= 0) { return retValue; }

		retValue = read(fileDescriptor, buff.data(), read_size);
		if (retValue < 0) {
			return -1;
		}
		if (retValue == 0) {
			break;
		}

		buff.at(retValue) = 0;
		result += buff.data();
	}

	return 1;
}
