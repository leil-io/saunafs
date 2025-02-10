include_directories(${CMAKE_CURRENT_SOURCE_DIR})

collect_sources(CHUNKSERVER_PLUGINS)
shared_add_library(chunkserver-common ${CHUNKSERVER_PLUGINS_SOURCES})
target_link_libraries(chunkserver-common PRIVATE safsprotocol sfscommon
    ${Boost_LIBRARIES} ${ADDITIONAL_LIBS} Boost::system Boost::filesystem)
create_unittest(chunkserver-common ${CHUNKSERVER_PLUGINS_TESTS})
link_unittest(chunkserver-common chunkserver-common sfscommon)
