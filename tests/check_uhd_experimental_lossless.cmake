if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "SOURCE_DIR not supplied")
endif()
file(READ "${SOURCE_DIR}/src/author.cpp" AUTHOR)
file(READ "${SOURCE_DIR}/include/bdmvauthor/model.hpp" MODEL)
file(READ "${SOURCE_DIR}/src/gui.cpp" GUI)
file(READ "${SOURCE_DIR}/src/cli.cpp" CLI)
file(READ "${SOURCE_DIR}/src/audio_tools.cpp" AUDIO)
foreach(needle
  "std::array<TimingCandidate,3>{kUhdTimingModes[0],kUhdTimingModes[1],kUhdTimingModes[2]}"
  "50p (experimental — explicit only)"
  "59.94p (experimental — explicit only)"
  "60p (experimental — explicit only)")
  string(FIND "${AUTHOR}${GUI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "UHD experimental-rate regression: missing ${needle}")
  endif()
endforeach()
foreach(needle
  "lpcm_sample_rate_hz = 0"
  "lpcm_bit_depth = 0"
  "resolved_lossless_sample_rate_hz"
  "resolved_lossless_bit_depth"
  "e.lpcm_sample_rate_hz = 48000"
  "e.lpcm_bit_depth = 16")
  string(FIND "${MODEL}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "lossless default regression: missing ${needle}")
  endif()
endforeach()
foreach(needle
  "lossless_audio_codec(resolved.audio_codec)"
  "-sample_fmt "
  "resolved.lpcm_sample_rate_hz"
  "resolved.lpcm_bit_depth")
  string(FIND "${AUTHOR}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "lossless encode regression: missing ${needle}")
  endif()
endforeach()
foreach(needle
  "--audio-sample-rate HZ"
  "--audio-bit-depth BITS"
  "--audio-stream-sample-rate S HZ"
  "--audio-stream-bit-depth S BITS"
  "--menu-audio-sample-rate HZ"
  "--menu-audio-bit-depth BITS")
  string(FIND "${CLI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "lossless CLI regression: missing ${needle}")
  endif()
endforeach()
foreach(needle
  "Lossless sample rate"
  "Lossless bit depth"
  "Auto — match source / round up"
  "Lossless rate"
  "Lossless depth")
  string(FIND "${GUI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "lossless GUI regression: missing ${needle}")
  endif()
endforeach()
foreach(needle "samples_per_truehd_frame = 40U << ratebits" "rate_multiplier = samples_per_truehd_frame / 40U")
  string(FIND "${AUDIO}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "TrueHD multi-rate merger regression: missing ${needle}")
  endif()
endforeach()
message(STATUS "UHD experimental-rate and lossless-format checks ok")
