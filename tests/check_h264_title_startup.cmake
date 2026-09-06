file(READ "${SOURCE_DIR}/src/author.cpp" AUTHOR)
file(READ "${SOURCE_DIR}/src/compliance.cpp" COMPLIANCE)
file(READ "${SOURCE_DIR}/include/bdmvauthor/compliance.hpp" COMPLIANCE_H)
file(READ "${SOURCE_DIR}/third_party/tsmuxer/tsMuxer/h264StreamReader.cpp" TSMUXER_H264)

foreach(needle
  "h264_buffering_period_sei_present"
  "h264_picture_timing_sei_present"
  "h264_initial_cpb_removal_delay_90k")
  string(FIND "${COMPLIANCE_H}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing H.264 startup-compliance model marker: ${needle}")
  endif()
endforeach()

foreach(needle
  "trace_contains_marker(trace, \"Buffering Period\")"
  "trace_contains_marker(trace, \"Picture Timing\")"
  "initial_cpb_removal_delay[0]"
  "bdmvauthor-compliance-cache-v11"
  "rules=bd-uhd-primary-av-v11-h264-cpb-delay"
  "meta += \", insertSEI, contSPS\""
  "insertSEI, contSPS")
  string(FIND "${AUTHOR}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing H.264 startup author marker: ${needle}")
  endif()
endforeach()

foreach(needle
  "initial CPB removal delay is inconsistent"
  "90000.0 * i.h264_hrd_max_cpb_bits / i.h264_hrd_max_bitrate_bps")
  string(FIND "${COMPLIANCE}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing H.264 startup compliance marker: ${needle}")
  endif()
endforeach()

foreach(needle
  "historical hard-coded value here was 855000 ticks (9.5 s)"
  "cpbBits * 90000.0L * 0.9L"
  "initial_cpb_removal_delay_length_minus1"
  "syntaxMax"
  "sei.initial_cpb_removal_delay[idx] = static_cast<int>(delay)"
  "matching x264's normal"
  "vbv-init=0.9")
  string(FIND "${TSMUXER_H264}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing bundled tsMuxer H.264 CPB-delay fix marker: ${needle}")
  endif()
endforeach()

string(FIND "${TSMUXER_H264}" "sei.initial_cpb_removal_delay[0] = 855000" old_delay)
if(NOT old_delay EQUAL -1)
  message(FATAL_ERROR "bundled tsMuxer still hard-codes a 9.5-second AVC initial CPB delay")
endif()

message(STATUS "H.264 title-start buffering regression checks ok")
