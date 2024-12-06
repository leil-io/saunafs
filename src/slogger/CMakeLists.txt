collect_sources(SLOGGER)

shared_add_library(slogger ${SLOGGER_SOURCES})
shared_target_link_libraries(slogger STATIC config sfserr SHARED config_pic sfserr_pic)
