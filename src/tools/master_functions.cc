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

#include "common/platform.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#endif

#include "common/datapack.h"
#include "common/serialization.h"
#include "common/server_connection.h"	
#include "common/special_inode_defs.h"
#include "common/sockets.h"
#include "common/stat_defs.h"
#include "errors/sfserr.h"
#include "protocol/cltoma.h"
#include "protocol/matocl.h"
#include "tools/tools_common_functions.h"

struct master_info_t {
	uint32_t ip;
	uint16_t port;
	uint32_t cuid;
	uint32_t version;
};

static thread_local int gCurrentMaster = -1;

static int master_register(int rfd, uint32_t cuid) {
	uint32_t i;
	const uint8_t *rptr;
	uint8_t *wptr, regbuff[8 + 73];

	wptr = regbuff;
	put32bit(&wptr, CLTOMA_FUSE_REGISTER);
	put32bit(&wptr, 73);
	memcpy(wptr, FUSE_REGISTER_BLOB_ACL, 64);
	wptr += 64;
	put8bit(&wptr, REGISTER_TOOLS);
	put32bit(&wptr, cuid);
	put16bit(&wptr, SAUNAFS_PACKAGE_VERSION_MAJOR);
	put8bit(&wptr, SAUNAFS_PACKAGE_VERSION_MINOR);
	put8bit(&wptr, SAUNAFS_PACKAGE_VERSION_MICRO);
	if (tcpwrite(rfd, regbuff, 8 + 73) != 8 + 73) {
		printf("register to master: send error\n");
		return -1;
	}
	if (tcpread(rfd, regbuff, 9) != 9) {
		printf("register to master: receive error\n");
		return -1;
	}
	rptr = regbuff;
	i = get32bit(&rptr);
	if (i != MATOCL_FUSE_REGISTER) {
		printf("register to master: wrong answer (type)\n");
		return -1;
	}
	i = get32bit(&rptr);
	if (i != 1) {
		printf("register to master: wrong answer (length)\n");
		return -1;
	}
	if (*rptr) {
		printf("register to master: %s\n", saunafs_error_string(*rptr));
		return -1;
	}
	return 0;
}

static int master_connect(const master_info_t *info) {
	for(int cnt = 0; cnt < 10; ++cnt) {
		int sd = tcpsocket();
		if (sd < 0) {
			return -1;
		}
		int timeout = (cnt % 2) ? (300 * (1 << (cnt >> 1))) : (200 * (1 << (cnt >> 1)));
		if (tcpnumtoconnect(sd, info->ip, info->port, timeout) >= 0) {
			return sd;
		}
		tcpclose(sd);
	}
	return -1;
}

static bool contains_master_info_name_end(const char *name) {
	auto end = name + strlen(name) - 1;
	std::string extracted_name;

	for (; end >= name && *end != '/' && *end != '\\'; --end) {
		extracted_name = *end + extracted_name;
	}

	return (strcmp(extracted_name.c_str(), SPECIAL_FILE_NAME_MASTERINFO) == 0);
}

static int read_master_info(const char *name, master_info_t *info) {
	static constexpr int kMasterInfoSize = 14;
	uint8_t buffer[kMasterInfoSize];
	struct stat stb;
	int sd;

	if (stat(name, &stb) < 0) {
		return -1;
	}

	if ((stb.st_ino != SPECIAL_INODE_MASTERINFO &&
	     !contains_master_info_name_end(name)) ||
	    stb.st_nlink != 1 || stb.st_uid != 0 || stb.st_gid != 0 ||
	    stb.st_size != kMasterInfoSize) {
		return -1;
	}

	sd = open(name, O_RDONLY);
	if (sd < 0) {
		return -2;
	}

	if (read(sd, buffer, kMasterInfoSize) != kMasterInfoSize) {
		close(sd);
		return -2;
	}

	close(sd);

	deserialize(buffer, kMasterInfoSize, info->ip, info->port, info->cuid, info->version);

	return 0;
}

#ifdef _WIN32
int get_inode_by_path(int sd, std::string path, uint32_t &inode) {
	try {
		uint32_t messageId = 0;
		MessageBuffer request;
		cltoma::inodeByPath::serialize(request, messageId, path, 0, 0);
		MessageBuffer response = ServerConnection::sendAndReceive(
		    sd, request, SAU_MATOCL_INODE_BY_PATH);
		PacketVersion version;
		deserializePacketVersionNoHeader(response, version);
		if (version == matocl::inodeByPath::kStatusPacketVersion) {
			uint8_t status;
			matocl::inodeByPath::deserialize(response, messageId, status);
			throw Exception(std::string(path) + ": failed", status);
		}
		matocl::inodeByPath::deserialize(response, messageId, inode);
	} catch (Exception &e) {
		fprintf(stderr, "%s\n", e.what());
		close_master_conn(1);
		return -1;
	}
	close_master_conn(0);
	return 0;
}
#endif

int open_master_conn(const char *name, uint32_t *inode, mode_t *mode,
                     [[maybe_unused]] bool needrwfs) {
	char rpath[PATH_MAX + 1];
	struct stat stb;
	[[maybe_unused]] struct statvfs stvfsb;
	master_info_t master_info;

	rpath[0] = 0;
#ifdef _WIN32
	std::string name_to_use = std::string(name);
	if (name[strlen(name) - 1] == '\\' || name[strlen(name) - 1] == '/') {
		name_to_use = std::string(name).substr(0, strlen(name) - 1);
	}
	if (GetFullPathName(name_to_use.c_str(), PATH_MAX, rpath, NULL) == 0) {
		printf("%s: GetFullPathName error: %lu\n", name, GetLastError());
		return -1;
	}
#else
	if (realpath(name, rpath) == NULL) {
		printf("%s: realpath error on (%s): %s\n", name, rpath,
		       strerror(errno));
		return -1;
	}
	if (needrwfs) {
		if (statvfs(rpath, &stvfsb) != 0) {
			printf("%s: (%s) statvfs error: %s\n", name, rpath, strerr(errno));
			return -1;
		}
		if (stvfsb.f_flag & ST_RDONLY) {
			printf("%s: (%s) Read-only file system\n", name, rpath);
			return -1;
		}
	}
#endif
	if (stat(rpath, &stb) != 0) {
		printf("%s: (%s) stat error: %s\n", name, rpath, strerr(errno));
		return -1;
	}
	*inode = stb.st_ino;
#ifdef _WIN32
	std::string lookup_rpath;
	if (strlen(rpath) > 3) {
		lookup_rpath = std::string(rpath).substr(2);
		std::replace(lookup_rpath.begin(), lookup_rpath.end(), '\\', '/');
	} else {
		lookup_rpath = "/";
	}
#endif
	if (mode) {
		*mode = stb.st_mode;
	}
	if (gCurrentMaster >= 0) {
		close(gCurrentMaster);
		gCurrentMaster = -1;
	}

	for (;;) {
		uint32_t rpath_inode;

		if (stat(rpath, &stb) != 0) {
			printf("%s: (%s) stat error: %s\n", name, rpath, strerr(errno));
			return -1;
		}
		rpath_inode = stb.st_ino;

		size_t rpath_len = strlen(rpath);
		if (rpath_len + sizeof("/" SPECIAL_FILE_NAME_MASTERINFO) > PATH_MAX) {
			printf("%s: path too long\n", name);
			return -1;
		}

		if (rpath_len == 4 && rpath[2] == '\\' && rpath[3] == '.') {
			strcpy(rpath + rpath_len - 1, SPECIAL_FILE_NAME_MASTERINFO);
		} else if (rpath_len == 3 && rpath[2] == '\\') {
			strcpy(rpath + rpath_len, SPECIAL_FILE_NAME_MASTERINFO);
		} else {
			strcpy(rpath + rpath_len, "/" SPECIAL_FILE_NAME_MASTERINFO);
		}

		int r = read_master_info(rpath, &master_info);
		if (r == -2) {
			printf("%s: can't read '" SPECIAL_FILE_NAME_MASTERINFO "'\n", name);
			return -1;
		}

		if (r == 0) {
			if (master_info.ip == 0 || master_info.port == 0 ||
			    master_info.cuid == 0) {
				printf("%s: incorrect '" SPECIAL_FILE_NAME_MASTERINFO "'\n",
				       name);
				return -1;
			}

			if (rpath_inode == *inode) {
				*inode = SPECIAL_INODE_ROOT;
			}

			int sd = master_connect(&master_info);
			if (sd < 0) {
				printf("%s: can't connect to master (" SPECIAL_FILE_NAME_MASTERINFO "): %s\n", name,
				       strerr(errno));
				return -1;
			}

			if (master_register(sd, master_info.cuid) < 0) {
				printf("%s: can't register to master (" SPECIAL_FILE_NAME_MASTERINFO ")\n", name);
				tcpclose(sd);
				return -1;
			}

#ifdef _WIN32
			if (lookup_rpath == "/") {
				*inode = SPECIAL_INODE_ROOT;
			} else {
				auto err = get_inode_by_path(sd, lookup_rpath, *inode);
				if (err != SAUNAFS_STATUS_OK) {
					printf("%s: can't get inode from path\n", name);
					tcpclose(sd);
					return -1;
				}
			}
#endif

			gCurrentMaster = sd;
			return sd;
		}

		// remove .masterinfo from end of string
		rpath[rpath_len] = 0;

#ifdef _WIN32
		if (strlen(rpath) < 3 || !std::isalpha(rpath[0]) || rpath[1] != ':' ||
		    rpath[2] != '\\') {
			printf("%s: not SaunaFS object\n", name);
			return -1;
		}
#else
		if (rpath[0] != '/' || rpath[1] == '\0') {
			printf("%s: not SaunaFS object\n", name);
			return -1;
		}
#endif
		dirname_inplace(rpath);
		if (stat(rpath, &stb) != 0) {
			printf("%s: (%s) stat error: %s\n", name, rpath, strerr(errno));
			return -1;
		}
	}
	return -1;
}

void close_master_conn(int err) {
	if (gCurrentMaster < 0) {
		return;
	}
	if (err) {
		close(gCurrentMaster);
		gCurrentMaster = -1;
	}
}

void force_master_conn_close() {
	if (gCurrentMaster < 0) {
		return;
	}
	close(gCurrentMaster);
	gCurrentMaster = -1;
}
