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

#pragma once

#include "common/platform.h"

#ifdef _WIN32
#include <windows.h>

// Define sigset_t and related functions for Windows
typedef unsigned long sigset_t;

inline int sigemptyset(sigset_t *set) {
	*set = 0;
	return 0;
}

inline int sigaddset(sigset_t *set, int signum) {
	*set |= (1 << (signum - 1));
	return 0;
}

inline int sigprocmask(int how, const sigset_t *set, sigset_t *oldset) {
	static sigset_t current_mask = 0;

	if (oldset) { *oldset = current_mask; }

	if (set) {
		switch (how) {
		case SIG_BLOCK:
			current_mask |= *set;
			break;
		case SIG_UNBLOCK:
			current_mask &= ~(*set);
			break;
		case SIG_SETMASK:
			current_mask = *set;
			break;
		default:
			return -1;
		}
	}

	return 0;
}

// Define kill and getpid for Windows
inline int kill(int pid, int sig) {
	// This is a simplified version and does not actually send signals
	// You can implement custom behavior here if needed
	HANDLE hProcess = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
	if (hProcess == NULL) { return -1; }
	BOOL result = TerminateProcess(hProcess, sig);
	CloseHandle(hProcess);
	return result ? 0 : -1;
}

// Define signal constants for Windows
#define SIGINT 2
#define SIGTERM 15
#define SIGHUP 1
#define SIGUSR1 10

#else
#include <signal.h>
#endif
