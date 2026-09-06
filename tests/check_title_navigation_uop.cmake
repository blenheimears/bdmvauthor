if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "SOURCE_DIR not supplied")
endif()

file(READ "${SOURCE_DIR}/include/bdmvauthor/model.hpp" MODEL)
foreach(needle
  "enum class UserOperation"
  "std::vector<NavigationAction> menu_button_actions;"
  "std::uint64_t prohibited_user_operations = 0;"
  "user_operation_bit"
  "user_operation_prohibited")
  string(FIND "${MODEL}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "title navigation/UOP model regression: missing ${needle}")
  endif()
endforeach()

file(READ "${SOURCE_DIR}/src/project_file.cpp" PROJECT_FILE)
foreach(needle
  "kProjectFormatVersion = 24"
  "menuButtonActions"
  "prohibitedUserOperations"
  "user_operations_json"
  "user_operations_from_json")
  string(FIND "${PROJECT_FILE}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "title navigation/UOP persistence regression: missing ${needle}")
  endif()
endforeach()

file(READ "${SOURCE_DIR}/src/gui.cpp" GUI)
foreach(needle
  "TitleNavigationDialog"
  "Navigation / UOP"
  "Default — return to Top/Main Menu"
  "User Operation Prohibitions"
  "RoleTitleMenuActions"
  "RoleTitleUopMask")
  string(FIND "${GUI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "title navigation/UOP GUI regression: missing ${needle}")
  endif()
endforeach()

file(READ "${SOURCE_DIR}/src/author.cpp" AUTHOR)
foreach(needle
  "dvd_chapter_dispatch_pre_commands"
  "title_menu_commands"
  "patch_dvd_title_uops"
  "dvd_uop_mask"
  "bluray_uop_mask"
  "title_menu_call_objects")
  string(FIND "${AUTHOR}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "title navigation/UOP author regression: missing ${needle}")
  endif()
endforeach()
string(FIND "${AUTHOR}" "g4 = 0; jump title 1;" broken_dvd_fallback)
if(NOT broken_dvd_fallback EQUAL -1)
  message(FATAL_ERROR "DVD title root menu still contains the historical title-1 restart fallback")
endif()

file(READ "${SOURCE_DIR}/src/hdmv.cpp" HDMV)
foreach(needle
  "MenuCallContextGpr=4092"
  "title_menu_call_objects"
  "add_playlist_user_operation_mask")
  string(FIND "${HDMV}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "title navigation/UOP HDMV regression: missing ${needle}")
  endif()
endforeach()

message(STATUS "per-title Menu-button/UOP source checks ok")
