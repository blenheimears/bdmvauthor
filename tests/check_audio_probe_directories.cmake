file(READ "${SOURCE_DIR}/src/author.cpp" A)

string(FIND "${A}" "int audio_channels(const ToolPaths& tools" START)
if(START EQUAL -1)
  message(FATAL_ERROR "audio channel probe helper not found")
endif()
string(SUBSTRING "${A}" ${START} 1800 HELPER)
string(FIND "${HELPER}" "fs::create_directories(tmp);" MKDIR)
string(FIND "${HELPER}" "run(c);" RUN)
if(MKDIR EQUAL -1)
  message(FATAL_ERROR "audio channel probe must create its output directory")
endif()
if(RUN EQUAL -1)
  message(FATAL_ERROR "audio channel probe run marker not found")
endif()
if(MKDIR GREATER RUN)
  message(FATAL_ERROR "audio channel probe directory is created only after ffprobe runs")
endif()

# These nested call sites reproduced the bug because their final directory did
# not otherwise exist before the helper's shell redirection was evaluated.
foreach(needle
    "md / \"menu-audio-channels\""
    "md / \"menu-audio-cache-channels\""
    "track_dir, source_track.source_ordinal)")
  string(FIND "${A}" "${needle}" POS)
  if(POS EQUAL -1)
    message(FATAL_ERROR "nested audio probe call-site regression: missing ${needle}")
  endif()
endforeach()
message(STATUS "audio probe directories are created before ffprobe redirection")
