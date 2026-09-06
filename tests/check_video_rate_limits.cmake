if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "SOURCE_DIR not supplied")
endif()
file(READ "${SOURCE_DIR}/include/bdmvauthor/model.hpp" MODEL)
file(READ "${SOURCE_DIR}/include/bdmvauthor/author.hpp" AUTHOR_H)
file(READ "${SOURCE_DIR}/src/author.cpp" AUTHOR)
file(READ "${SOURCE_DIR}/src/project_file.cpp" PROJECT)
file(READ "${SOURCE_DIR}/src/gui.cpp" GUI)
file(READ "${SOURCE_DIR}/src/cli.cpp" CLI)

foreach(needle
  "int video_min_bitrate_kbps = 0;"
  "int video_max_bitrate_kbps = 40000;"
  "e.video_max_bitrate_kbps = 86000;"
  "e.video_max_bitrate_kbps = 9800;"
  "video_codec_min_bitrate_kbps"
  "video_codec_max_bitrate_kbps")
  string(FIND "${MODEL}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "video rate-limit model regression: missing ${needle}")
  endif()
endforeach()

foreach(needle
  "kProjectFormatVersion = 24"
  "videoMinrateKbps"
  "videoMaxrateKbps")
  string(FIND "${PROJECT}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "video rate-limit project persistence regression: missing ${needle}")
  endif()
endforeach()

foreach(needle
  "Video minrate"
  "Video maxrate"
  "videoMinrateKbps"
  "videoMaxrateKbps"
  "configure_video_minrate_spin"
  "configure_video_maxrate_spin"
  "Allow exceeding format limits")
  string(FIND "${GUI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "video rate-limit GUI regression: missing ${needle}")
  endif()
endforeach()

foreach(needle
  "--video-minrate"
  "--video-maxrate"
  "--menu-video-minrate"
  "--menu-video-maxrate")
  string(FIND "${CLI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "video rate-limit CLI regression: missing ${needle}")
  endif()
endforeach()

foreach(needle
  "bool allow_exceeding_format_limits = false;"
  "int dvd_transport_bitrate_kbps = 10080;"
  "return allow_exceeding_format_limits ? configured")
  string(FIND "${AUTHOR_H}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "debug limit API regression: missing ${needle}")
  endif()
endforeach()

foreach(needle
  "-minrate \" << e.video_min_bitrate_kbps"
  "e.video_max_bitrate_kbps"
  "if (allow_exceeding_format_limits) return std::max(1, requested_maxrate_kbps);"
  "if (limits_.allow_exceeding_format_limits)"
  "return std::max(1, encoding.video_max_bitrate_kbps);"
  "audio-derived ceiling disabled"
  " -f 8 -r \" << std::max(1, mux_bitrate_kbps)"
  "bdmvauthor-video-cache-v14"
  "bdmvauthor-menu-video-cache-v5")
  string(FIND "${AUTHOR}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "video rate-limit authoring regression: missing ${needle}")
  endif()
endforeach()

# The debug gate must be session-only. It belongs in AuthoringLimits and the
# Debug menu, never in project serialization or persistent QSettings.
string(FIND "${PROJECT}" "allowExceedingFormatLimits" project_debug_pos)
if(NOT project_debug_pos EQUAL -1)
  message(FATAL_ERROR "debug allow-exceeding gate was accidentally persisted in project files")
endif()
string(FIND "${GUI}" "setValue(\"debugAllowExceed" settings_debug_pos)
if(NOT settings_debug_pos EQUAL -1)
  message(FATAL_ERROR "debug allow-exceeding gate was accidentally persisted in QSettings")
endif()

message(STATUS "structured video minrate/maxrate and debug bitrate-limit checks ok")
