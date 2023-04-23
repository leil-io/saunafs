# Plugins must be located in a subdirectory of this directory.
file(GLOB PLUGIN_DIRS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/*)

foreach(PLUGIN_DIR ${PLUGIN_DIRS})
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${PLUGIN_DIR}/CMakeLists.txt)
        add_subdirectory(${PLUGIN_DIR})
    endif()
endforeach()
