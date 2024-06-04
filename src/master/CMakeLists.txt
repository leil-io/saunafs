include_directories(${CMAKE_CURRENT_SOURCE_DIR})
add_definitions(-DAPPNAME=sfsmaster -DAPP_EXAMPLES_SUBDIR="${SFSMASTER_EXAMPLES_SUBDIR}")

collect_sources(MASTER)

if(NOT DB_FOUND)
  list(REMOVE_ITEM MASTER_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/hstring_bdbstorage.cc")
endif()

add_library(master ${MASTER_SOURCES})
target_link_libraries(master sfscommon metrics ${ADDITIONAL_LIBS})
if(JUDY_LIBRARY)
  target_link_libraries(master ${JUDY_LIBRARY})
endif()
if(DB_FOUND)
  target_link_libraries(master ${DB_LIBRARY})
endif()
create_unittest(master ${MASTER_TESTS})
link_unittest(master master sfscommon)

add_executable(sfsmaster ${MAIN_SRC})
target_link_libraries(sfsmaster master config metrics ${PAM_LIBRARIES})
if(SYSTEMD_FOUND)
  target_link_libraries(sfsmaster ${SYSTEMD_LIBRARIES})
endif()
install(TARGETS sfsmaster RUNTIME DESTINATION ${SBIN_SUBDIR})

configure_file(sfsrestoremaster.in sfsrestoremaster @ONLY)
install(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/sfsrestoremaster DESTINATION ${SBIN_SUBDIR})
