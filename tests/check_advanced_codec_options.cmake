file(READ "${SOURCE_DIR}/include/bdmvauthor/model.hpp" MODEL)
file(READ "${SOURCE_DIR}/src/author.cpp" AUTHOR)
file(READ "${SOURCE_DIR}/src/gui.cpp" GUI)
file(READ "${SOURCE_DIR}/src/cli.cpp" CLI)
file(READ "${SOURCE_DIR}/src/project_file.cpp" PROJECT)

foreach(needle
  "x264_advanced_options" "x265_advanced_options" "mpeg2_advanced_options"
  "ac3_advanced_options" "dca_advanced_options" "lpcm_advanced_options" "truehd_advanced_options"
  "parse_advanced_codec_options" "validate_advanced_codec_options" "maximum_keyframe_interval_for_rate"
  "controls a disc-spec parameter" "Blu-ray x264 bframes must be 0..3")
  string(FIND "${MODEL}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "advanced-codec model regression: missing ${needle}")
  endif()
endforeach()

foreach(needle
  "bdmvauthor-video-cache-v13" "bdmvauthor-audio-cache-v7" "bdmvauthor-dvd-audio-cache-v4"
  "ffmpeg_private_options" "x264_private_options" "x265_private_params" "advancedVideo=" "advancedAudio="
  "e.x264_advanced_options" "e.mpeg2_advanced_options" "e.lpcm_advanced_options"
  "e.dca_advanced_options" "resolved.truehd_advanced_options" "e.ac3_advanced_options")
  string(FIND "${AUTHOR}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "advanced-codec encoder regression: missing ${needle}")
  endif()
endforeach()

foreach(needle
  "Advanced codec options" "AdvancedCodecOptionsDialog" "edit_advanced_codec_options"
  "gui_keyframe_maximum" "configure_keyframe_interval_spin"
  "Legal explicit range for the current selection" "Advanced…")
  string(FIND "${GUI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "advanced-codec GUI/live-GOP regression: missing ${needle}")
  endif()
endforeach()

foreach(needle
  "--video-option NAME=VALUE" "--audio-option NAME=VALUE" "--clear-video-options"
  "--menu-video-option N=V" "--menu-audio-option N=V" "--menu-clear-video-options")
  string(FIND "${CLI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "advanced-codec CLI regression: missing ${needle}")
  endif()
endforeach()

foreach(needle
  "kProjectFormatVersion = 24" "x264AdvancedOptions" "x265AdvancedOptions"
  "mpeg2AdvancedOptions" "ac3AdvancedOptions" "dcaAdvancedOptions"
  "lpcmAdvancedOptions" "truehdAdvancedOptions")
  string(FIND "${PROJECT}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "advanced-codec persistence regression: missing ${needle}")
  endif()
endforeach()
message(STATUS "advanced codec options and live GOP source checks ok")
