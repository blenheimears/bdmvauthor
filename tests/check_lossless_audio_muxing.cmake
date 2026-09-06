file(READ "${SOURCE_DIR}/src/audio_tools.cpp" AUDIO_TOOLS)
file(READ "${SOURCE_DIR}/src/author.cpp" AUTHOR)
file(READ "${SOURCE_DIR}/third_party/tsmuxer/tsMuxer/lpcmStreamReader.cpp" LPCM)
file(READ "${SOURCE_DIR}/third_party/tsmuxer/tsMuxer/ac3StreamReader.cpp" AC3)

foreach(needle
  "Ac3BitratesKbps[]"
  "frmsizecod"
  "bitrate_index"
  "ac3_frames.emplace_back(pos, frame_bytes)"
  "experimental TrueHD AC-3 core must use 48 kHz AC-3")
  string(FIND "${AUDIO_TOOLS}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing variable-rate TrueHD AC-3 core marker: ${needle}")
  endif()
endforeach()
string(FIND "${AUDIO_TOOLS}" "constexpr std::size_t Ac3FrameBytes = 2560" stale_fixed_core)
if(NOT stale_fixed_core EQUAL -1)
  message(FATAL_ERROR "TrueHD core merger still assumes 2560-byte/640-kb/s AC-3 frames")
endif()

# Blu-ray LPCM start_flag is Track-Start metadata, not a generic first-frame
# marker.  The vendored muxer does not author the matching Track-Start EP-map
# entry, so it must leave this bit clear rather than triggering decoder resets.
foreach(needle
  "start_flag is not a generic"
  "m_tmpFrameBuffer[3] = (bits_per_sample << 6) & 0xff;")
  string(FIND "${LPCM}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing LPCM Track-Start compatibility fix: ${needle}")
  endif()
endforeach()
string(FIND "${LPCM}" "bits_per_sample << 6 | m_firstFrame << 5" stale_lpcm_start)
if(NOT stale_lpcm_start EQUAL -1)
  message(FATAL_ERROR "LPCM still sets start_flag merely for the first packet")
endif()

# TrueHD requires the Blu-ray extended audio PES extension/subtype signalling.
string(FIND "${AUTHOR}" "--new-audio-pes --start-time=" explicit_pes)
string(FIND "${AC3}" "if (m_useNewStyleAudioPES)" pes_ext)
string(FIND "${AC3}" "0x76;  // AC3 at TRUE-HD" truehd_core_subtype)
if(explicit_pes EQUAL -1 OR pes_ext EQUAL -1 OR truehd_core_subtype EQUAL -1)
  message(FATAL_ERROR "TrueHD Blu-ray extended-PES signalling regression")
endif()

message(STATUS "lossless Blu-ray audio muxing regression checks ok")
