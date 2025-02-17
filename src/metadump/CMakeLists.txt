include_directories(${CMAKE_CURRENT_SOURCE_DIR})

aux_source_directory(. METADUMP_SOURCES)
add_executable(sfsmetadump ${METADUMP_SOURCES})
target_link_libraries(sfsmetadump)
install(TARGETS sfsmetadump RUNTIME DESTINATION ${SBIN_SUBDIR})
