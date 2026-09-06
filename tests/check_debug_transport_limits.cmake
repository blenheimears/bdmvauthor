if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "SOURCE_DIR not supplied")
endif()
file(READ "${SOURCE_DIR}/src/gui.cpp" GUI)
file(READ "${SOURCE_DIR}/include/bdmvauthor/author.hpp" AUTHOR_H)
file(READ "${SOURCE_DIR}/src/author.cpp" AUTHOR)

foreach(needle
  "std::string_view(argv[i])==\"--debug\""
  "explicit Window(bool debug_mode=false):debug_mode_(debug_mode)"
  "menuBar()->addMenu(\"&Debug\")"
  "Transport bitrate &limits…"
  "These debug overrides apply only to this --debug session"
  "authoring_limits_.bluray_transport_bitrate_kbps=bd->value()"
  "authoring_limits_.uhd_transport_bitrate_kbps=uhd->value()"
  "authoring_limits_.dvd_transport_bitrate_kbps=dvd->value()"
  "Allow exceeding format limits"
  "authoring_limits_.allow_exceeding_format_limits"
  "auto limits=authoring_limits_"
  "AuthorEngine e(tools,limits)")
  string(FIND "${GUI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "debug transport settings regression: missing GUI marker: ${needle}")
  endif()
endforeach()
foreach(needle
  "AuthoringLimits"
  "int bluray_transport_bitrate_kbps = 48000;"
  "int uhd_transport_bitrate_kbps = 70000;"
  "int dvd_transport_bitrate_kbps = 10080;"
  "bool allow_exceeding_format_limits = false;"
  "AuthorEngine(ToolPaths tools = {}, AuthoringLimits limits = {})")
  string(FIND "${AUTHOR_H}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "debug transport settings regression: missing API marker: ${needle}")
  endif()
endforeach()
foreach(needle
  "limits_.combined_transport_bitrate_kbps(p.target)"
  "combined_transport_limit_kbps"
  "peakBitrateLimitKbps="
  "audio-derived ceiling disabled"
  " -f 8 -r ")
  string(FIND "${AUTHOR}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "debug transport settings regression: effective limit not propagated: ${needle}")
  endif()
endforeach()
# The debug fields must not be persisted. A source-level guard makes accidental
# addition to QSettings/project serialization fail this regression.
string(FIND "${GUI}" "setValue(\"debug" persisted_debug_setting)
if(NOT persisted_debug_setting EQUAL -1)
  message(FATAL_ERROR "debug transport settings were accidentally made persistent")
endif()
message(STATUS "session-only debug transport-limit checks ok")
