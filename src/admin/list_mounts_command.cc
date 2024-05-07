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
#include "admin/list_mounts_command.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <vector>

#include "common/goal.h"
#include "common/human_readable_format.h"
#include "common/saunafs_version.h"
#include "common/legacy_string.h"
#include "common/legacy_vector.h"
#include "protocol/packet.h"
#include "common/serialization.h"
#include "common/serialization_macros.h"
#include "common/server_connection.h"

typedef std::array<uint32_t, 16> OperationStats;

SERIALIZABLE_CLASS_BEGIN(MountEntry)
SERIALIZABLE_CLASS_BODY(MountEntry,
		uint32_t, sessionId,
		uint32_t, peerIp,
		uint32_t, version,
		LegacyString<uint32_t>, info,
		LegacyString<uint32_t>, path,
		uint8_t, flags,
		uint32_t, rootuid,
		uint32_t, rootgid,
		uint32_t, mapalluid,
		uint32_t, mapallgid,
		uint8_t, minGoal,
		uint8_t, maxGoal,
		uint32_t, minTrashTime,
		uint32_t, maxTrashTime,
		OperationStats, currentOpStats,
		OperationStats, lastHourOpStats)
SERIALIZABLE_CLASS_END;

std::string ListMountsCommand::name() const {
	return "list-mounts";
}

SaunaFsAdminCommand::SupportedOptions ListMountsCommand::supportedOptions() const {
	return {
		{kPorcelainMode, kPorcelainModeDescription},
		{kVerboseMode,   "Be a little more verbose and show goal and trash time limits."},
	};
}

void ListMountsCommand::usage() const {
	std::cerr << name() << " <master ip> <master port>\n";
	std::cerr << "    Prints information about all connected mounts.\n";
}

void ListMountsCommand::run(const Options& options) const {
	if (options.arguments().size() != 2) {
		throw WrongUsageException("Expected <master ip> and <master port> for " + name());
	}

	ServerConnection connection(options.argument(0), options.argument(1));
	LegacyVector<MountEntry> mounts;
	std::vector<uint8_t> request, response;
	serializeLegacyPacket(request, CLTOMA_SESSION_LIST, true);
	response = connection.sendAndReceive(request, MATOCL_SESSION_LIST);
	// There is uint16_t SESSION_STATS = 16 at the beginning of response
	uint16_t dummy;
	deserializeAllLegacyPacketDataNoHeader(response, dummy, mounts);

	std::sort(mounts.begin(), mounts.end(),
			[](const MountEntry& a, const MountEntry& b) -> bool
			{ return a.sessionId < b.sessionId; });

	for (const MountEntry& mount : mounts) {
		bool readonly = mount.flags & SESFLAG_READONLY;
		bool restrictedIp = !(mount.flags & SESFLAG_DYNAMICIP);
		bool ignoregid = mount.flags & SESFLAG_IGNOREGID;
		bool allCanChangeQuota = mount.flags & SESFLAG_ALLCANCHANGEQUOTA;
		bool mapAll = mount.flags & SESFLAG_MAPALL;
		bool shouldPrintGoal = GoalId::isValid(mount.minGoal) && GoalId::isValid(mount.maxGoal);
		bool shouldPrintTrashTime = mount.minTrashTime < mount.maxTrashTime
				&& (mount.minTrashTime != 0 || mount.maxTrashTime != 0xFFFFFFFF);

		if (!options.isSet(kPorcelainMode)) {
			std::cout << "session " << mount.sessionId << ": " << '\n'
					<< "\tip: " << ipToString(mount.peerIp) << '\n'
					<< "\tmount point: " << mount.info << '\n'
					<< "\tversion: " << saunafsVersionToString(mount.version) << '\n'
					<< "\troot dir: " << mount.path << '\n'
					<< "\troot uid: " << mount.rootuid << '\n'
					<< "\troot gid: " << mount.rootgid << '\n'
					<< "\tusers uid: " << mount.mapalluid << '\n'
					<< "\tusers gid: " << mount.mapallgid << '\n'
					<< "\tread only: " << (readonly ? "yes" : "no") << '\n'
					<< "\trestricted ip: " << (restrictedIp ? "yes" : "no") << '\n'
					<< "\tignore gid: " << (ignoregid ? "yes" : "no") << '\n'
					<< "\tall can change quota: " << (allCanChangeQuota ? "yes" : "no") << '\n'
					<< "\tmap all users: " << (mapAll ? "yes" : "no") << std::endl;

			if (options.isSet(kVerboseMode)) {
				if (shouldPrintGoal) {
					std::cout << "\tmin goal: " << static_cast<uint32_t>(mount.minGoal) << std::endl
							<< "\tmax goal: " << static_cast<uint32_t>(mount.maxGoal) << std::endl;
				} else {
					std::cout << "\tmin goal: -\n\tmax goal: -" << std::endl;
				}

				if (shouldPrintTrashTime) {
					std::cout << "\tmin trash time:" << mount.minTrashTime << std::endl
							<< "\tmax trash time: " << mount.maxTrashTime << std::endl;
				} else {
					std::cout << "\tmin trash time: -\n\tmax trash time: -" << std::endl;
				}
			}
		} else {
			std::cout << mount.sessionId
					<< ' ' << ipToString(mount.peerIp)
					<< ' ' << mount.info
					<< ' ' << saunafsVersionToString(mount.version)
					<< ' ' << mount.path
					<< ' ' << mount.rootuid
					<< ' ' << mount.rootgid
					<< ' ' << mount.mapalluid
					<< ' ' << mount.mapallgid
					<< ' ' << (readonly ? "yes" : "no")
					<< ' ' << (restrictedIp ? "yes" : "no")
					<< ' ' << (ignoregid ? "yes" : "no")
					<< ' ' << (allCanChangeQuota ? "yes" : "no")
					<< ' ' << (mapAll ? "yes" : "no");
			if (options.isSet(kVerboseMode)) {
				if (shouldPrintGoal) {
					std::cout << ' ' << static_cast<uint32_t>(mount.minGoal)
							<< ' ' << static_cast<uint32_t>(mount.maxGoal);
				} else {
					std::cout << " - -";
				}

				if (shouldPrintTrashTime) {
					std::cout << ' ' << mount.minTrashTime
							<< ' ' << mount.maxTrashTime;
				} else {
					std::cout << " - -";
				}
			}
			std::cout << std::endl;
		}
	}
}
