if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "SOURCE_DIR not supplied")
endif()
file(READ "${SOURCE_DIR}/include/bdmvauthor/model.hpp" MODEL)
file(READ "${SOURCE_DIR}/include/bdmvauthor/author.hpp" AUTHOR_H)
file(READ "${SOURCE_DIR}/src/author.cpp" AUTHOR)
file(READ "${SOURCE_DIR}/src/audio_tools.cpp" AUDIO)

foreach(needle
  "derived_video_peak_limit_kbps"
  "aggregate_audio_peak_kbps"
  "Encoding audio track "
  "before video"
  "Measured encoded audio track "
  "video peak budget: "
  "peakBitrateLimitKbps="
  "bdmvauthor-video-cache-v14"
  "bdmvauthor-menu-video-cache-v5"
  "bdmvauthor-audio-cache-v7"
  "peakBitrateKbps"
  "combined_transport_limit_kbps"
  "max_transport_bitrate_kbps"
  "e.video_codec == VideoCodec::Hevc ? 86000 : 40000"
  "kUhdHevcVbvBufsizeKbits = 100000"
  "peak_bitrate_limit_kbps << \"k -bufsize \" << kUhdHevcVbvBufsizeKbits"
  "vbv-maxrate=\" << peak_bitrate_limit_kbps << \":vbv-bufsize=\" << kUhdHevcVbvBufsizeKbits"
  "k -maxrate \" << peak_bitrate_limit_kbps << \"k -bufsize 9781248")
  string(FIND "${AUTHOR}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "dynamic AV bitrate regression: missing author marker: ${needle}")
  endif()
endforeach()

# The normal 25/50 GB BD-R profile is intentionally conservative: 70 Mb/s
# combined UHD transport. The HEVC codec legality ceiling remains separate so
# a --debug session can raise/lower the transport budget without rewriting the
# project or codec rules.
string(FIND "${MODEL}" "case DiscTarget::UltraHdBluRay2160: return 70000;" UHD_COMBINED)
if(UHD_COMBINED EQUAL -1)
  message(FATAL_ERROR "UHD normal combined transport ceiling is not 70 Mb/s")
endif()
foreach(needle
  "int bluray_transport_bitrate_kbps = 48000;"
  "int uhd_transport_bitrate_kbps = 70000;"
  "combined_transport_bitrate_kbps")
  string(FIND "${AUTHOR_H}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "runtime transport-limit regression: missing ${needle}")
  endif()
endforeach()

# Audio preparation must happen before title video encoding so a TrueHD cache
# miss can measure its actual peak before the video cache key/encoder is chosen.
string(FIND "${AUTHOR}" "title_label << \"title \"" TITLE_START)
string(SUBSTRING "${AUTHOR}" ${TITLE_START} -1 TITLE_SECTION)
string(FIND "${TITLE_SECTION}" "encoded = encode_audio(tools_, title.source" AUDIO_ENCODE)
string(FIND "${TITLE_SECTION}" "video_cache_key(*title_fp" VIDEO_KEY)
string(FIND "${TITLE_SECTION}" "(void)encode_video(tools_, title.source" VIDEO_ENCODE)
if(AUDIO_ENCODE EQUAL -1 OR VIDEO_KEY EQUAL -1 OR VIDEO_ENCODE EQUAL -1 OR
   AUDIO_ENCODE GREATER VIDEO_KEY OR AUDIO_ENCODE GREATER VIDEO_ENCODE)
  message(FATAL_ERROR "title audio is not prepared before video cache selection/encoding")
endif()

# Menus use the same sequencing and derived budget model.
string(FIND "${AUTHOR}" "Prepare menu audio before video" MENU_AUDIO)
string(FIND "${AUTHOR}" "menu_video_cache_key(menu_background" MENU_VIDEO_KEY)
if(MENU_AUDIO EQUAL -1 OR MENU_VIDEO_KEY EQUAL -1 OR MENU_AUDIO GREATER MENU_VIDEO_KEY)
  message(FATAL_ERROR "menu audio is not prepared before menu video cache selection")
endif()

foreach(needle
  "int merge_truehd_ac3_core"
  "truehd_frames_per_second"
  "truehd_peak_window_bytes"
  "return truehd_peak_kbps + static_cast<int>(ac3_peak_kbps)")
  string(FIND "${AUDIO}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "TrueHD peak measurement regression: missing ${needle}")
  endif()
endforeach()

# UHD HEVC CPB size must remain fixed at the UHD ceiling rather than following
# the audio-derived/effective video peak budget.
foreach(stale
  "--vbv-bufsize \" << peak_bitrate_limit_kbps"
  "-bufsize \" << peak_bitrate_limit_kbps << \"k\"\n                        \" -profile:v main10"
  ":vbv-bufsize=\" << peak_bitrate_limit_kbps")
  string(FIND "${AUTHOR}" "${stale}" stale_pos)
  if(NOT stale_pos EQUAL -1)
    message(FATAL_ERROR "UHD HEVC VBV buffer is still coupled to video maxrate: ${stale}")
  endif()
endforeach()

message(STATUS "dynamic Blu-ray/UHD audio-first peak bitrate budgeting checks ok")

string(FIND "${AUTHOR}" "? 100000 : 40000" STALE_HEVC_VALIDATE)
if(NOT STALE_HEVC_VALIDATE EQUAL -1)
  message(FATAL_ERROR "generic encoding validation still permits the old 100 Mb/s HEVC ceiling")
endif()
