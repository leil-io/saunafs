/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA
   Copyright 2013-2014 EditShare
   Copyright 2013-2019 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ


   SaunaFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   SaunaFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with SaunaFS  If not, see <http://www.gnu.org/licenses/>.
 */

#include "common/platform.h"
#include "init.h"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <ios>
#include <malloc.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <fstream>
#include <iostream>
#include <list>
#include <memory>

#include "config/cfg.h"
#include "common/crc.h"
#include "common/cwrap.h"
#include "common/event_loop.h"
#include "common/exceptions.h"
#include "common/exit_status.h"
#include "common/main.h"
#include "common/massert.h"
#include "common/setup.h"
#include "common/time_utils.h"
#include "common/version.h"
#include "protocol/SFSCommunication.h"
#include "slogger/slogger.h"

#if defined(SAUNAFS_HAVE_MLOCKALL)
#  if defined(SAUNAFS_HAVE_SYS_MMAN_H)
#    include <sys/mman.h>
#  endif
#  if defined(SAUNAFS_HAVE_SYS_RESOURCE_H)
#    include <sys/resource.h>
#  endif
#  if defined(RLIMIT_MEMLOCK) && defined(MCL_CURRENT) && defined(MCL_FUTURE)
#    define SFS_USE_MEMLOCK 1
#  endif
#endif

#if defined(SAUNAFS_HAVE_PAM)
#  include <security/pam_appl.h>
#  include <security/pam_misc.h>
#endif

#ifndef CFGNAME
#define CFGNAME APPNAME
#endif

#ifndef SAUNAFS_MAX_FILES
#  define SAUNAFS_MAX_FILES 5000
#endif

#if defined(SAUNAFS_HAVE_SYSTEMD_SD_DAEMON_H)
#include <systemd/sd-daemon.h>
#endif

#define STR_AUX(x) #x
#define STR(x) STR_AUX(x)

enum class RunMode {
	kRestart = 0,
	kStart = 1,
	kStop = 2,
	kReload = 3,
	kTest = 4,
	kKill = 5,
	kIsAlive = 6
};

#if defined(SAUNAFS_HAVE_PAM)
static pam_handle_t *gPAMHandle=NULL;
#endif

static bool gRunAsDaemon = true;

constexpr uint8_t kDefaultLimitGlibcArenas = 0;
constexpr const char *kEnvironmentVariableNotDefined = nullptr;
static uint8_t gLimitGlibcArenas = kDefaultLimitGlibcArenas;

/// When set to true, config will be reloaded after the current loop

static int signalpipe[2];

/* interface */

static void signal_pipe_desc(std::vector<pollfd> &pdesc) {
	assert(pdesc.size() == 0);
	pdesc.push_back({signalpipe[0],POLLIN,0});
}

static void signal_pipe_serv(const std::vector<pollfd> &pdesc) {
	if ((pdesc[0].revents)&POLLIN) {
		uint8_t sigid;
		if (read(signalpipe[0],&sigid,1)==1) {
			if (sigid == '\001' && gExitingStatus == ExitingStatus::kRunning) {
				safs_pretty_syslog(LOG_NOTICE,"terminate signal received");
				gExitingStatus = ExitingStatus::kWantExit;
			} else if (sigid=='\002') {
				safs_pretty_syslog(LOG_NOTICE,"reloading config files");
				gReloadRequested = true;
			} else if (sigid=='\003') {
				safs_pretty_syslog(LOG_NOTICE, "Received SIGUSR1, killing gently...");
				exit(SAUNAFS_EXIT_STATUS_GENTLY_KILL);
			}
		}
	}
}

bool initialize(const std::vector<RunTab> &tabs) {
	bool isOk = true;

	for (const auto &tab : tabs) {
		eventloop_updatetime();

		try {
			if (tab.function() < 0) {
				safs_pretty_syslog(LOG_ERR, "init: %s failed",
				                   tab.name.c_str());
				isOk = false;
				break;
			}
		} catch (const std::exception &e) {
			safs::log_err("unhandled exception ({}): {}", typeid(e).name(), e.what());
			isOk = false;
			break;
		}
	}

	eventloop_updatetime();

	return isOk;
}

bool initialize_early() {
	return initialize(earlyRunTabs);
}

bool initialize() {
	return initialize(runTabs);
}

bool initialize_late() {
	return initialize(lateRunTabs);
}

const std::string& set_syslog_ident() {
	static std::string logIdent;
	logIdent = cfg_get("SYSLOG_IDENT", std::string(STR(APPNAME)));
	if (logIdent.empty()) {
		logIdent = STR(APPNAME);
	}
	closelog();
	if (gRunAsDaemon) {
		openlog(logIdent.c_str(), LOG_PID | LOG_NDELAY, LOG_DAEMON);
	} else {
#if defined(LOG_PERROR)
		openlog(logIdent.c_str(), LOG_PID | LOG_NDELAY | LOG_PERROR, LOG_USER);
#else
		openlog(logIdent.c_str(), LOG_PID | LOG_NDELAY, LOG_USER);
#endif
	}

	return logIdent;
}

void main_reload() {
	// Reload SYSLOG_IDENT
	safs_pretty_syslog(LOG_NOTICE, "Changing SYSLOG_IDENT to %s",
			cfg_get("SYSLOG_IDENT", STR(APPNAME)).c_str());
	set_syslog_ident();

	// Reload MAGIC_DEBUG_LOG
	safs::setup_logs();
	safs_silent_syslog(LOG_DEBUG, "main.reload");
}

/* signals */

static int termsignal[]={
	SIGTERM,
	-1
};

static int reloadsignal[]={
	SIGHUP,
	-1
};

static int ignoresignal[]={
	SIGQUIT,
#ifdef SIGPIPE
	SIGPIPE,
#endif
#ifdef SIGTSTP
	SIGTSTP,
#endif
#ifdef SIGTTIN
	SIGTTIN,
#endif
#ifdef SIGTTOU
	SIGTTOU,
#endif
#ifdef SIGINFO
	SIGINFO,
#endif
#if defined(SIGUSR1) && !defined(ENABLE_EXIT_ON_USR1)
	SIGUSR1,
#endif
#ifdef SIGUSR2
	SIGUSR2,
#endif
#ifdef SIGCHLD
	SIGCHLD,
#endif
#ifdef SIGCLD
	SIGCLD,
#endif
	-1
};

static int exitsignal[]={
#if defined(SIGUSR1) && defined(ENABLE_EXIT_ON_USR1)
	SIGUSR1,
#endif
	-1
};

static int daemonignoresignal[]={
	SIGINT,
	-1
};

void termhandle(int signo) {
	signo = write(signalpipe[1],"\001",1); // killing two birds with one stone - use signo and do something with value returned by write :)
	(void)signo; // and then use this value to calm down compiler ;)
}

void reloadhandle(int signo) {
	signo = write(signalpipe[1],"\002",1); // see above
	(void)signo;
}

void exithandle(int signo) {
	signo = write(signalpipe[1],"\003",1); // see above
	(void)signo;
}

void set_signal_handlers(int daemonflag) {
	struct sigaction sa;
	uint32_t i;

	zassert(pipe(signalpipe));

#ifdef SA_RESTART
	sa.sa_flags = SA_RESTART;
#else
	sa.sa_flags = 0;
#endif
	sigemptyset(&sa.sa_mask);

	sa.sa_handler = termhandle;
	for (i=0 ; termsignal[i]>0 ; i++) {
		sigaction(termsignal[i],&sa,(struct sigaction *)0);
	}
	sa.sa_handler = reloadhandle;
	for (i=0 ; reloadsignal[i]>0 ; i++) {
		sigaction(reloadsignal[i],&sa,(struct sigaction *)0);
	}
	sa.sa_handler = exithandle;
	for (i=0 ; exitsignal[i]>0 ; i++) {
		sigaction(exitsignal[i],&sa,(struct sigaction *)0);
	}
	sa.sa_handler = SIG_IGN;
	for (i=0 ; ignoresignal[i]>0 ; i++) {
		sigaction(ignoresignal[i],&sa,(struct sigaction *)0);
	}
	sa.sa_handler = daemonflag?SIG_IGN:termhandle;
	for (i=0 ; daemonignoresignal[i]>0 ; i++) {
		sigaction(daemonignoresignal[i],&sa,(struct sigaction *)0);
	}
}

void signal_cleanup(void) {
	close(signalpipe[0]);
	close(signalpipe[1]);
}

void changeugid(RunMode runmode) {
	char pwdgrpbuff[16384];
	struct passwd pwd,*pw;
	struct group grp,*gr;
	char *wuser;
	char *wgroup;
	uid_t wrk_uid;
	gid_t wrk_gid;
	int gidok;

	if (geteuid()==0) {
		wuser = cfg_getstr("WORKING_USER",DEFAULT_USER);
		wgroup = cfg_getstr("WORKING_GROUP",DEFAULT_GROUP);

		gidok = 0;
		wrk_gid = -1;
		if (wgroup[0]=='#') {
			wrk_gid = strtol(wgroup+1,NULL,10);
			gidok = 1;
		} else if (wgroup[0]) {
			getgrnam_r(wgroup,&grp,pwdgrpbuff,16384,&gr);
			if (gr==NULL) {
				safs_pretty_syslog(LOG_WARNING,"%s: no such group",wgroup);
				exit(SAUNAFS_EXIT_STATUS_ERROR);
			} else {
				wrk_gid = gr->gr_gid;
				gidok = 1;
			}
		}

		if (wuser[0]=='#') {
			wrk_uid = strtol(wuser+1,NULL,10);
			if (gidok==0) {
				getpwuid_r(wrk_uid,&pwd,pwdgrpbuff,16384,&pw);
				if (pw==NULL) {
					safs_pretty_syslog(LOG_ERR,"%s: no such user id - can't obtain group id",wuser+1);
					exit(SAUNAFS_EXIT_STATUS_ERROR);
				}
				wrk_gid = pw->pw_gid;
			}
		} else {
			getpwnam_r(wuser,&pwd,pwdgrpbuff,16384,&pw);
			if (pw==NULL) {
				safs_pretty_syslog(LOG_ERR,"%s: no such user",wuser);
				exit(SAUNAFS_EXIT_STATUS_ERROR);
			}
			wrk_uid = pw->pw_uid;
			if (gidok==0) {
				wrk_gid = pw->pw_gid;
			}
		}
		free(wuser);
		free(wgroup);

		if (setgroups(0, NULL) < 0) {
			safs::log_warn("can't reset supplementary groups");
		}
		if (setgid(wrk_gid) < 0) {
			safs::log_err("can't set gid to {}", (int)wrk_gid);
			exit(SAUNAFS_EXIT_STATUS_ERROR);
		} else if ((runmode == RunMode::kStart) || (runmode == RunMode::kRestart)){
			safs::log_info("set gid to {}", (int)wrk_gid);
		}
		if (setuid(wrk_uid)<0) {
			safs::log_err("can't set uid to {}", (int)wrk_uid);
			exit(SAUNAFS_EXIT_STATUS_ERROR);
		} else if ((runmode == RunMode::kStart) || (runmode == RunMode::kRestart)){
			safs::log_info("set uid to {}", (int)wrk_uid);
		}
	}
}

bool open_pam_session() {
#if defined(SAUNAFS_HAVE_PAM)
	static struct pam_conv conv = {misc_conv, NULL};

	assert(gPAMHandle == NULL);

	int retval;

	retval = pam_start("saunafs", DEFAULT_USER, &conv, &gPAMHandle);
	if (retval != PAM_SUCCESS) {
		safs_pretty_errlog(LOG_ERR, "Can't initialize PAM");
		return false;
	}

	retval = pam_open_session(gPAMHandle, 0);
	if (retval != PAM_SUCCESS) {
		safs_pretty_errlog(LOG_ERR, "Can't open PAM session");
		return false;
	}

	return true;
#else
	return false;
#endif
}

void close_pam_session() {
#if defined(SAUNAFS_HAVE_PAM)
	if (!gPAMHandle) {
		return;
	}

	int retval = pam_close_session(gPAMHandle, 0);
	pam_end(gPAMHandle, retval);
#endif
}

class FileLock {
public:
	enum class LockStatus {
		kSuccess = 0,
		kAlive = 1,
		kAgain = 2,
		kTest = 3,
		kFail = -1
	};

	FileLock(RunMode runmode, uint32_t timeout)
			: fd_(),
			  name_("." STR(APPNAME) ".lock"),
			  lockstatus_(LockStatus::kFail),
			  thisProcessCreatedLockFile_(false) {
		while ((lockstatus_ = wdlock(runmode, timeout)) == LockStatus::kAgain) {
		}
	}
	~FileLock() {
		/*
		 * Order of unlock/file deletion matters
		 * for RunMode::kRestart mode.
		 * We need to remove file first and then
		 * unlock file.
		 */
		if (lockstatus_ != LockStatus::kFail && thisProcessCreatedLockFile_) {
			::unlink(name_.c_str());
		}
	}

	LockStatus lockstatus() const {
		return lockstatus_;
	}
	const std::string& name() const {
		return name_;
	}

private:
	LockStatus wdlock(RunMode runmode, uint32_t timeout);
	pid_t mylock();
	bool createLockFile();

	FileDescriptor fd_;
	std::string name_;
	LockStatus lockstatus_;
	bool thisProcessCreatedLockFile_;
};

pid_t FileLock::mylock() {
	struct flock fl;
	fl.l_start = 0;
	fl.l_len = 0;
	fl.l_pid = getpid();
	fl.l_type = F_WRLCK;
	fl.l_whence = SEEK_SET;
	pid_t pid = -1;
	for (;;) {
		if (fcntl(fd_.get(), F_SETLK, &fl) >= 0) { // lock set
			pid = 0;                       // ok
			break;
		}
		if (errno!=EAGAIN) {             // error other than "already locked"
			break;
		}
		if (fcntl(fd_.get(), F_GETLK, &fl) < 0) {  // get lock owner
			break;
		}
		if (fl.l_type!=F_UNLCK) {        // found lock
			pid = fl.l_pid;                // return lock owner
			break;
		}
	}
	return pid;
}

bool FileLock::createLockFile() {
	bool notExisted((::access(name_.c_str(), F_OK) != 0) && (errno == ENOENT));
	fd_.reset(open(name_.c_str(), O_WRONLY | O_CREAT, 0666));
	if (!fd_.isOpened()) {
		std::string err = "can't create lockfile " + fs::getCurrentWorkingDirectoryNoThrow()
		                  + "/" + name();
		safs_pretty_errlog(LOG_ERR, "%s", err.c_str());

		return false;
	}
	thisProcessCreatedLockFile_ = notExisted;
	return true;
}

FileLock::LockStatus FileLock::wdlock(RunMode runmode, uint32_t timeout) {
	std::string lockPath = fs::getCurrentWorkingDirectoryNoThrow() + "/" + name_;
	if(!createLockFile()) {
		return LockStatus::kFail;
	}

	pid_t ownerpid(mylock());
	if (ownerpid<0) {
		safs_pretty_errlog(LOG_ERR, "fcntl error while creating lockfile %s", lockPath.c_str());
		return LockStatus::kFail;
	}

	if (ownerpid>0) {
		if (runmode==RunMode::kIsAlive) {
			return LockStatus::kAlive;
		}
		if (runmode==RunMode::kTest) {
			// TODO(5.0.0): Fix this stupidity in a breaking change by changing it
			// to stdout. It's not that hard, stderr for logs/errors, stdout
			// for output
			fprintf(stderr,STR(APPNAME) " pid: %ld\n",(long)ownerpid);
			return LockStatus::kTest;
		}
		if (runmode==RunMode::kStart) {
			safs_pretty_syslog(LOG_ERR,
					"can't start: lockfile %s is already locked by another process",
					lockPath.c_str());
			return LockStatus::kFail;
		}
		if (runmode==RunMode::kReload) {
			/*
			 * FIXME: business logic should not be in file locking function.
			 */
			if (kill(ownerpid,SIGHUP)<0) {
				safs_pretty_errlog(LOG_ERR,
						"can't send reload signal to the lock owner of %s",
						lockPath.c_str());
				return LockStatus::kFail;
			}
			safs_pretty_syslog(LOG_INFO, "reload signal has been sent");
			return LockStatus::kSuccess;
		}
		if (runmode==RunMode::kKill) {
			/*
			 * FIXME: business logic should not be in file locking function.
			 */
#ifdef ENABLE_EXIT_ON_USR1
			if (kill(ownerpid,SIGUSR1)<0) {
#else
			if (kill(ownerpid,SIGKILL)<0) {
#endif
				safs_pretty_errlog(LOG_ERR,
						"can't kill (SIGKILL) the lock owner (pid:%ld) of %s",
						(long int)ownerpid, lockPath.c_str());
				return LockStatus::kFail;
			}
		} else {
			sassert((runmode == RunMode::kStop) || (runmode == RunMode::kRestart));
			/*
			 * FIXME: business logic should not be in file locking function.
			 */
			if (kill(ownerpid,SIGTERM)<0) {
				safs_pretty_errlog(LOG_ERR,
						"can't kill (SIGTERM) the lock owner (pid:%ld) of %s",
						(long int)ownerpid, lockPath.c_str());
				return LockStatus::kFail;
			}
		}
		fflush(stderr);
		uint32_t l = 0;
		pid_t newownerpid;
		uint32_t checksPerSecond = 10;
		do {
			newownerpid = mylock();
			if (newownerpid<0) {
				safs_pretty_errlog(LOG_ERR,
						"fcntl error while creating lockfile %s",
						lockPath.c_str());
				return LockStatus::kFail;
			}
			if (newownerpid>0) {
				l++;
				uint32_t secondsElapsed = l / checksPerSecond;
				if (secondsElapsed >= timeout) {
					safs_pretty_syslog(LOG_ERR,
							"about %" PRIu32 " seconds passed and the lock %s still exists - giving up",
							secondsElapsed, lockPath.c_str());
					return LockStatus::kFail;
				}
				if (l % (10 * checksPerSecond) == 0) {
					safs_pretty_syslog(LOG_WARNING,
							"about %" PRIu32 " seconds passed and the lock %s still exists...",
							secondsElapsed, lockPath.c_str());
					fflush(stderr);
				}
				if (newownerpid!=ownerpid) {
					safs_silent_syslog(LOG_INFO, "new lock owner of %s detected", lockPath.c_str());
					if (runmode==RunMode::kKill) {
						if (kill(newownerpid,SIGKILL)<0) {
							safs_pretty_errlog(LOG_ERR,
									"can't kill (SIGKILL) the lock owner (pid:%ld) of %s",
									(long int)newownerpid, lockPath.c_str());
							return LockStatus::kFail;
						}
					} else {
						sassert((runmode == RunMode::kStop) || (runmode == RunMode::kRestart));
						if (kill(newownerpid,SIGTERM)<0) {
							safs_pretty_errlog(LOG_ERR,
									"can't kill (SIGTERM) lock owner (pid:%ld) of %s",
									(long int)newownerpid, lockPath.c_str());
							return LockStatus::kFail;
						}
					}
					ownerpid = newownerpid;
				}
			}
			usleep(1000000 / checksPerSecond);
		} while (newownerpid!=0);
		return (runmode == RunMode::kRestart) ? LockStatus::kAgain : LockStatus::kSuccess;
	}
	if (runmode==RunMode::kStart || runmode==RunMode::kRestart) {
		safs_pretty_syslog(LOG_INFO,"lockfile %s created and locked", lockPath.c_str());
	} else if (runmode==RunMode::kStop || runmode==RunMode::kKill) {
		safs_pretty_syslog(LOG_WARNING,"can't find process to terminate");
		return LockStatus::kSuccess;
	} else if (runmode==RunMode::kReload) {
		safs_pretty_syslog(LOG_ERR,"can't find process to send reload signal");
		return LockStatus::kFail;
	} else if (runmode==RunMode::kTest) {
		fprintf(stderr, STR(APPNAME) " is not running\n");
	} else if (runmode==RunMode::kIsAlive) {
		return LockStatus::kSuccess;
	}
	return LockStatus::kSuccess;
}

void makedaemon() {
	int f;
	uint8_t pipebuff[1000];
	ssize_t r;
	size_t happy;
	int piped[2];

	fflush(stdout);
	fflush(stderr);
	if (pipe(piped)<0) {
		safs::log_err("pipe error: {}", strerr(errno));
		exit(SAUNAFS_EXIT_STATUS_ERROR);
	}
	f = fork();
	if (f<0) {
		safs::log_err("pipe error: {}", strerr(errno));
		exit(SAUNAFS_EXIT_STATUS_ERROR);
	}
	if (f>0) {
		wait(&f);       // just get child status - prevents child from being zombie during initialization stage
		if (f) {
			safs_pretty_syslog(LOG_ERR, "child status: %d",f);
			safs::log_err("{}", strerr(errno));
			exit(SAUNAFS_EXIT_STATUS_ERROR);
		}
		close(piped[1]);
		while ((r=read(piped[0],pipebuff,1000))) {
			if (r>0) {
				if (pipebuff[r-1]==0) { // zero as a last char in the pipe means error
					if (r>1) {
						happy = fwrite(pipebuff,1,r-1,stderr);
						(void)happy;
					}
					safs::log_err("pipe error: {}", strerr(errno));
					exit(SAUNAFS_EXIT_STATUS_ERROR);
				}
				happy = fwrite(pipebuff,1,r,stderr);
				(void)happy;
			} else {
				safs::log_err("pipe error: {}", strerr(errno));
				exit(SAUNAFS_EXIT_STATUS_ERROR);
			}
		}
		exit(SAUNAFS_EXIT_STATUS_SUCCESS);
	}
	setsid();
	setpgid(0,getpid());
	f = fork();
	if (f<0) {
		safs::log_err("second fork error: {}", strerr(errno));
		if (write(piped[1],"fork error\n",11)!=11) {
			safs::log_err("pipe error: {}", strerr(errno));
		}
		close(piped[1]);
		exit(SAUNAFS_EXIT_STATUS_ERROR);
	}
	if (f>0) {
		exit(SAUNAFS_EXIT_STATUS_SUCCESS);
	}
	set_signal_handlers(1);

	close(STDIN_FILENO);
	sassert(open("/dev/null", O_RDWR, 0)==STDIN_FILENO);
	close(STDOUT_FILENO);
	sassert(dup(STDIN_FILENO)==STDOUT_FILENO);
	close(STDERR_FILENO);
	sassert(dup(piped[1])==STDERR_FILENO);
	close(piped[1]);

	// close all inherited file descriptors
	int open_max = sysconf(_SC_OPEN_MAX);
	for (int i = 3; i < open_max; i++) {
		if (i == signalpipe[0] || i == signalpipe[1]) {
			continue;
		}
		close(i);
	}
}

void close_msg_channel() {
	fflush(stderr);
	close(STDERR_FILENO);
	sassert(open("/dev/null", O_RDWR, 0)==STDERR_FILENO);
}

void createpath(const char *filename) {
	char pathbuff[1024];
	const char *src = filename;
	char *dst = pathbuff;
	if (*src=='/') *dst++=*src++;

	while (*src) {
		while (*src!='/' && *src) {
			*dst++=*src++;
		}
		if (*src=='/') {
			*dst='\0';
			if (mkdir(pathbuff,(mode_t)0777)<0) {
				if (errno!=EEXIST) {
					safs_pretty_errlog(LOG_NOTICE,"creating directory %s",pathbuff);
				}
			} else {
				safs_pretty_syslog(LOG_NOTICE,"directory %s has been created",pathbuff);
			}
			*dst++=*src++;
		}
	}
}

void usage(const char *appname) {
	printf(
"usage: %s [-vdu] [-t locktimeout] [-c cfgfile] [start|stop|restart|reload|test|isalive]\n"
"\n"
"-v[|V] : print version number and exit\n"
"-d : run in foreground\n"
"-u : log undefined config variables\n"
"-t locktimeout : how long wait for lockfile\n"
"-c cfgfile : use given config file\n"
"-p pidfile : write pid to given file\n"
"-o extra_option : module specific extra option\n"
	,appname);
	exit(SAUNAFS_EXIT_STATUS_ERROR);
}

void makePidFile(const std::string &name) {
	if (name.empty()) {
		return;
	}

	std::ofstream ofs(name, std::ios_base::out | std::ios_base::trunc);
	if (ofs.fail()) {
		safs_pretty_syslog(LOG_WARNING, "failed to create pid file: %s",
				name.c_str());
		return;
	}
	ofs << std::to_string(getpid()) << std::endl;
}

/// (glibc specific) Tune glibc malloc arenas to avoid high virtual memory usage
inline void tuneMalloc() {
#if defined(SAUNAFS_HAVE_MALLOPT) && defined(M_ARENA_MAX) && \
    defined(M_ARENA_TEST)
	gLimitGlibcArenas =
	    cfg_getuint8("LIMIT_GLIBC_MALLOC_ARENAS", kDefaultLimitGlibcArenas);

	if (gLimitGlibcArenas > 0) {
		if (::getenv("MALLOC_ARENA_MAX") == kEnvironmentVariableNotDefined) {
			safs::log_info("Setting glibc malloc arena max to {}",
			               gLimitGlibcArenas);
			::mallopt(M_ARENA_MAX, static_cast<int>(gLimitGlibcArenas));
		}
		if (::getenv("MALLOC_ARENA_TEST") == kEnvironmentVariableNotDefined) {
			safs::log_info("Setting glibc malloc arena test to {}",
			               gLimitGlibcArenas);
			::mallopt(M_ARENA_TEST, static_cast<int>(gLimitGlibcArenas));
		}
	}
#endif
}

int main(int argc,char **argv) {
	char *wrkdir;
	char *appname;
	int ch;
	int logundefined;
	int lockmemory;
	int32_t nicelevel;
	uint32_t locktimeout;
	struct rlimit rls;
	std::string default_cfgfile = ETC_PATH "/" STR(APPNAME) ".cfg";
	std::string cfgfile = default_cfgfile;
	std::string pidfile;

	prepareEnvironment();
	mycrc32_init();

	locktimeout = 1800;
	RunMode runmode = RunMode::kRestart;
	logundefined = 0;
	lockmemory = 0;
	appname = argv[0];

	while ((ch = getopt(argc, argv, "o:c:p:dht:uvVx?")) != -1) {
		switch(ch) {
			case 'v':
			case 'V':
				std::cout << common::version() << '\n';
				return 0;
			case 'd':
				gRunAsDaemon = false;
				break;
			case 't':
				locktimeout=strtoul(optarg,NULL,10);
				break;
			case 'o':
				gExtraArguments.emplace_back(optarg);
				break;
			case 'c':
				cfgfile = optarg;
				break;
			case 'p':
				pidfile = optarg;
				break;
			case 'u':
				logundefined=1;
				break;
			case 'x':
				++gVerbosity;
				break;
			default:
				usage(appname);
				return SAUNAFS_EXIT_STATUS_ERROR;
		}
	}
	argc -= optind;
	argv += optind;
	if (argc==1) {
		if (strcasecmp(argv[0],"start")==0) {
			runmode = RunMode::kStart;
		} else if (strcasecmp(argv[0],"stop")==0) {
			runmode = RunMode::kStop;
		} else if (strcasecmp(argv[0],"restart")==0) {
			runmode = RunMode::kRestart;
		} else if (strcasecmp(argv[0],"reload")==0) {
			runmode = RunMode::kReload;
		} else if (strcasecmp(argv[0],"test")==0) {
			runmode = RunMode::kTest;
		} else if (strcasecmp(argv[0],"kill")==0) {
			runmode = RunMode::kKill;
		} else if (strcasecmp(argv[0],"isalive")==0) {
			runmode = RunMode::kIsAlive;
		} else {
			usage(appname);
			return SAUNAFS_EXIT_STATUS_ERROR;
		}
	} else if (argc!=0) {
		usage(appname);
		return SAUNAFS_EXIT_STATUS_ERROR;
	}

	if (gRunAsDaemon) {
		safs::add_log_stderr(safs::log_level::warn);
	} else {
		safs::add_log_stderr(safs::log_level::debug);
	}
	if (runmode==RunMode::kStart || runmode==RunMode::kRestart) {
		if (gRunAsDaemon) {
			makedaemon();
		} else {
			set_signal_handlers(0);
		}
		makePidFile(pidfile);
	}

	ch = cfg_load(cfgfile.c_str(), logundefined);
	if (ch == 1) {
		safs_pretty_syslog(LOG_WARNING,
				"configuration file %s not found - using "
				"defaults; please create one to remove this "
				"warning (you can copy sample configuration "
				"from '" APP_EXAMPLES_SUBDIR "/" STR(CFGNAME) ".cfg' to get a base "
				"configuration)",
				cfgfile.c_str());
	} else if (runmode==RunMode::kStart || runmode==RunMode::kRestart) {
		// Setup logs before first log
		safs::setup_logs();
		safs::log_info("Configuration file {} loaded", cfgfile);
	}


	if (runmode==RunMode::kStart || runmode==RunMode::kRestart) {
		if (!open_pam_session()) {
			rls.rlim_cur = SAUNAFS_MAX_FILES;
			rls.rlim_max = SAUNAFS_MAX_FILES;
			if (setrlimit(RLIMIT_NOFILE, &rls) < 0) {
				safs_pretty_syslog(
				    LOG_WARNING,
				    "can't change open files limit to %u, check restrictions with `ulimit -n'",
				    SAUNAFS_MAX_FILES);
			} else {
				safs_pretty_syslog(
				    LOG_INFO, "failed to use system open files limit, changed to default value %u",
				    SAUNAFS_MAX_FILES);
			}
		}

		lockmemory = cfg_getnum("LOCK_MEMORY",0);
#ifdef SFS_USE_MEMLOCK
		if (lockmemory) {
			rls.rlim_cur = RLIM_INFINITY;
			rls.rlim_max = RLIM_INFINITY;
			setrlimit(RLIMIT_MEMLOCK,&rls);
		}
#endif
		nicelevel = cfg_getint32("NICE_LEVEL",-19);
		setpriority(PRIO_PROCESS,getpid(),nicelevel);
	}

	changeugid(runmode);

	wrkdir = cfg_getstr("DATA_PATH",DATA_PATH);


	if (chdir(wrkdir)<0) {
		safs::log_err("can't set working directory to {}: {}", wrkdir, strerr(errno));
		if (gRunAsDaemon) {
			fputc(0,stderr);
			close_msg_channel();
		}
		closelog();
		return SAUNAFS_EXIT_STATUS_ERROR;
	} else {
		if (runmode==RunMode::kStart || runmode==RunMode::kRestart) {
			safs::log_info("changed working directory to: {}", wrkdir);
		}
	}
	free(wrkdir);

	umask(cfg_getuint32("FILE_UMASK",027)&077);

	eventloop_pollregister(signal_pipe_desc, signal_pipe_serv);

	if (!initialize_early()) {
		safs::log_err("couldn't initialize early functions");
		if (gRunAsDaemon) {
			fputc(0, stderr);
			close_msg_channel();
		}
		closelog();
		return SAUNAFS_EXIT_STATUS_ERROR;
	}

	// Only kStart should check for lock file consistency
	FileLock fl(runmode, locktimeout);
	if (fl.lockstatus() == FileLock::LockStatus::kFail) {
		safs::log_err("couldn't acquire lock on {}", fl.name());
		if (gRunAsDaemon) {
			fputc(0,stderr);
			close_msg_channel();
		}
		closelog();
		return SAUNAFS_EXIT_STATUS_ERROR;
	}

	if (fl.lockstatus() == FileLock::LockStatus::kTest) {
		if (gRunAsDaemon) {
			fputc(0,stderr);
			close_msg_channel();
		}
		closelog();
		return SAUNAFS_STATUS_OK;
	}

	if (runmode==RunMode::kStop || runmode==RunMode::kKill || runmode==RunMode::kReload
			|| runmode==RunMode::kIsAlive) {
		if (gRunAsDaemon) {
			close_msg_channel();
		}
		closelog();
		if (runmode==RunMode::kIsAlive) {
			FileLock::LockStatus lockstatus = fl.lockstatus();
			sassert((lockstatus == FileLock::LockStatus::kSuccess)
					|| (lockstatus == FileLock::LockStatus::kAlive));
			return (lockstatus == FileLock::LockStatus::kAlive
					? SAUNAFS_EXIT_STATUS_SUCCESS : SAUNAFS_EXIT_STATUS_NOT_ALIVE);
		} else {
			return SAUNAFS_EXIT_STATUS_SUCCESS;
		}
	}


#ifdef SFS_USE_MEMLOCK
	if (lockmemory) {
		if (getrlimit(RLIMIT_MEMLOCK,&rls)<0) {
			safs_pretty_errlog(LOG_WARNING,"error getting memory lock limits");
		} else {
			if (rls.rlim_cur!=RLIM_INFINITY && rls.rlim_max==RLIM_INFINITY) {
				rls.rlim_cur = RLIM_INFINITY;
				rls.rlim_max = RLIM_INFINITY;
				if (setrlimit(RLIMIT_MEMLOCK,&rls)<0) {
					safs_pretty_errlog(LOG_WARNING,"error setting memory lock limit to unlimited");
				}
			}
			if (getrlimit(RLIMIT_MEMLOCK,&rls)<0) {
				safs_pretty_errlog(LOG_WARNING,"error getting memory lock limits");
			} else {
				if (rls.rlim_cur!=RLIM_INFINITY) {
					safs_pretty_errlog(LOG_WARNING,"can't set memory lock limit to unlimited");
				} else {
					if (mlockall(MCL_CURRENT|MCL_FUTURE)<0) {
						safs_pretty_errlog(LOG_WARNING,"memory lock error");
					} else {
						safs_pretty_syslog(LOG_NOTICE,"process memory was successfully locked in RAM");
					}
				}
			}
		}
	}
#else
	if (lockmemory) {
		safs_pretty_syslog(LOG_WARNING,"trying to lock memory, but memory lock not supported");
	}
#endif

	tuneMalloc();

	if (initialize()) {
		if (getrlimit(RLIMIT_NOFILE,&rls)==0) {
			safs_pretty_syslog(LOG_NOTICE,"open files limit: %lu",(unsigned long)(rls.rlim_cur));
		}
		safs_pretty_syslog(LOG_NOTICE, STR(APPNAME)" daemon initialized properly");
#ifdef SAUNAFS_HAVE_SYSTEMD_SD_DAEMON_H
		sd_notify(0, "READY=1\nSTATUS=" STR(APPNAME) " daemon initialized properly.\n");
#endif
		set_syslog_ident();
		if (gRunAsDaemon) {
			close_msg_channel();
		}
		if (initialize_late()) {
			eventloop_reloadregister(main_reload); // this will be the first thing to do
			eventloop_run();
			ch=SAUNAFS_EXIT_STATUS_SUCCESS;
		} else {
			ch=SAUNAFS_EXIT_STATUS_ERROR;
		}
	} else {
		safs::log_err("couldn't initialize functions");
		if (gRunAsDaemon) {
			fputc(0,stderr);
			close_msg_channel();
		}
		ch=SAUNAFS_EXIT_STATUS_ERROR;
	}
	eventloop_destruct();
	eventloop_release_resources();
	signal_cleanup();
	cfg_term();
	closelog();
	close_pam_session();
	return ch;
}
