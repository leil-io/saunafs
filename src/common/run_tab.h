/*
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

#include <functional>
#include <string>

/// Simple function prototype for callbacks
using RunTabFuntion = std::function<int()>;

/// Structure to pack the function and a descriptive name together
struct RunTab {
	RunTabFuntion function;  ///< Actual function to call
	std::string name;        ///< Descriptive name (useful to log errors)
};
