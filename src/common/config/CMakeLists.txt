collect_sources(CONFIG)

shared_add_library(config ${CONFIG_SOURCES})
shared_target_link_libraries(config ${YAML_CPP_LIBRARIES})

include_directories(${GTEST_INCLUDE_DIRS})
add_executable(config_test ${CONFIG_TESTS})
target_link_libraries(config_test config GTest::gtest_main ${Boost_FILESYSTEM_LIBRARY})
