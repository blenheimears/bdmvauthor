file(READ "${SOURCE_DIR}/src/author.cpp" AUTHOR)
file(READ "${SOURCE_DIR}/src/compliance.cpp" COMPLIANCE)

foreach(NEEDED
    "parse_h264_hrd_stats"
    "bitrate_scale = static_cast<int>"
    "probe_audio_elementary_bitrate_sampled"
    "probe_dts_core_bitrate_sampled"
    "sampled_average_bitrate_bps"
    "-c:a copy -f dts pipe:1"
    "info.reported_bitrate_bps"
    "if (info.codec_name == \"dts\")"
)
  string(FIND "${AUTHOR}" "${NEEDED}" POS)
  if(POS EQUAL -1)
    message(FATAL_ERROR "missing corrected author bitrate compliance marker: ${NEEDED}")
  endif()
endforeach()

foreach(NEEDED
    "dts_core_actual_bitrate_from_bytes"
    "frame_bytes"
    "pcm_samples"
)
  string(FIND "${COMPLIANCE}" "${NEEDED}" POS)
  if(POS EQUAL -1)
    message(FATAL_ERROR "missing DTS core header-rate marker: ${NEEDED}")
  endif()
endforeach()

foreach(BAD_RATE
    "probe_audio_packet_bitrate_sampled"
    "video sampled packet rate exceeds"
)
  string(FIND "${AUTHOR}${COMPLIANCE}" "${BAD_RATE}" POS)
  if(NOT POS EQUAL -1)
    message(FATAL_ERROR "old packet-burst bitrate compliance logic returned: ${BAD_RATE}")
  endif()
endforeach()

foreach(BAD
    "const auto scale = *std::max_element(br_scales.begin(), br_scales.end())"
    "const auto scale = *std::max_element(cpb_scales.begin(), cpb_scales.end())"
)
  string(FIND "${AUTHOR}" "${BAD}" POS)
  if(NOT POS EQUAL -1)
    message(FATAL_ERROR "cross-paired H.264 HRD scale/value logic returned: ${BAD}")
  endif()
endforeach()

foreach(NEEDED
    "DTS core frame-header actual bitrate"
    "DTS-HD sampled raw elementary average bitrate"
    "H.264 HRD bitrate "
)
  string(FIND "${COMPLIANCE}" "${NEEDED}" POS)
  if(POS EQUAL -1)
    message(FATAL_ERROR "missing diagnostic bitrate message: ${NEEDED}")
  endif()
endforeach()
