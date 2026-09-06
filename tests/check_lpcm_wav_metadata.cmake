file(READ "${SOURCE_DIR}/third_party/tsmuxer/tsMuxer/lpcmStreamReader.cpp" LPCM)
foreach(needle
  "Walk RIFF chunks instead of looking for"
  "memcmp(chunk, \"fmt \", 4) == 0"
  "memcmp(chunk, \"data\", 4) == 0"
  "paddedChunkSize"
  "audioData = chunkData"
  "MAX_HEADER_SIZE = DEFAULT_FILE_BLOCK_SIZE")
  string(FIND "${LPCM}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing metadata-heavy WAV LPCM fix marker: ${needle}")
  endif()
endforeach()
string(FIND "${LPCM}" "static constexpr int MAX_HEADER_SIZE = 192" stale_limit)
if(NOT stale_limit EQUAL -1)
  message(FATAL_ERROR "LPCM WAV parser still has the old 192-byte header search limit")
endif()
message(STATUS "LPCM WAV metadata/header regression checks ok")
