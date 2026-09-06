if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "SOURCE_DIR not supplied")
endif()
file(READ "${SOURCE_DIR}/include/bdmvauthor/model.hpp" MODEL)
file(READ "${SOURCE_DIR}/include/bdmvauthor/hdmv.hpp" HDMV_H)
file(READ "${SOURCE_DIR}/src/hdmv.cpp" HDMV)
file(READ "${SOURCE_DIR}/src/author.cpp" AUTHOR)
file(READ "${SOURCE_DIR}/src/gui.cpp" GUI)
file(READ "${SOURCE_DIR}/src/project_file.cpp" PROJECT)
file(READ "${SOURCE_DIR}/src/cli.cpp" CLI)

foreach(needle
  "ButtonStateStyle active{{255,215,0,255}"
  "int default_audio_stream = 0"
  "int default_subtitle_stream = -1")
  string(FIND "${MODEL}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing active/default stream model marker: ${needle}")
  endif()
endforeach()

foreach(needle
  "struct TitleStreamDefaults"
  "struct MenuStreamIndicatorPlan"
  "title_audio_state_gpr"
  "title_subtitle_state_gpr"
  "make_menu_stream_indicator_plan"
  "make_title_stream_initialization_commands")
  string(FIND "${HDMV_H}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing HDMV active/default stream API marker: ${needle}")
  endif()
endforeach()

foreach(needle
  "indicator_bog_count=plan.variants.size()"
  "std::map<std::pair<StreamIndicatorKind,std::uint16_t>"
  "enable_button_command"
  "disable_button_command"
  "active_indicator_switch_commands"
  "disable every conflicting"
  "active_indicator_geometry"
  "active_indicator_bounds"
  "every persistent active-stream indicator in its own fixed-position"
  "Explicitly clear every variant first"
  "Transparent initialization BOG"
  "apply_title_stream_state_commands"
  "title_audio_state_gpr(action.target_title)"
  "title_subtitle_state_gpr(action.target_title)")
  string(FIND "${HDMV}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing HDMV per-title indicator/state marker: ${needle}")
  endif()
endforeach()

foreach(needle
  "default audio selection is unavailable"
  "default subtitle selection is unavailable"
  "stream_defaults.push_back({title.default_audio_stream,title.default_subtitle_stream})"
  "dvd_title_stream_pre_commands"
  "if (g2 == 0) audio = "
  "if (g3 == 0) subtitle = 62"
  "authored_title.default_audio_stream"
  "authored_title.default_subtitle_stream")
  string(FIND "${AUTHOR}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing authoring default-stream marker: ${needle}")
  endif()
endforeach()

foreach(needle
  "Playback defaults"
  "Default audio"
  "Default subtitles"
  "Player default"
  "Subtitles off"
  "Active option"
  "RoleDefaultAudioStream"
  "RoleDefaultSubtitleStream")
  string(FIND "${GUI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing GUI active/default stream control: ${needle}")
  endif()
endforeach()

foreach(needle
  "kProjectFormatVersion = 24"
  "\"active\", state_style_json(s.active)"
  "defaultAudioStream"
  "defaultSubtitleStream")
  string(FIND "${PROJECT}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing project v18 active/default persistence marker: ${needle}")
  endif()
endforeach()

foreach(needle
  "--default-audio-stream"
  "--default-subtitle-stream"
  "--menu-button-active-text"
  "--menu-button-active-border"
  "--button-active-text"
  "--button-active-border"
  "if (which == \"active\") return s.active"
  "{\"normal\", \"active\", \"selected\", \"activated\"}")
  string(FIND "${CLI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing CLI active/default stream marker: ${needle}")
  endif()
endforeach()


foreach(forbidden
  "--menu-button-active-bg"
  "--button-active-bg")
  string(FIND "${CLI}" "${forbidden}" pos)
  if(NOT pos EQUAL -1)
    message(FATAL_ERROR "active-option overlay must not expose an opaque background CLI control: ${forbidden}")
  endif()
endforeach()

message(STATUS "per-title active/default stream source checks ok")
