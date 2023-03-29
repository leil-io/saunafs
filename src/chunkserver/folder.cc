#include "chunkserver/folder.h"

#include <bitset>

#define MarkedForDeletionFlagIndex 0
#define IsDamageFlagIndex          1
#define ScanInProgressFlagIndex    2

bool Folder::isMarkedForDeletion() const {
	return isMarkedForRemoval || isReadOnly;
}

bool Folder::isSelectableForNewChunk() const {
	return !(isDamaged || isMarkedForDeletion() || totalSpace == 0
	         || availableSpace == 0
	         || scanState != Folder::ScanState::kWorking);
}

DiskInfo Folder::toDiskInfo() const
{
	DiskInfo diskInfo;

	diskInfo.path = path;
	if (diskInfo.path.length() > MooseFsString<uint8_t>::maxLength()) {
		std::string dots("(...)");
		uint32_t substrSize = MooseFsString<uint8_t>::maxLength() - dots.length();
		diskInfo.path = dots + diskInfo.path.substr(diskInfo.path.length()
		                                            - substrSize, substrSize);
	}

	diskInfo.entrySize = serializedSize(diskInfo) - serializedSize(diskInfo.entrySize);

	std::bitset<8> flags;
	flags.set(MarkedForDeletionFlagIndex, isMarkedForDeletion());
	flags.set(IsDamageFlagIndex, isDamaged);
	flags.set(ScanInProgressFlagIndex, scanState == ScanState::kInProgress);
	diskInfo.flags = static_cast<uint8_t>(flags.to_ulong());

	uint32_t erroIndex = (lastErrorIndex + (LAST_ERROR_SIZE - 1)) % LAST_ERROR_SIZE;
	diskInfo.errorChunkId = lastErrorTab[erroIndex].chunkid;
	diskInfo.errorTimeStamp = lastErrorTab[erroIndex].timestamp;

	if (scanState == ScanState::kInProgress) {
		diskInfo.used = scanProgress;
		diskInfo.total = 0;
	} else {
		diskInfo.used = totalSpace - availableSpace;
		diskInfo.total = totalSpace;
	}

	diskInfo.chunksCount = chunks.size();

	// Statistics: last minute
	HddStatistics hddStats = stats[statsPos];
	diskInfo.lastMinuteStats = hddStats;

	// last hour
	for (auto pos = 1 ; pos < 60 ; pos++) {
		hddStats.add(stats[(statsPos + pos) % STATS_HISTORY]);
	}
	diskInfo.lastHourStats = hddStats;

	// last day
	for (auto pos = 60 ; pos < 24 * 60 ; pos++) {
		hddStats.add(stats[(statsPos + pos) % STATS_HISTORY]);
	}
	diskInfo.lastDayStats = hddStats;

	return diskInfo;
}
