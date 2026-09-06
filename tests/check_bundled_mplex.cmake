if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "SOURCE_DIR not supplied")
endif()

file(READ "${SOURCE_DIR}/CMakeLists.txt" TOP_CMAKE)
file(READ "${SOURCE_DIR}/third_party/mplex/CMakeLists.txt" MPLEX_CMAKE)
file(READ "${SOURCE_DIR}/third_party/mplex/BDMVAUTHOR-VENDORING.md" VENDORING)
file(READ "${SOURCE_DIR}/third_party/mplex/config.h.cmake" MPLEX_CONFIG)
file(READ "${SOURCE_DIR}/windows/build-msys2-ucrt64.sh" WINDOWS_BUILD)

foreach(path
  "third_party/mplex/COPYING"
  "third_party/mplex/mplex/main.cpp"
  "third_party/mplex/utils/mjpeg_logging.c"
  "third_party/mplex/utils/mpegconsts.c"
  "third_party/mplex/utils/yuv4mpeg_ratio.c")
  if(NOT EXISTS "${SOURCE_DIR}/${path}")
    message(FATAL_ERROR "missing vendored mplex source: ${path}")
  endif()
endforeach()

# 0.1.108 deliberately carries only the mplex build subset, not the full
# mjpegtools source snapshot or unused support translation units.
if(EXISTS "${SOURCE_DIR}/third_party/mjpegtools")
  message(FATAL_ERROR "full mjpegtools source tree must not be vendored")
endif()
foreach(path
  "third_party/mplex/aenc"
  "third_party/mplex/lavtools"
  "third_party/mplex/mpeg2enc"
  "third_party/mplex/y4mdenoise"
  "third_party/mplex/y4mscaler"
  "third_party/mplex/yuvscaler"
  "third_party/mplex/utils/yuv4mpeg.c"
  "third_party/mplex/mplex/Makefile.am"
  "third_party/mplex/mplex/Makefile.in")
  if(EXISTS "${SOURCE_DIR}/${path}")
    message(FATAL_ERROR "unrelated/unused mjpegtools source was retained: ${path}")
  endif()
endforeach()

file(GLOB MPLEX_TOP RELATIVE "${SOURCE_DIR}/third_party/mplex" "${SOURCE_DIR}/third_party/mplex/*")
foreach(entry IN LISTS MPLEX_TOP)
  if(NOT entry STREQUAL "BDMVAUTHOR-VENDORING.md" AND
     NOT entry STREQUAL "CMakeLists.txt" AND
     NOT entry STREQUAL "COPYING" AND
     NOT entry STREQUAL "config.h.cmake" AND
     NOT entry STREQUAL "mplex" AND
     NOT entry STREQUAL "utils")
    message(FATAL_ERROR "unexpected top-level file/directory in minimal mplex subset: ${entry}")
  endif()
endforeach()

foreach(needle
  "BDMVAUTHOR_BUILD_BUNDLED_MPLEX"
  "add_subdirectory(third_party/mplex"
  "add_executable(mplex"
  "utils/yuv4mpeg_ratio.c"
  "RUNTIME_OUTPUT_DIRECTORY"
  "HAVE_CONFIG_H=1")
  string(FIND "${TOP_CMAKE}\n${MPLEX_CMAKE}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "bundled mplex build marker missing: ${needle}")
  endif()
endforeach()

foreach(needle
  "# define strcasecmp _stricmp"
  "# define strncasecmp _strnicmp")
  string(FIND "${MPLEX_CONFIG}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "native-Windows mplex compatibility marker missing: ${needle}")
  endif()
endforeach()

foreach(needle
  "SVN r3517"
  "only the source required to build `mplex`"
  "18cf570556b34112dbc69a541dea24894c2e614897111ea7094ede1b4f9c0fc4")
  string(FIND "${VENDORING}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "mplex provenance/subset marker missing: ${needle}")
  endif()
endforeach()

foreach(needle
  "-DBDMVAUTHOR_BUILD_BUNDLED_MPLEX=ON"
  "mplex.exe"
  "mingw-w64-ucrt-x86_64-ffmpeg"
  "ffmpeg.exe"
  "ffprobe.exe")
  string(FIND "${WINDOWS_BUILD}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "Windows bundled-tool marker missing: ${needle}")
  endif()
endforeach()

if("${WINDOWS_BUILD}" MATCHES "third_party-source")
  message(FATAL_ERROR "Windows binary staging must not copy third-party source trees")
endif()
