/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA
   Copyright 2013-2014 EditShare
   Copyright 2013-2015 Skytechnology sp. z o.o.
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

constexpr const char *kMetadataFilename          = "metadata.sfs";
constexpr const char *kMetadataTmpFilename       = "metadata.sfs.tmp";
constexpr const char *kMetadataLegacyFilename    = "metadata.mfs";
constexpr const char *kMetadataEmergencyFilename = "metadata.sfs.emergency";
constexpr const char *kMetadataMlFilename        = "metadata_ml.sfs";
constexpr const char *kMetadataMlTmpFilename     = "metadata_ml.sfs.tmp";
constexpr const char *kChangelogFilename         = "changelog.sfs";
constexpr const char *kChangelogTmpFilename      = "changelog.sfs.tmp";
constexpr const char *kChangelogMlFilename       = "changelog_ml.sfs";
constexpr const char *kChangelogMlTmpFilename    = "changelog_ml.sfs.tmp";
constexpr const char *kSessionsFilename          = "sessions.sfs";
constexpr const char *kSessionsTmpFilename       = "sessions.sfs.tmp";
constexpr const char *kSessionsMlFilename        = "sessions_ml.sfs";
constexpr const char *kSessionsMlTmpFilename     = "sessions_ml.sfs.tmp";
