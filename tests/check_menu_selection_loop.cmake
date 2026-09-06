if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "SOURCE_DIR not supplied")
endif()
file(READ "${SOURCE_DIR}/include/bdmvauthor/hdmv.hpp" HDMV_H)
file(READ "${SOURCE_DIR}/src/hdmv.cpp" HDMV)
file(READ "${SOURCE_DIR}/src/author.cpp" AUTHOR)
foreach(needle
  "menu_selection_state_gpr"
  "PSR10 (currently selected ordinary button)"
  "move_gpr_from_psr(menu_selection_state_gpr(0),10)"
  "move_gpr_from_psr(menu_selection_state_gpr(i),10)"
  "goto_command(0)"
  "goto_command(play_command)"
  "set_button_page_command_indirect"
  "selection_state_gpr,0"
  "indicators.variants.empty()?0xffffU"
  "honor PSR10"
  "restores the saved ordinary selection")
  string(FIND "${HDMV_H}\n${HDMV}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing Blu-ray menu-loop selection preservation marker: ${needle}")
  endif()
endforeach()
string(FIND "${AUTHOR}" "hdmv::menu_selection_state_gpr(static_cast<std::uint16_t>(mi))" pos)
if(pos EQUAL -1)
  message(FATAL_ERROR "authoring path does not assign a distinct selection-state GPR to each menu")
endif()
