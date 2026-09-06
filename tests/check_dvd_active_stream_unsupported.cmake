if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "SOURCE_DIR not supplied")
endif()
file(READ "${SOURCE_DIR}/README.md" README)
file(READ "${SOURCE_DIR}/include/bdmvauthor/author.hpp" AUTHOR_H)
file(READ "${SOURCE_DIR}/src/author.cpp" AUTHOR)
file(READ "${SOURCE_DIR}/src/gui.cpp" GUI)
foreach(needle
  "persistent Active-option (yellow by default) indicators are not supported on DVD menus"
  "persistent configurable Active-option indicator"
  "Blu-ray/UHD HDMV menus only")
  string(FIND "${README}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing documented DVD Active-option limitation: ${needle}")
  endif()
endforeach()
foreach(forbidden
  "dvd_menu_active_state_count"
  "DvdMenuActiveStatePlan"
  "dvd_draw_active_option"
  "dvd-active-state"
  "dvd_state_limit_warned_menus_"
  "warn_if_dvd_menu_state_limit_exceeded")
  string(FIND "${AUTHOR_H}\n${AUTHOR}\n${GUI}" "${forbidden}" pos)
  if(NOT pos EQUAL -1)
    message(FATAL_ERROR "experimental DVD persistent Active-option implementation was not fully reverted: ${forbidden}")
  endif()
endforeach()
