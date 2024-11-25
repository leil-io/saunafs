/*

   Copyright 2024 Leil Storage OÜ

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

#include "metrics/master.h"
#include "metrics/utils.h"

#ifdef HAVE_PROMETHEUS
using namespace metrics::master;

Master::Master(std::shared_ptr<prometheus::Registry> &registry) {
	// clang-format off
	// NOLINTBEGIN(cppcoreguidelines-prefer-member-initializer)
	packetClientCounter = &setupFamily(
		"metadata_observed_packets_client_total",
		"Number of observed packets from and for client", registry);
	byteClientCounter = &setupFamily(
		"metadata_observed_bytes_client_total",
		"Number of observed bytes from and for client", registry);
	filesystemCounter = &setupFamily(
		"metadata_stats_total",
		"Number of observed filesystem operations", registry);
	chunkCounter= &setupFamily(
		"metadata_chunk_operations_total",
		"Number of chunk operations", registry);
	// NOLINTEND(cppcoreguidelines-prefer-member-initializer)

	// A very hacky way to allow compile time checking if metrics have been
	// set. Any enum value not used will throw a compile time error.
	master::Counters start = KEY_START;
	switch (start) {
		case KEY_START:
			[[fallthrough]];
		case CHUNK_DELETE:
			masterCounters[CHUNK_DELETE] =
				Counter(
					{{"chunk", "operations"}, {"operation", "delete"}},
					chunkCounter);
			[[fallthrough]];
		case CHUNK_REPLICATE:
			masterCounters[CHUNK_REPLICATE] =
				Counter(
					{{"chunk", "operations"}, {"operation", "replicate"}},
					chunkCounter);
			[[fallthrough]];
		case FS_STATFS:
			masterCounters[FS_STATFS] =
				Counter(
					{{"filesystem", "operations"}, {"operation", "STATFS"}},
					filesystemCounter);
			[[fallthrough]];
		case FS_GETATTR:
			masterCounters[FS_GETATTR] =
				Counter(
					{{"filesystem", "operations"}, {"operation", "GETATTR"}},
					filesystemCounter);
			[[fallthrough]];
		case FS_SETATTR:
			masterCounters[FS_SETATTR] =
				Counter(
					{{"filesystem", "operations"}, {"operation", "SETATTR"}},
					filesystemCounter);
			[[fallthrough]];
		case FS_LOOKUP:
			masterCounters[FS_LOOKUP] =
				Counter(
					{{"filesystem", "operations"}, {"operation", "LOOKUP"}},
					filesystemCounter);
			[[fallthrough]];
		case FS_MKDIR:
			masterCounters[FS_MKDIR] =
				Counter(
					{{"filesystem", "operations"}, {"operation", "MKDIR"}},
					filesystemCounter);
			[[fallthrough]];
		case FS_RMDIR:
			masterCounters[FS_RMDIR] =
				Counter(
					{{"filesystem", "operations"}, {"operation", "RMDIR"}},
					filesystemCounter);
			[[fallthrough]];
		case FS_SYMLINK:
			masterCounters[FS_SYMLINK] =
				Counter(
					{{"filesystem", "operations"}, {"operation", "SYMLINK"}},
					filesystemCounter);
			[[fallthrough]];
		case FS_READLINK:
			masterCounters[FS_READLINK] =
				Counter(
					{{"filesystem", "operations"}, {"operation", "READLINK"}},
					filesystemCounter);
			[[fallthrough]];
		case FS_MKNOD:
			masterCounters[FS_MKNOD] =
				Counter(
					{{"filesystem", "operations"}, {"operation", "MKNOD"}},
					filesystemCounter);
			[[fallthrough]];
		case FS_UNLINK:
			masterCounters[FS_UNLINK] =
				Counter(
					{{"filesystem", "operations"}, {"operation", "UNLINK"}},
					filesystemCounter);
			[[fallthrough]];
		case FS_RENAME:
			masterCounters[FS_RENAME] =
				Counter(
					{{"filesystem", "operations"}, {"operation", "RENAME"}},
					filesystemCounter);
			[[fallthrough]];
		case FS_LINK:
			masterCounters[FS_LINK] =
				Counter(
					{{"filesystem", "operations"}, {"operation", "LINK"}},
					filesystemCounter);
			[[fallthrough]];
		case FS_READDIR:
			masterCounters[FS_READDIR] = Counter(
				{{"filesystem", "operations"}, {"operation", "READDIR"}},
				filesystemCounter);
			[[fallthrough]];
		case FS_OPEN:
			masterCounters[FS_OPEN] =
				Counter(
					{{"filesystem", "operations"}, {"operation", "OPEN"}},
					filesystemCounter);
			[[fallthrough]];
		case FS_READ:
			masterCounters[FS_READ] =
				Counter(
					{{"filesystem", "operations"}, {"operation", "READ"}},
					filesystemCounter);
			[[fallthrough]];
		case FS_WRITE:
			masterCounters[FS_WRITE] =
				Counter(
					{{"filesystem", "operations"}, {"operation", "WRITE"}},
					filesystemCounter);
			[[fallthrough]];
		case CLIENT_RX_PACKETS:
			masterCounters[CLIENT_RX_PACKETS] =
				Counter(
					{{"protocol", "tcp"}, {"direction", "rx"}},
					packetClientCounter);
			[[fallthrough]];
		case CLIENT_TX_PACKETS:
			masterCounters[CLIENT_TX_PACKETS] =
				Counter(
					{{"protocol", "tcp"}, {"direction", "tx"}},
					packetClientCounter);
			[[fallthrough]];
		case CLIENT_RX_BYTES:
			masterCounters[CLIENT_RX_BYTES] =
				Counter(
					{{"protocol", "tcp"}, {"direction", "rx"}},
					byteClientCounter);
			[[fallthrough]];
		case CLIENT_TX_BYTES:
			masterCounters[CLIENT_TX_BYTES] =
				Counter(
					{{"protocol", "tcp"}, {"direction", "tx"}},
					byteClientCounter);
			[[fallthrough]];
		case KEY_END:
			break;
	}
	// clang-format on
}
#endif
