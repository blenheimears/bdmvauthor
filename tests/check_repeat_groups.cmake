if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "SOURCE_DIR not supplied")
endif()
file(READ "${SOURCE_DIR}/include/bdmvauthor/model.hpp" MODEL)
foreach(needle
  "RepeatBegin, RepeatEnd"
  "ExpandedNavigationSequence"
  "expand_navigation_repeat_groups"
  "nested repeat groups are not supported"
  "an infinitely repeated group must be final"
  "an infinitely repeated group must contain at least one Play Title action")
  string(FIND "${MODEL}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing repeat-group model marker: ${needle}")
  endif()
endforeach()

file(READ "${SOURCE_DIR}/src/hdmv.cpp" HDMV)
foreach(needle
  "expanded.has_infinite_loop()"
  "infinite_loop_command"
  "commands.push_back(goto_command(infinite_loop_command))"
  "repeat group must resume after every title")
  string(FIND "${HDMV}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing HDMV repeat-group marker: ${needle}")
  endif()
endforeach()

file(READ "${SOURCE_DIR}/src/author.cpp" AUTHOR)
foreach(needle
  "const auto expanded = expand_navigation_repeat_groups(source)"
  "const std::size_t loop_id = loop_index + 1U"
  "continuations_[loop_index].commands = compile_from(body"
  "end_continuation")
  string(FIND "${AUTHOR}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing DVD repeat-group marker: ${needle}")
  endif()
endforeach()

file(READ "${SOURCE_DIR}/src/project_file.cpp" PROJECT)
foreach(needle "repeat-begin" "repeat-end" "kProjectFormatVersion = 24")
  string(FIND "${PROJECT}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing repeat-group persistence marker: ${needle}")
  endif()
endforeach()

file(READ "${SOURCE_DIR}/src/gui.cpp" GUI)
foreach(needle
  "Repeat group…"
  "First action number"
  "Last action number"
  "Remove / ungroup"
  "Nested groups and menu jumps inside groups are not supported")
  string(FIND "${GUI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing repeat-group GUI marker: ${needle}")
  endif()
endforeach()

file(READ "${SOURCE_DIR}/src/cli.cpp" CLI)
foreach(needle
  "--button-repeat-group-begin N"
  "--button-repeat-group-end"
  "--first-play-repeat-group-begin N"
  "--first-play-repeat-group-end")
  string(FIND "${CLI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing repeat-group CLI marker: ${needle}")
  endif()
endforeach()
message(STATUS "repeat-group source checks ok")
