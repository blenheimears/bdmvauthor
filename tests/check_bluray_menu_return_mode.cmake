file(READ "${SOURCE_DIR}/src/hdmv.cpp" HDMV)
foreach(needle
  "constexpr std::uint16_t ReturnDispatchGpr=2092"
  "commands.push_back(move_gpr_immediate(ReturnDispatchGpr,1))"
  "commands.push_back(jump_title(0))"
  "commands.push_back(eq_gpr_immediate(ReturnDispatchGpr,1))"
  "commands.push_back(move_gpr_immediate(ReturnDispatchGpr,0))"
  "directly to a menu MovieObject leaves players in numbered-title"
  "re-enter the interactive Top Menu title (title 0)")
  string(FIND "${HDMV}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing Blu-ray menu-return title-context marker: ${needle}")
  endif()
endforeach()
message(STATUS "Blu-ray natural title return re-enters interactive Top Menu title before submenu routing")
