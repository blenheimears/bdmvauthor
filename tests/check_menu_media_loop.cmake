file(READ "${SOURCE_DIR}/include/bdmvauthor/model.hpp" MODEL)
file(READ "${SOURCE_DIR}/src/author.cpp" AUTHOR)
file(READ "${SOURCE_DIR}/src/gui.cpp" GUI)
file(READ "${SOURCE_DIR}/src/cli.cpp" CLI)
file(READ "${SOURCE_DIR}/src/project_file.cpp" PROJECT)
file(READ "${SOURCE_DIR}/third_party/tsmuxer/tsMuxer/main.cpp" TSMUX_MAIN)
file(READ "${SOURCE_DIR}/third_party/tsmuxer/tsMuxer/tsPacket.cpp" TSMUX_MPLS)

foreach(needle
    "bool loop_media = true"
    "loopMedia"
    "--menu-loop"
    "--no-menu-loop"
    "Loop menu audio/video"
    "kProjectFormatVersion = 24")
  set(found FALSE)
  foreach(text "${MODEL}" "${PROJECT}" "${CLI}" "${GUI}")
    string(FIND "${text}" "${needle}" pos)
    if(NOT pos EQUAL -1)
      set(found TRUE)
    endif()
  endforeach()
  if(NOT found)
    message(FATAL_ERROR "menu media-loop model/UI regression: missing ${needle}")
  endif()
endforeach()

foreach(needle
    "const bool has_menu_media"
    "const bool loop_menu_media"
    "if (loop_menu_media) xml << \"<post>jump cell 1;</post>"
    "!loop_menu_media ? \" pause=\\\"inf\\\"\""
    "motion && menu.loop_media"
    "motion && !menu.loop_media"
    "motion_background && menu.loop_media"
    "final_menu_still"
    "Looping menus use exactly one PlayItem"
    "--final-still"
    "mediaLoop="
    "-af apad -t ")
  string(FIND "${AUTHOR}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "menu media-loop authoring regression: missing ${needle}")
  endif()
endforeach()

string(FIND "${TSMUX_MAIN}" "--final-still" pos)
if(pos EQUAL -1)
  message(FATAL_ERROR "tsMuxer final-still option regression")
endif()
foreach(needle "m_finalStill" "still_mode: 2 = infinite")
  string(FIND "${TSMUX_MPLS}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "tsMuxer infinite-still MPLS regression: missing ${needle}")
  endif()
endforeach()
