if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "SOURCE_DIR not supplied")
endif()
file(READ "${SOURCE_DIR}/src/author.cpp" AUTHOR)
file(READ "${SOURCE_DIR}/src/hdmv.cpp" HDMV)

foreach(needle
    "Looping menus use exactly one PlayItem"
    "real Play_PL restart gives the player a clean transport-clock epoch"
    "menu_main_clip, final_menu_still"
    "commands.push_back(move_gpr_from_psr(menu_selection_state_gpr(0),10))"
    "move_gpr_from_psr(menu_selection_state_gpr(i),10),goto_command(0)"
    "goto_command(play_command)")
  string(FIND "${AUTHOR}\n${HDMV}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "Blu-ray/UHD clean menu restart regression: missing ${needle}")
  endif()
endforeach()

string(FIND "${AUTHOR}" "--repeat-playitems=" repeat_option)
if(NOT repeat_option EQUAL -1)
  message(FATAL_ERROR "authoring path still exposes repeated PlayItems")
endif()
string(FIND "${AUTHOR}" "kMenuPlayItemRepeats" repeat_constant)
if(NOT repeat_constant EQUAL -1)
  message(FATAL_ERROR "legacy 999-PlayItem menu-loop constant is still present")
endif()

message(STATUS "Blu-ray/UHD menu loops use single PlayItems with MovieObject playlist restart")
