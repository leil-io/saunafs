function(check_members STRUCT MEMBERS HEADER)
  foreach(MEMBER ${MEMBERS})
    string(REPLACE " " "_" STRUCT_STR ${STRUCT})
    string(TOUPPER "SAUNAFS_HAVE_${STRUCT_STR}_${MEMBER}" VAR)
    CHECK_STRUCT_HAS_MEMBER(${STRUCT} ${MEMBER} ${HEADER} ${VAR})
    if(NOT ${${VAR}} EQUAL 1)
      set(${VAR} 0)
      message(WARNING "${STRUCT} has no member ${MEMBER}")
    endif()
  endforeach()
endfunction(check_members)

