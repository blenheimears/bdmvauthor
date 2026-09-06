file(READ "${SOURCE_DIR}/include/bdmvauthor/compliance.hpp" COMPLIANCE_H)
file(READ "${SOURCE_DIR}/include/bdmvauthor/model.hpp" MODEL)
file(READ "${SOURCE_DIR}/src/compliance.cpp" COMPLIANCE)
file(READ "${SOURCE_DIR}/src/author.cpp" AUTHOR)
file(READ "${SOURCE_DIR}/src/gui.cpp" GUI)
file(READ "${SOURCE_DIR}/src/cli.cpp" CLI)
file(READ "${SOURCE_DIR}/src/project_file.cpp" PROJECT)

foreach(NEEDED
    "struct VideoStreamInfo"
    "h264_hrd_max_cpb_bits"
    "h264_frame_mbs_only_flag"
    "h264_mb_adaptive_frame_field_flag"
    "mpeg2_vbv_bits"
    "evaluate_blu_ray_video"
    "evaluate_blu_ray_audio"
)
  string(FIND "${COMPLIANCE_H}" "${NEEDED}" POS)
  if(POS EQUAL -1)
    message(FATAL_ERROR "missing passthrough compliance API marker: ${NEEDED}")
  endif()
endforeach()

foreach(NEEDED
    "kPrimaryVideoMaxBitrate = 40'000'000.0"
    "kAvcMaxCpbBits = 30'000'000.0"
    "kMpeg2MaxVbvBits = 9'781'248.0"
    "h264_max_frame_mbs"
    "h264_max_mbps"
    "H.264 random-access interval exceeds one second"
    "MPEG-2 GOP/random-access interval exceeds one second in a sampled window"
    "Main Profile the chroma_format_idc"
    "h264_frame_mbs_only_flag == 1"
    "VC-1 must use Advanced Profile"
    "Dolby Digital Plus"
    "DTS-HD"
)
  string(FIND "${COMPLIANCE}" "${NEEDED}" POS)
  if(POS EQUAL -1)
    message(FATAL_ERROR "missing Blu-ray compliance rule marker: ${NEEDED}")
  endif()
endforeach()

foreach(NEEDED
    "trace_headers"
    "frame_mbs_only_flag"
    "probe_video_packets"
    "h264_mp4toannexb"
    "-c:v copy"
    "-c:a copy"
    "analyze_title_streams"
    "video_passthrough"
    "audio_passthrough"
    "p.force_reencode || title.force_reencode"
    "Remuxing compliant video"
    "Remuxing compliant audio"
)
  string(FIND "${AUTHOR}" "${NEEDED}" POS)
  if(POS EQUAL -1)
    message(FATAL_ERROR "missing automatic passthrough implementation marker: ${NEEDED}")
  endif()
endforeach()

foreach(NEEDED
    "bool force_reencode = false"
    "bool force_reencode_new_titles = false"
    "title.force_reencode = defaults.force_reencode_new_titles"
)
  string(FIND "${MODEL}" "${NEEDED}" POS)
  if(POS EQUAL -1)
    message(FATAL_ERROR "missing force-reencode model/default marker: ${NEEDED}")
  endif()
endforeach()

foreach(NEEDED
    "forceReencode"
    "t.force_reencode"
)
  string(FIND "${PROJECT}" "${NEEDED}" POS)
  if(POS EQUAL -1)
    message(FATAL_ERROR "per-title force-reencode is not persisted: ${NEEDED}")
  endif()
endforeach()

foreach(NEEDED
    "Always re-encode new titles"
    "Force re-encode"
    "t.force_reencode"
)
  string(FIND "${GUI}" "${NEEDED}" POS)
  if(POS EQUAL -1)
    message(FATAL_ERROR "missing GUI force-reencode control/default marker: ${NEEDED}")
  endif()
endforeach()

foreach(NEEDED
    "--force-reencode            Force video and audio encoding for following titles"
    "--allow-passthrough         Remux compliant streams for following titles (default)"
    "t.force_reencode = next_title_force_reencode"
)
  string(FIND "${CLI}" "${NEEDED}" POS)
  if(POS EQUAL -1)
    message(FATAL_ERROR "missing CLI per-title passthrough marker: ${NEEDED}")
  endif()
endforeach()

message(STATUS "automatic Blu-ray elementary-stream passthrough source checks ok")
