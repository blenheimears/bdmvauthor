if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "SOURCE_DIR not supplied")
endif()

file(READ "${SOURCE_DIR}/include/bdmvauthor/model.hpp" MODEL)
foreach(needle
  "project_has_menu"
  "default_menuless_startup_actions"
  "is_default_menuless_startup_sequence"
  "every title in order exactly once, then stop")
  string(FIND "${MODEL}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing menuless model marker: ${needle}")
  endif()
endforeach()

file(READ "${SOURCE_DIR}/src/project_file.cpp" PROJECT)
foreach(needle "kProjectFormatVersion = 24" "hasMenu" "!o.value(\"hasMenu\").toBool(true)")
  string(FIND "${PROJECT}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing menuless persistence marker: ${needle}")
  endif()
endforeach()

file(READ "${SOURCE_DIR}/src/gui.cpp" GUI)
foreach(needle
  "Automatically add a menu button for each video"
  "add_button->setChecked(true)"
  "DontUseNativeDialog"
  "Remove menu"
  "Add main menu"
  "root_menu_.id.empty()||current_menu_id_.empty()"
  "Main menu removed. By default all titles play in order and playback stops at the end."
  "default_menuless_startup_actions"
  "Menu-less default: play all titles in order, then stop.")
  string(FIND "${GUI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing menuless GUI marker: ${needle}")
  endif()
endforeach()

file(READ "${SOURCE_DIR}/src/author.cpp" AUTHOR)
foreach(needle
  "if (root.id.empty()) return menus"
  "default_menuless_startup_actions(p.titles.size())"
  "else out << \"exit;\""
  "const auto hdmv_menu_object_count = menus.empty() ? 1U : menus.size()")
  string(FIND "${AUTHOR}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing menuless author marker: ${needle}")
  endif()
endforeach()

file(READ "${SOURCE_DIR}/src/hdmv.cpp" HDMV)
foreach(needle
  "menu_count == 0U ? 1U : menu_count"
  "reserves MovieObject 0 as an empty terminal object"
  "if(menu_call_dispatch.empty())objs.push_back({false,false,false,{}})")
  string(FIND "${HDMV}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing menuless HDMV marker: ${needle}")
  endif()
endforeach()

file(READ "${SOURCE_DIR}/src/cli.cpp" CLI)
string(FIND "${CLI}" "--no-menu" cli_pos)
if(cli_pos EQUAL -1)
  message(FATAL_ERROR "missing --no-menu CLI option")
endif()
message(STATUS "menuless source checks ok")
