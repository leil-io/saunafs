#  Copyright 2017 Skytechnology sp. z o.o..
#
#  This file is part of LizardFS.
#
#  LizardFS is free software: you can redistribute it and/or modify
#  it under the terms of the GNU General Public License as published by
#  the Free Software Foundation, version 3.
#
#  LizardFS is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
#  You should have received a copy of the GNU General Public License
#  along with LizardFS  If not, see <http://www.gnu.org/licenses/>.

collect_sources(NFS_GANESHA_PLUGIN)

add_definitions(-D__USE_MISC)

include_directories(${CMAKE_SOURCE_DIR}/external/${NFS_GANESHA_DIR_NAME}/src/include)
include_directories(${CMAKE_SOURCE_DIR}/external/${NTIRPC_DIR_NAME}/ntirpc)

add_library(fsallizardfs MODULE ${NFS_GANESHA_PLUGIN_MAIN} ${NFS_GANESHA_PLUGIN_SOURCES})

target_link_libraries(fsallizardfs lizardfs-client_pic)

# Original DESTINATION was ${LIB_SUBDIR}/ganesha
# In this version, it was changed to install at the Ganesha lib folder
# Define FSAL Destination to: /usr/lib/ganesha
SET(CMAKE_INSTALL_PREFIX "/usr" CACHE PATH "Install prefix for common files" FORCE)

# Library path name
get_property(USE_LIB64 GLOBAL PROPERTY FIND_LIBRARY_USE_LIB64_PATHS)
if (USE_LIB64)
        set(LIB_INSTALL_DIR ${CMAKE_INSTALL_PREFIX}/lib64 CACHE PATH
                "Specify name of libdir inside install path")
else (USE_LIB64)
        set(LIB_INSTALL_DIR ${CMAKE_INSTALL_PREFIX}/lib CACHE PATH
                "Specify name of libdir inside install path")
endif (USE_LIB64)

IF(FSAL_DESTINATION)
    set(FSAL_DESTINATION ${FSAL_DESTINATION} )
ELSE()
    set(FSAL_DESTINATION "${LIB_INSTALL_DIR}/ganesha")
ENDIF()

install(TARGETS fsallizardfs COMPONENT fsal DESTINATION ${FSAL_DESTINATION} )
