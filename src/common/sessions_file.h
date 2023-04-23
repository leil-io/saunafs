#pragma once

#include "common/platform.h"
#include "common/serialization_macros.h"

SAUNAFS_DEFINE_SERIALIZABLE_CLASS(SessionFiles,
		uint32_t, sessionId,
		uint32_t, peerIp,
		uint32_t, filesNumber);
