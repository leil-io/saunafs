#pragma once

#include <sys/syslog.h>
#include <cstring>
#include <future>
#include "common/memory_file.h"
#include "common/slogger.h"

struct MetadataSectionLoaderOptions {
	MetadataSectionLoaderOptions(const MemoryMappedFile &metadataFile_,
	                             size_t offset_, int ignoreFlag_,
	                             uint64_t sectionLength_, bool loadLockIds_)
	    : metadataFile(metadataFile_),
	      offset(offset_),
	      ignoreFlag(ignoreFlag_),
	      sectionLength(sectionLength_),
	      loadLockIds(loadLockIds_) {}

	const MemoryMappedFile &metadataFile;
	size_t offset;
	int ignoreFlag;
	uint64_t sectionLength;
	bool loadLockIds;
};

struct MetadataSection {
	using LoadImplMethod = std::function<bool(MetadataSectionLoaderOptions)>;

	MetadataSection(std::string_view name_, std::string_view description_,
	                LoadImplMethod load_, bool asyncLoad_ = true,
	                bool isLegacy_ = false)
	    : name(name_),
	      description(description_),
	      load(std::move(load_)),
	      asyncLoad(asyncLoad_),
	      isLegacy(isLegacy_) {}

	bool matchesSectionTypeOf(const uint8_t *sectionPtr) const {
		return std::memcmp(sectionPtr, name.data(), name.size()) == 0;
	}

	std::string_view name;
	std::string_view description;
	LoadImplMethod load;
	bool asyncLoad;
	bool isLegacy;
};

struct MetadataLoaderFuture {
	std::string sectionName;
	std::string sectionDescription;
	std::future<bool> future;
};

class MetadataLoader {
public:
	using Futures = std::vector<MetadataLoaderFuture>;
	using Options = MetadataSectionLoaderOptions;

	static bool loadSection(const MetadataSection &, Options);

	static std::future<bool> loadSectionAsync(const MetadataSection &, Options);

	static void loadSectionAsync(const MetadataSection &, Options, Futures &);
};
