file(READ "${SOURCE_DIR}/src/author.cpp" AUTHOR)
file(READ "${SOURCE_DIR}/src/compliance.cpp" COMPLIANCE)
file(READ "${SOURCE_DIR}/include/bdmvauthor/compliance.hpp" COMPLIANCE_H)

foreach(NEEDED
    "compliance_sample_windows"
    "kSampleSeconds = 60.0"
    "duration_seconds <= kSampleSeconds * 3.0"
    "duration_seconds * 0.5 - kSampleSeconds * 0.5"
    "duration_seconds - kSampleSeconds"
    "probe_video_packets_sampled"
    "probe_max_consecutive_b_frames_sampled"
    "-read_intervals"
    "stream_command_lines"
    "first_mb_in_slice"
    "slice_type"
    "evaluate_blu_ray_video_headers(info, target)"
)
  string(FIND "${AUTHOR}" "${NEEDED}" POS)
  if(POS EQUAL -1)
    message(FATAL_ERROR "missing bounded compliance sampling marker: ${NEEDED}")
  endif()
endforeach()

foreach(FORBIDDEN
    "video-packets.txt"
    "video-frames.txt"
    " -show_frames "
    " -v error -select_streams v:0 -show_packets"
)
  string(FIND "${AUTHOR}" "${FORBIDDEN}" POS)
  if(NOT POS EQUAL -1)
    message(FATAL_ERROR "unbounded/temporary-file compliance scan returned: ${FORBIDDEN}")
  endif()
endforeach()

string(FIND "${AUTHOR}" "if (!evaluate_blu_ray_video_headers(info, target).compliant) return info;" HEADER_POS)
string(FIND "${AUTHOR}" "probe_video_packets_sampled(tools, source, duration_seconds)" SAMPLE_POS)
if(HEADER_POS EQUAL -1 OR SAMPLE_POS EQUAL -1 OR NOT HEADER_POS LESS SAMPLE_POS)
  message(FATAL_ERROR "cheap header compliance gate must run before sampled packet verification")
endif()

foreach(NEEDED
    "evaluate_blu_ray_video_headers"
    "sampled first/middle/last"
)
  string(FIND "${COMPLIANCE}" "${NEEDED}" POS)
  if(POS EQUAL -1)
    message(FATAL_ERROR "missing sampled compliance evaluator marker: ${NEEDED}")
  endif()
endforeach()

string(FIND "${COMPLIANCE_H}" "evaluate_blu_ray_video_headers" POS)
if(POS EQUAL -1)
  message(FATAL_ERROR "missing cheap Blu-ray video header evaluator API")
endif()

message(STATUS "bounded first/middle/last compliance sampling source checks ok")
