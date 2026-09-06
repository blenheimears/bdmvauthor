file(READ "${SOURCE_DIR}/src/author.cpp" AUTHOR)
file(READ "${SOURCE_DIR}/include/bdmvauthor/author.hpp" AUTHOR_H)
file(READ "${SOURCE_DIR}/third_party/tsmuxer/tsMuxer/tsMuxer.cpp" TSMUXER)
file(READ "${SOURCE_DIR}/third_party/tsmuxer/tsMuxer/tsMuxer.h" TSMUXER_H)

foreach(needle
  "--new-audio-pes --start-time="
  "max_transport_bitrate_kbps"
  "s << \" --maxbitrate=\" << max_transport_bitrate_kbps << \" --vbv-len=950\""
  "finite transport pacing ceiling"
  "--minbitrate="
  "kInteractiveMenuTransportFloorKbps = 4000U"
  "const unsigned menu_transport_floor = kInteractiveMenuTransportFloorKbps"
  "Apply the read-ahead floor to both standard Blu-ray and UHD menus")
  string(FIND "${AUTHOR}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing Blu-ray transport-startup marker: ${needle}")
  endif()
endforeach()
foreach(needle
  "int bluray_transport_bitrate_kbps = 48000;"
  "int uhd_transport_bitrate_kbps = 70000;")
  string(FIND "${AUTHOR_H}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing default runtime transport limit: ${needle}")
  endif()
endforeach()

foreach(needle
  "DEFAULT_VBV_BUFFER_LEN = 500"
  "m_fixed_pcr_offset = m_timeOffset - m_vbvLen"
  "m_cbrBitrate != -1 && m_minBitrate != -1"
  "writeNullPackets(tsFrames)")
  string(FIND "${TSMUXER}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "unexpected bundled tsMuxer pacing implementation; missing: ${needle}")
  endif()
endforeach()

string(FIND "${TSMUXER_H}" "m_cbrBitrate = val" maxrate_setter)
if(maxrate_setter EQUAL -1)
  message(FATAL_ERROR "unexpected bundled tsMuxer max-bitrate implementation")
endif()

# Menu and title mux calls must both receive the session's effective transport
# limit. Only menus receive the separate 4 Mb/s read-ahead floor.
string(FIND "${AUTHOR}" "meta_header(static_cast<unsigned>(mi), {}, p.target, static_cast<unsigned>(combined_transport_limit_kbps), menu_transport_floor," menu_floor_call)
if(menu_floor_call EQUAL -1)
  message(FATAL_ERROR "Blu-ray/UHD menu mux does not receive effective transport limit and menu floor")
endif()
string(FIND "${AUTHOR}" "meta_header(playlist, resolved_title_chapters[i], p.target, static_cast<unsigned>(combined_transport_limit_kbps), 0U, title_clip)" title_call)
if(title_call EQUAL -1)
  message(FATAL_ERROR "title mux does not receive effective transport limit")
endif()

# BDMV Author must request Blu-ray's extended audio PES explicitly.
string(FIND "${TSMUXER}" "paramPair[0] == \"--new-audio-pes\"" parses_new_audio_pes)
if(parses_new_audio_pes EQUAL -1)
  message(FATAL_ERROR "bundled tsMuxer no longer parses --new-audio-pes")
endif()

message(STATUS "Blu-ray/UHD transport-startup pacing regression checks ok")
