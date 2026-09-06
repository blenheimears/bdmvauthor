file(READ "${SOURCE_DIR}/src/author.cpp" AUTHOR)
file(READ "${SOURCE_DIR}/src/gui.cpp" GUI)
file(READ "${SOURCE_DIR}/src/cli.cpp" CLI)

foreach(needle
    "probe_stream_duration_seconds"
    "probe_stream_present"
    "menu.duration_seconds = std::max(video_duration, audio_duration)"
    "fs::path chosen_audio = menu.audio_source"
    "chosen_audio = menu.background_video"
    "menu.audio_source = std::move(chosen_audio)"
    "still background without menu audio")
  string(FIND "${AUTHOR}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "menu auto-duration authoring regression: missing ${needle}")
  endif()
endforeach()
foreach(needle
    "probe_gui_stream_duration"
    "automatic_menu_duration"
    "s (automatic)"
    "A separate menu-audio file overrides audio embedded in the background video")
  string(FIND "${GUI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "menu auto-duration GUI regression: missing ${needle}")
  endif()
endforeach()
foreach(needle
    "Manual duration for still-image menus with no audio"
    "Explicit menu audio; overrides background-video audio")
  string(FIND "${CLI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "menu auto-duration CLI regression: missing ${needle}")
  endif()
endforeach()
