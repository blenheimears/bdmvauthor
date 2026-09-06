file(READ "${SOURCE_DIR}/include/bdmvauthor/model.hpp" MODEL)
file(READ "${SOURCE_DIR}/src/author.cpp" AUTHOR)
file(READ "${SOURCE_DIR}/src/project_file.cpp" PROJECT_FILE)
file(READ "${SOURCE_DIR}/src/gui.cpp" GUI)
file(READ "${SOURCE_DIR}/src/cli.cpp" CLI)

foreach(needle
    "std::filesystem::path background_video"
)
  string(FIND "${MODEL}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing video-background model marker: ${needle}")
  endif()
endforeach()

foreach(needle
    "effective.background_video = parent->background_video"
    "missing background video for"
    "prepare_menu_overlay"
    "-stream_loop -1"
    "motion_background = !menu.background_video.empty()"
    "motion_background ? video_menu.background_video : prepare_menu_background"
    "menu.background_color, motion_background && menu.loop_media, menu_overlay, motion_background && !menu.loop_media"
)
  string(FIND "${AUTHOR}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing video-background authoring marker: ${needle}")
  endif()
endforeach()

foreach(needle
    "backgroundVideo"
    "background video"
    "role.contains(\"video\""
)
  string(FIND "${PROJECT_FILE}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing video-background project marker: ${needle}")
  endif()
endforeach()

foreach(needle
    "bg_type_->addItems({\"Still image\",\"Video\"})"
    "Menu background video"
    "m->background_video"
    "Video background\\n%1"
)
  string(FIND "${GUI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing video-background GUI marker: ${needle}")
  endif()
endforeach()

foreach(needle
    "--menu-background-video FILE"
    "m.background_video=value()"
    "m.background_image.clear()"
)
  string(FIND "${CLI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing video-background CLI marker: ${needle}")
  endif()
endforeach()
