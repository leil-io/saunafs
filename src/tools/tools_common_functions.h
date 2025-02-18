/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA
   Copyright 2013-2014 EditShare
   Copyright 2013-2017 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ


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

#include <functional>
#include <unistd.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

#include "common/sockets.h"
#include "protocol/SFSCommunication.h"

#define tcpread(s, b, l) tcptoread(s, b, l, 10000)
#define tcpwrite(s, b, l) tcptowrite(s, b, l, 10000)

extern uint8_t humode;

extern const char *eattrtab[EATTR_BITS];
extern const char *eattrdesc[EATTR_BITS];

bool check_usage(std::function<void()> f, bool expressionExpectedToBeFalse, const char *format, ...);

void set_humode();

void print_number(const char *prefix, const char *suffix, uint64_t number, uint8_t mode32,
				  uint8_t bytesflag, uint8_t dflag);
int my_get_number(const char *str, uint64_t *ret, uint64_t max, uint8_t bytesflag);

int bsd_basename(const char *path, char *bname);
int bsd_dirname(const char *path, char *bname);
void dirname_inplace(char *path);

int open_master_conn(const char *name, uint32_t *inode, mode_t *mode, bool needrwfs);
void close_master_conn(int err);
void force_master_conn_close();

void signalHandler(uint32_t job_id);

inline void print_numberformat_options() {
	fprintf(stderr, " -n - show numbers in plain format\n");
	fprintf(stderr, " -h - \"human-readable\" numbers using base 2 prefixes (IEC 60027)\n");
	fprintf(stderr, " -H - \"human-readable\" numbers using base 10 prefixes (SI)\n");
}

inline void print_recursive_option() {
	fprintf(stderr, " -r - do it recursively\n");
}

inline void print_extra_attributes() {
	int j;
	fprintf(stderr, "\nattributes:\n");
	for (j = 0; j < EATTR_BITS; j++) {
		if (eattrtab[j][0]) {
			fprintf(stderr, " %s - %s\n", eattrtab[j], eattrdesc[j]);
		}
	}
}

inline bool get_full_path(const char *file_name, char *path_buf) {
#ifdef _WIN32
	// Use GetFullPathName on Windows
	return GetFullPathName(file_name, PATH_MAX, path_buf, NULL) != 0;
#else
	// Use realpath on Unix-like systems
	return realpath(file_name, path_buf) != nullptr;
#endif
}
