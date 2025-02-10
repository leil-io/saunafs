collect_sources(SAUNAFS_ADMIN)

add_library(saunafs-admin-lib ${SAUNAFS_ADMIN_SOURCES})
target_link_libraries(saunafs-admin-lib PRIVATE sfscommon)
create_unittest(saunafs-admin-lib ${SAUNAFS_ADMIN_TESTS})
link_unittest(saunafs-admin-lib sfscommon)

add_executable(saunafs-admin ${SAUNAFS_ADMIN_MAIN})
target_link_libraries(saunafs-admin PRIVATE saunafs-admin-lib)
install(TARGETS saunafs-admin RUNTIME DESTINATION ${BIN_SUBDIR})

if(NOT MINGW)
  add_custom_target(saunafs-probe ALL
    COMMAND ln -sf saunafs-admin saunafs-probe
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS saunafs-admin)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/saunafs-probe DESTINATION ${BIN_SUBDIR})
endif()
