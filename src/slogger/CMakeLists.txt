collect_sources(SLOGGER)

shared_add_library(slogger ${SLOGGER_SOURCES})
if(MINGW)
    target_link_libraries(slogger fmt::fmt-header-only)
else()
    target_link_libraries(slogger  fmt::fmt-header-only spdlog::spdlog_header_only)
endif()
shared_target_link_libraries(slogger STATIC config sfserr SHARED config_pic sfserr_pic)
