configure_file(saunafs-uraft-helper.in saunafs-uraft-helper @ONLY)

include_directories(${CMAKE_CURRENT_SOURCE_DIR})

collect_sources(URAFT)
set(URAFT_SOURCES ${URAFT_SOURCES} ${CMAKE_SOURCE_DIR}/src/common/time_utils.cc)

add_executable(saunafs-uraft ${URAFT_MAIN} ${URAFT_SOURCES})

target_link_libraries(saunafs-uraft ${Boost_LIBRARIES} ${RT_LIBRARY} pthread)

install(TARGETS saunafs-uraft RUNTIME DESTINATION ${SBIN_SUBDIR})
install(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/saunafs-uraft-helper DESTINATION ${SBIN_SUBDIR})
