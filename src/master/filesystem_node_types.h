/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA
   Copyright 2013-2014 EditShare
   Copyright 2013-2016 Skytechnology sp. z o.o.
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

#pragma once

#include "common/platform.h"

#include <array>
#include <cstdint>
#include <unordered_map>

#include "common/goal.h"
#include "common/compact_vector.h"
#include "protocol/SFSCommunication.h"

#if defined(SAUNAFS_HAVE_64BIT_JUDY) &&               \
    (!defined(DISABLE_JUDY_FOR_TRASHPATHCONTAINER) || \
     !defined(DISABLE_JUDY_FOR_RESERVEDPATHCONTAINER))
#include "common/judy_map.h"
#endif
#if !defined(SAUNAFS_HAVE_64BIT_JUDY) ||            \
    defined(DISABLE_JUDY_FOR_TRASHPATHCONTAINER) || \
    defined(DISABLE_JUDY_FOR_RESERVEDPATHCONTAINER)
#include <map>
#endif

#include <ext/pb_ds/assoc_container.hpp>
#include <ext/pb_ds/tree_policy.hpp>

#include "master/hstring_storage.h"

#define NODEHASHBITS (22)
#define NODEHASHSIZE (1 << NODEHASHBITS)
#define NODEHASHPOS(nodeid) ((nodeid) & (NODEHASHSIZE - 1))
#define NODECHECKSUMSEED 12345

#define EDGEHASHBITS (22)
#define EDGEHASHSIZE (1 << EDGEHASHBITS)
#define EDGEHASHPOS(hash) ((hash) & (EDGEHASHSIZE - 1))
#define EDGECHECKSUMSEED 1231241261

#define MAX_INDEX 0x7FFFFFFF

enum class AclInheritance { kInheritAcl, kDontInheritAcl };

// Arguments for verify_session
enum class SessionType { kNotMeta, kOnlyMeta, kAny };
enum class OperationMode { kReadWrite, kReadOnly };
enum class ExpectedNodeType { kFile, kDirectory, kNotDirectory, kFileOrDirectory, kAny };

using TrashtimeMap = std::unordered_map<uint32_t, uint32_t>;
using GoalStatistics = std::array<uint32_t, GoalId::kMax + 1>;

struct statsrecord {
	uint32_t inodes;
	uint32_t dirs;
	uint32_t files;
	uint32_t links;
	uint32_t chunks;
	uint64_t length;
	uint64_t size;
	uint64_t realsize;
};

/*! \brief Node containing common meta data for each file system object (file or directory).
 *
 * Node size = 64B
 *
 * Estimating (taking into account directory and file node size) 150B per file.
 *
 * 10K files will occupy 1.5MB
 * 10M files will occupy 1.5GB
 * 1G files will occupy 150GB
 * 4G files will occupy 600GB
 */
struct FSNode {
	enum {
		kFile = TYPE_FILE,
		kDirectory = TYPE_DIRECTORY,
		kSymlink = TYPE_SYMLINK,
		kFifo = TYPE_FIFO,
		kBlockDev = TYPE_BLOCKDEV,
		kCharDev = TYPE_CHARDEV,
		kSocket = TYPE_SOCKET,
		kTrash = TYPE_TRASH,
		kReserved = TYPE_RESERVED,
		kUnknown = TYPE_UNKNOWN
	};

	uint32_t id; /*!< Unique number identifying node. */
	uint32_t ctime; /*!< Change time. */
	uint32_t mtime; /*!< Modification time. */
	uint32_t atime; /*!< Access time. */
	uint8_t type; /*!< Node type. (file, directory, symlink, ...) */
	uint8_t goal; /*!< Goal id. */
	uint16_t mode;  /*!< Only 12 lowest bits are used for mode, in unix standard upper 4 are used
	                 for object type, but since there is field "type" this bits can be used as
	                 extra flags. */
	uint32_t uid; /*!< User id. */
	uint32_t gid; /*!< Group id. */
	uint32_t trashtime; /*!< Trash time. */

	compact_vector<std::pair<uint32_t, const hstorage::Handle *>, uint32_t>
	    parent; /*!< Parent nodes ids + handles of entries of this node in those parents.
	               To reduce memory usage ids are stored instead of pointers to
	               FSNode. */

	FSNode   *next; /*!< Next field used for storing FSNode in hash map. */
	uint64_t checksum; /*!< Node checksum. */

	FSNode(uint8_t t) {
		type = t;
		next = nullptr;
		checksum = 0;
	}

	/*! \brief Static function used for creating proper node for given type.
	 * \param type Type of node to create.
	 * \return Pointer to created node.
	 */
	static FSNode *create(uint8_t type);

	/*! \brief Static function used for erasing node (uses node's type
	 * for correct invocation of destructors).
	 *
	 * \param node Pointer to node that should be erased.
	 */
	static void destroy(FSNode *node);
};

constexpr FSNode* kUnknownNode = nullptr;

/*! \brief Node used for storing file object.
 *
 * Node size = 64B + 40B + 8 * chunks_count + 4 * session_count
 * Avg size (assuming 1 chunk and session id) = 104 + 8 + 4 ~ 120B
 */
struct FSNodeFile : public FSNode {
	uint64_t length{};
	compact_vector<uint32_t> sessionid;
	compact_vector<uint64_t, uint32_t> chunks;

	explicit FSNodeFile(uint8_t t) : FSNode(t) {
		assert(t == kFile || t == kTrash || t == kReserved);
	}

	uint32_t chunkCount() const {
		for(uint32_t i = chunks.size(); i > 0; --i) {
			if (chunks[i-1] != 0) {
				return i;
			}
		}

		return 0;
	}
};

/*! \brief Node used for storing symbolic link.
 *
 * Node size = 64 + 16 = 80B
 */
struct FSNodeSymlink : public FSNode {
	hstorage::Handle path;
	uint16_t path_length{};

	explicit FSNodeSymlink() : FSNode(kSymlink) {
	}
};

/*! \brief Node used for storing device object.
 *
 * Node size = 64 + 8 = 72B
 */
struct FSNodeDevice : public FSNode {
	uint32_t rdev;

	FSNodeDevice(uint8_t device_type) : FSNode(device_type), rdev() {
	}
};

/*! \brief Node used for storing directory.
 *
 * Node size = 64 + 56 + 16 * entries_count
 * Avg size (10 files) ~ 280B (28B per file)
 */
struct FSNodeDirectory : public FSNode {
	struct HandleCompare {
		bool operator()(const std::pair<hstorage::Handle *, FSNode *> &a,
		                const std::pair<hstorage::Handle *, FSNode *> &b) const {
			return std::make_pair(a.first->data(), a.second) <
			       std::make_pair(b.first->data(), b.second);
		}
	};

	using EntriesContainer =
	    __gnu_pbds::tree<std::pair<hstorage::Handle *, FSNode *>,
	                     __gnu_pbds::null_type, HandleCompare,
	                     __gnu_pbds::rb_tree_tag,
	                     __gnu_pbds::tree_order_statistics_node_update>;

	using iterator = EntriesContainer::iterator;
	using const_iterator = EntriesContainer::const_iterator;

	EntriesContainer entries; /*!< Directory entries (entry: name + pointer to child node). */
	EntriesContainer
	    lowerCaseEntries; /*!< Directory entries with lowe case name (entry:
	                         name + pointer to child node). */
	bool case_insensitive = false;
	statsrecord stats; /*!< Directory statistics (including subdirectories). */
	uint32_t nlink; /*!< Number of directories linking to this directory. */
	uint16_t entries_hash;
	uint16_t lowerCaseEntriesHash;

	FSNodeDirectory() : FSNode(kDirectory) {
		memset(&stats, 0, sizeof(stats));
		nlink = 2;
		entries_hash = 0;
		lowerCaseEntriesHash = 0;
	}

	~FSNodeDirectory() {
	}


	/*! \brief Find directory entry with given name.
	 *
	 * \param name Name of entry to find.
	 * \return If node is found returns iterator pointing to directory entry containing node,
	 *         otherwise entries.end().
	 */
	iterator find(const HString& name) {
		uint64_t name_hash = (hstorage::Handle::HashType)name.hash();

		if (case_insensitive) {
			HString lowerCaseNameHandle = HString::hstringToLowerCase(name);
			name_hash = (hstorage::Handle::HashType)lowerCaseNameHandle.hash();
			auto tmp_handle =
			    hstorage::Handle(name_hash << hstorage::Handle::kHashShift);
			auto pair_to_find = std::make_pair(&tmp_handle, kUnknownNode);
			auto lowerCaseIt = lowerCaseEntries.lower_bound(pair_to_find);

			for (; lowerCaseIt != lowerCaseEntries.end(); ++lowerCaseIt) {
				if ((*lowerCaseIt).first->hash() != name_hash) {
					break;
				}
				if (*((*lowerCaseIt).first) == lowerCaseNameHandle) {
					auto it = find((*lowerCaseIt).second);
					return it;
				}
			}
		} else {
			auto tmp_handle =
			    hstorage::Handle(name_hash << hstorage::Handle::kHashShift);
			auto pair_to_find = std::make_pair(&tmp_handle, kUnknownNode);
			auto it = entries.lower_bound(pair_to_find);

			for (; it != entries.end(); ++it) {
				if ((*it).first->hash() != name_hash) {
					break;
				}
				if (*((*it).first) == name) {
					return it;
				}
			}
		}

		return entries.end();
	}

	iterator find_lowercase_container(const HString &name) {
		uint64_t name_hash = (hstorage::Handle::HashType)name.hash();

		if (case_insensitive) {
			HString lowerCaseNameHandle = HString::hstringToLowerCase(name);
			name_hash = (hstorage::Handle::HashType)lowerCaseNameHandle.hash();
			auto tmp_handle =
			    hstorage::Handle(name_hash << hstorage::Handle::kHashShift);
			auto pair_to_find = std::make_pair(&tmp_handle, kUnknownNode);
			auto lowerCaseIt = lowerCaseEntries.lower_bound(pair_to_find);

			for (; lowerCaseIt != lowerCaseEntries.end(); ++lowerCaseIt) {
				if ((*lowerCaseIt).first->hash() != name_hash) {
					break;
				}
				if (*((*lowerCaseIt).first) == lowerCaseNameHandle) {
					return lowerCaseIt;
				}
			}
		}

		return lowerCaseEntries.end();
	}

	/*! \brief Find directory entry with given node.
	 *
	 * \param node Node to find.
	 * \return If node is found returns iterator pointing to directory entry containing node,
	 *         otherwise entries.end().
	 */
	iterator find(const FSNode *node) {
		HString name = HString(getChildName(node));
		uint64_t name_hash = (hstorage::Handle::HashType)name.hash();
		auto tmp_handle =
		    hstorage::Handle(name_hash << hstorage::Handle::kHashShift);
		auto pair_to_find = std::make_pair(&tmp_handle, kUnknownNode);
		auto it = entries.lower_bound(pair_to_find);
		for (; it != entries.end(); ++it) {
			if ((*it).first->hash() != name_hash) {
				break;
			}
			if (*((*it).first) == name) {
				return it;
			}
		}
		return entries.end();
	}

	iterator find_nth(EntriesContainer::size_type nth) {
		return entries.find_by_order(nth);
	}

	const_iterator find_nth(EntriesContainer::size_type nth) const {
		return entries.find_by_order(nth);
	}

	/*! \brief Returns name for specified node.
	 *
	 * \param node Pointer to node.
	 * \return If node is found returns name associated with this node,
	 *         otherwise returns empty string.
	 */
	std::string getChildName(const FSNode *node) const {
		for (const auto &[parentId, hstring] : node->parent) {
			if (parentId == this->id) {
				return hstring->get();
			}
		}
		return std::string();
	}

	iterator begin() {
		return entries.begin();
	}

	iterator end() {
		return entries.end();
	}

	const_iterator begin() const {
		return entries.begin();
	}

	const_iterator end() const {
		return entries.end();
	}
};

struct TrashPathKey {
	explicit TrashPathKey(const FSNode *node) :
#ifdef WORDS_BIGENDIAN
	    timestamp(std::min((uint64_t)node->ctime + node->trashtime, (uint64_t)UINT32_MAX)),
	    id(node->id)
#else
	    id(node->id),
	    timestamp(std::min((uint64_t)node->ctime + node->trashtime, (uint64_t)UINT32_MAX))
#endif
	{}

	bool operator<(const TrashPathKey &other) const {
		return std::make_pair(timestamp, id) < std::make_pair(other.timestamp, other.id);
	}

#ifdef WORDS_BIGENDIAN
	uint32_t timestamp;
	uint32_t id;
#else
	uint32_t id;
	uint32_t timestamp;
#endif
};

#if defined(SAUNAFS_HAVE_64BIT_JUDY) && !defined(DISABLE_JUDY_FOR_TRASHPATHCONTAINER)
using TrashPathContainer = judy_map<TrashPathKey, hstorage::Handle>;
#else
using TrashPathContainer = std::map<TrashPathKey, hstorage::Handle>;
#endif

#if defined(SAUNAFS_HAVE_64BIT_JUDY) && !defined(DISABLE_JUDY_FOR_RESERVEDPATHCONTAINER)
using ReservedPathContainer = judy_map<uint32_t, hstorage::Handle>;
#else
using ReservedPathContainer = std::map<uint32_t, hstorage::Handle>;
#endif
