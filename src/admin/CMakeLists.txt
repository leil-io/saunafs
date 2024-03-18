collect_sources(SAUNAFS_ADMIN)

add_library(saunafs-admin-lib ${SAUNAFS_ADMIN_SOURCES})
target_link_libraries(saunafs-admin-lib sfscommon)
create_unittest(saunafs-admin-lib ${SAUNAFS_ADMIN_TESTS})
link_unittest(saunafs-admin-lib sfscommon)

add_executable(saunafs-admin ${SAUNAFS_ADMIN_MAIN})
target_link_libraries(saunafs-admin saunafs-admin-lib)
install(TARGETS saunafs-admin RUNTIME DESTINATION ${BIN_SUBDIR})
