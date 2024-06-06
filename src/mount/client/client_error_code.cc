/*

   Copyright 2017 Skytechnology sp. z o.o.
   Copyright 2023 Leil Storage OÜ

   This file is part of SaunaFS.

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

#include <string>
#include "common/platform.h"
#include "common/sfserr/saunafs_error_codes.h"
#include "common/sfserr/sfserr.h"

#include "client_error_code.h"

saunafs::detail::saunafs_error_category saunafs::detail::saunafs_error_category::instance_;

std::string saunafs::detail::saunafs_error_category::message(int ev) const {
	return saunafs_error_string(ev);
}

bool saunafs::detail::saunafs_error_category::equivalent(
		int code, const std::error_condition &condition) const noexcept {
	if (default_error_condition(code) == condition) {
		return true;
	}

	switch (code) {
	case (int)saunafs::error::operation_not_permitted:
		return std::make_error_code(std::errc::operation_not_permitted) == condition;
	case (int)saunafs::error::not_a_directory:
		return std::make_error_code(std::errc::not_a_directory) == condition;
	case (int)saunafs::error::no_such_file_or_directory:
		return std::make_error_code(std::errc::no_such_file_or_directory) == condition;
	case (int)saunafs::error::permission_denied:
		return std::make_error_code(std::errc::permission_denied) == condition;
	case (int)saunafs::error::file_exists:
		return std::make_error_code(std::errc::file_exists) == condition;
	case (int)saunafs::error::invalid_argument:
		return std::make_error_code(std::errc::invalid_argument) == condition;
	case (int)saunafs::error::directory_not_empty:
		return std::make_error_code(std::errc::directory_not_empty) == condition;
	case (int)saunafs::error::no_space_left:
		return std::make_error_code(std::errc::no_space_on_device) == condition;
	case (int)saunafs::error::io_error:
		return std::make_error_code(std::errc::io_error) == condition;
	case (int)saunafs::error::read_only_file_system:
		return std::make_error_code(std::errc::read_only_file_system) == condition;
	case (int)saunafs::error::attribute_not_found:
#if defined(__APPLE__) || defined(__FreeBSD__)
		return std::make_error_code(std::errc::no_message) == condition;
#else
		return std::make_error_code(std::errc::no_message_available) == condition;
#endif
	case (int)saunafs::error::not_supported:
		return std::make_error_code(std::errc::not_supported) == condition;
	case (int)saunafs::error::result_out_of_range:
		return std::make_error_code(std::errc::result_out_of_range) == condition;
	case (int)saunafs::error::no_lock_available:
		return std::make_error_code(std::errc::no_lock_available) == condition;
	case (int)saunafs::error::filename_too_long:
		return std::make_error_code(std::errc::filename_too_long) == condition;
	case (int)saunafs::error::file_too_large:
		return std::make_error_code(std::errc::file_too_large) == condition;
	case (int)saunafs::error::bad_file_descriptor:
		return std::make_error_code(std::errc::bad_file_descriptor) == condition;
#if defined(__APPLE__) || defined(__FreeBSD__)
	case (int)saunafs::error::no_message:
		return std::make_error_code(std::errc::no_message) == condition;
#else
	case (int)saunafs::error::no_message_available:
		return std::make_error_code(std::errc::no_message_available) == condition;
#endif
	case (int)saunafs::error::not_enough_memory:
		return std::make_error_code(std::errc::not_enough_memory) == condition;
	case (int)saunafs::error::argument_list_too_long:
		return std::make_error_code(std::errc::argument_list_too_long) == condition;
	}

	return false;
}

bool saunafs::detail::saunafs_error_category::equivalent(const std::error_code &code,
		int condition) const noexcept {
	if (code.category() == *this && code.value() == condition) {
		return true;
	}

	switch (condition) {
	case (int)saunafs::error::operation_not_permitted:
		return code == std::make_error_condition(std::errc::operation_not_permitted);
	case (int)saunafs::error::not_a_directory:
		return code == std::make_error_condition(std::errc::not_a_directory);
	case (int)saunafs::error::no_such_file_or_directory:
		return code == std::make_error_condition(std::errc::no_such_file_or_directory);
	case (int)saunafs::error::permission_denied:
		return code == std::make_error_condition(std::errc::permission_denied);
	case (int)saunafs::error::file_exists:
		return code == std::make_error_condition(std::errc::file_exists);
	case (int)saunafs::error::invalid_argument:
		return code == std::make_error_condition(std::errc::invalid_argument);
	case (int)saunafs::error::directory_not_empty:
		return code == std::make_error_condition(std::errc::directory_not_empty);
	case (int)saunafs::error::no_space_left:
		return code == std::make_error_condition(std::errc::no_space_on_device);
	case (int)saunafs::error::io_error:
		return code == std::make_error_condition(std::errc::io_error);
	case (int)saunafs::error::read_only_file_system:
		return code == std::make_error_condition(std::errc::read_only_file_system);
	case (int)saunafs::error::attribute_not_found:
#if defined(__APPLE__) || defined(__FreeBSD__)
		return code == std::make_error_condition(std::errc::no_message);
#else
		return code == std::make_error_condition(std::errc::no_message_available);
#endif
	case (int)saunafs::error::not_supported:
		return code == std::make_error_condition(std::errc::not_supported);
	case (int)saunafs::error::result_out_of_range:
		return code == std::make_error_condition(std::errc::result_out_of_range);
	case (int)saunafs::error::no_lock_available:
		return code == std::make_error_condition(std::errc::no_lock_available);
	case (int)saunafs::error::filename_too_long:
		return code == std::make_error_condition(std::errc::filename_too_long);
	case (int)saunafs::error::file_too_large:
		return code == std::make_error_condition(std::errc::file_too_large);
	case (int)saunafs::error::bad_file_descriptor:
		return code == std::make_error_condition(std::errc::bad_file_descriptor);
#if defined(__APPLE__) || defined(__FreeBSD__)
	case (int)saunafs::error::no_message:
		return code == std::make_error_condition(std::errc::no_message);
#else
	case (int)saunafs::error::no_message_available:
		return code == std::make_error_condition(std::errc::no_message_available);
#endif
	case (int)saunafs::error::not_enough_memory:
		return code == std::make_error_condition(std::errc::not_enough_memory);
	case (int)saunafs::error::argument_list_too_long:
		return code == std::make_error_condition(std::errc::argument_list_too_long);
	}

	return false;
}
