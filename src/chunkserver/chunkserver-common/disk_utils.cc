#include "disk_utils.h"

namespace disk {

Configuration::Configuration(std::string hddCfgLine) {
	// Trim leading whitespace characters
	auto forwardIt =
	    std::find_if(hddCfgLine.begin(), hddCfgLine.end(), [](char symbol) {
		    return !std::isspace<char>(symbol, std::locale::classic());
	    });

	if (hddCfgLine.begin() != forwardIt) {
		hddCfgLine.erase(hddCfgLine.begin(), forwardIt);
	}

	if (hddCfgLine.empty()) {  // The complete line is empty
		isValid = false;
		isEmpty = true;
		return;
	}

	if (hddCfgLine.at(0) == '#') {  // Skip comments
		isValid = false;
		isComment = true;
		return;
	}

	// Trim trailing whitespace characters
	auto reverseIt =
	    std::find_if(hddCfgLine.rbegin(), hddCfgLine.rend(), [](char symbol) {
		    return !std::isspace<char>(symbol, std::locale::classic());
	    });
	hddCfgLine.erase(reverseIt.base(), hddCfgLine.end());

	if (hddCfgLine.at(0) == '*') {
		isMarkedForRemoval = true;
		hddCfgLine.erase(hddCfgLine.begin());
	}

	static const std::string zonedToken = "zonefs:";
	if (hddCfgLine.find(zonedToken) == 0) {
		prefix = hddCfgLine.substr(0, zonedToken.size() - 1);
		isZoned = true;
		hddCfgLine.erase(0, zonedToken.size());
	}

	static std::string const delimiter = " | ";
	auto delimiterPos = hddCfgLine.find(delimiter);

	if (isZoned && delimiterPos == std::string::npos) {
		safs_pretty_syslog(LOG_WARNING,
		                   "Parse hdd line: %s - zoned drives must contain two "
		                   "paths separated by ' | '.",
		                   hddCfgLine.c_str());
		isValid = false;
		return;
	}

	if (delimiterPos != std::string::npos) {
		metaPath = hddCfgLine.substr(0, delimiterPos);
		dataPath = hddCfgLine.substr(delimiterPos + delimiter.length());
	} else {
		metaPath = hddCfgLine;
		dataPath = hddCfgLine;
	}

	// Ensure / at the end for both paths
	if (metaPath.at(metaPath.size() - 1) != '/') {
		metaPath.append("/");
	}

	if (dataPath.at(dataPath.size() - 1) != '/') {
		dataPath.append("/");
	}

	isValid = true;
}

Configuration::Configuration(const std::string &_metaPath,
                             const std::string &_dataPath,
                             bool _isMarkedForRemoval, bool _isZonedDevice)
    : metaPath(_metaPath),
      dataPath(_dataPath),
      isMarkedForRemoval(_isMarkedForRemoval),
      isZoned(_isZonedDevice),
      isValid(true) {}

}  // namespace disk
