file(READ "${SOURCE_DIR}/include/bdmvauthor/model.hpp" MODEL)
file(READ "${SOURCE_DIR}/include/bdmvauthor/author.hpp" AUTHOR_H)
file(READ "${SOURCE_DIR}/src/author.cpp" AUTHOR)
file(READ "${SOURCE_DIR}/src/compliance.cpp" COMPLIANCE)
file(READ "${SOURCE_DIR}/src/project_file.cpp" PROJECT_FILE)
file(READ "${SOURCE_DIR}/src/gui.cpp" GUI)
file(READ "${SOURCE_DIR}/src/cli.cpp" CLI)

foreach(marker
  "Avchd = 3"
  "default_avchd_encoding_profile"
  "disc_avchd_capacity_bytes"
  "title_avchd_encoding"
  "avchd_encoding"
  "target_is_avchd"
  "case DiscTarget::Avchd"
)
  string(FIND "${MODEL}" "${marker}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "AVCHD model regression: missing ${marker}")
  endif()
endforeach()

foreach(marker
  "avchd_transport_bitrate_kbps = 18000"
  "case DiscTarget::Avchd: return avchd_transport_bitrate_kbps"
)
  string(FIND "${AUTHOR_H}" "${marker}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "AVCHD transport-limit regression: missing ${marker}")
  endif()
endforeach()

foreach(marker
  "--avchd"
  "AVCHD export currently requires exactly one title"
  "AVCHD export currently requires a menuless project"
  "AVCHD export does not support custom prohibited-user-operation masks"
  "AVCHD audio must use Dolby Digital AC-3 or Linear PCM"
  "AVCHD Linear PCM must use 16-bit samples"
  "require_v0100"
  "\"INDX\""
  "\"MOBJ\""
  "\"MPLS\""
  "\"0100\""
  "Validating native AVCHD 1.0 navigation"
  "o.grafts.push_back({disc, \"\"})"
)
  string(FIND "${AUTHOR}" "${marker}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "AVCHD authoring regression: missing ${marker}")
  endif()
endforeach()

foreach(marker
  "target == DiscTarget::Avchd"
  "AVCHD"
  "48 kHz"
  "7.1 channels"
)
  string(FIND "${COMPLIANCE}" "${marker}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "AVCHD compliance regression: missing ${marker}")
  endif()
endforeach()

foreach(marker
  "\"avchd\""
  "avchdEncoding"
  "avchdFrameRate"
  "avchdResolution"
  "avchdAspectRatio"
  "kProjectFormatVersion = 29"
)
  string(FIND "${PROJECT_FILE}" "${marker}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "AVCHD project persistence regression: missing ${marker}")
  endif()
endforeach()

foreach(marker
  "AVCHD (DVD media)"
  "4.7 GB (single-layer DVD)"
  "8.5 GB (dual-layer DVD)"
  "New titles — AVCHD"
  "AVCHD combined transport limit"
  "Select exactly one video for an AVCHD project"
)
  string(FIND "${GUI}" "${marker}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "AVCHD GUI regression: missing ${marker}")
  endif()
endforeach()

foreach(marker
  "s == \"avchd\""
  "DiscTarget::Avchd"
  "uhd-bluray-2160, avchd, or dvd-video-480p"
)
  string(FIND "${CLI}" "${marker}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "AVCHD CLI regression: missing ${marker}")
  endif()
endforeach()
