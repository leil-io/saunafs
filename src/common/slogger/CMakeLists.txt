collect_sources(SLOGGER)

shared_add_library(slogger ${SLOGGER_SOURCES})
target_link_libraries(slogger sfserr)
target_link_libraries(slogger sfserr_pic)
