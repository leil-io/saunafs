#include "common/platform.h"
#include "master/metadata_loader.h"

#include "master/filesystem_store.h"

bool MetadataLoader::loadSection(const MetadataSection &section,
                                 Options options) {
	try {
		if (section.load(options)) {
			safs_pretty_syslog(LOG_INFO, "Section loaded successfully (%s)",
			                   section.name.data());
			return true;
		} else {
			safs_pretty_syslog(LOG_ERR, "error reading section (%s)",
			                   section.name.data());
		}
	} catch (const std::exception &e) {
		safs_pretty_syslog(LOG_ERR, "Exception while processing section (%s)",
		                   section.name.data());
		throw MetadataConsistencyException(e.what());
	}
	return false;
}

std::future<bool> MetadataLoader::loadSectionAsync(
    const MetadataSection &section, Options options) {
	return std::async(std::launch::async, loadSection, section, options);
}

void MetadataLoader::loadSectionAsync(const MetadataSection &section,
                                      Options options, Futures &futures) {
	MetadataLoaderFuture future;
	future.sectionName = section.name;
	future.sectionDescription = section.description;
	future.future = loadSectionAsync(section, options);
	futures.push_back(std::move(future));
}
