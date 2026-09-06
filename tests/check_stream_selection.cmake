file(READ "${SOURCE_DIR}/include/bdmvauthor/model.hpp" MODEL)
file(READ "${SOURCE_DIR}/src/author.cpp" AUTHOR)
file(READ "${SOURCE_DIR}/src/hdmv.cpp" HDMV)
file(READ "${SOURCE_DIR}/src/gui.cpp" GUI)
file(READ "${SOURCE_DIR}/src/project_file.cpp" PROJECT_FILE)
file(READ "${SOURCE_DIR}/src/cli.cpp" CLI)

foreach(needle
    "AudioTrack, SubtitleTrack, SubtitleOff"
    "std::uint16_t target_stream = 1"
    "button_target_is_title_scoped"
    "default_audio_stream = 0"
    "default_subtitle_stream = -1"
)
  string(FIND "${MODEL}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing stream-selection model/default marker: ${needle}")
  endif()
endforeach()

foreach(needle
    "probe_source_track_catalog"
    "supported_subtitle_codec"
    "sourceAudioOrdinal="
    "prepare_subtitle"
    "S_HDMV/PGS"
    "S_TEXT/UTF8"
    "audio_passthrough"
    "target_stream > title_tracks"
)
  string(FIND "${AUTHOR}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing multi-stream authoring marker: ${needle}")
  endif()
endforeach()

foreach(needle
    "make_button_navigation_commands"
    "MenuButtonTargetKind::AudioTrack"
    "MenuButtonTargetKind::SubtitleTrack"
    "MenuButtonTargetKind::SubtitleOff"
    "set_stream_command"
    "move_gpr_from_psr"
    "make_menu_stream_indicator_plan"
    "title_audio_state_gpr"
    "title_subtitle_state_gpr"
)
  string(FIND "${HDMV}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing HDMV stream-selection marker: ${needle}")
  endif()
endforeach()

foreach(needle
    "Add audio/subtitle button"
    "stream_target_choices() const"
    "hdmv_pgs_subtitle"
    "Subtitles off"
    "Playback defaults"
    "Active option"
    "UDF Disc label"
    "ISO image (*.iso)"
)
  string(FIND "${GUI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing GUI stream/default marker: ${needle}")
  endif()
endforeach()

foreach(needle
    "audio-track"
    "subtitle-track"
    "subtitle-off"
    "targetStream"
    "defaultAudioStream"
    "defaultSubtitleStream"
)
  string(FIND "${PROJECT_FILE}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing stream-button project persistence marker: ${needle}")
  endif()
endforeach()

foreach(needle
    "--audio-button N S LABEL"
    "--subtitle-button N S LABEL"
    "--subtitles-off-button N LABEL"
    "--default-audio-stream"
    "--default-subtitle-stream"
    "UDF Disc label"
    "-o PATH"
)
  string(FIND "${CLI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing CLI stream/default marker: ${needle}")
  endif()
endforeach()
