if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "SOURCE_DIR not supplied")
endif()
file(READ "${SOURCE_DIR}/src/author.cpp" A)
file(READ "${SOURCE_DIR}/src/compliance.cpp" C)
file(READ "${SOURCE_DIR}/src/gui.cpp" G)
file(READ "${SOURCE_DIR}/include/bdmvauthor/model.hpp" M)
foreach(needle
  "kBluRay1080AvcTimingModes"
  "1080p25 (x264 fake interlaced)"
  "1080p29.97 (x264 fake interlaced)"
  "--fake-interlaced"
  ":fake-interlaced=1"
  "timing.fake_interlaced"
  "kBluRay1080TimingModes")
  string(FIND "${A}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "fake-interlaced authoring regression: missing ${needle}")
  endif()
endforeach()
foreach(needle
  "fake_interlaced_avc"
  "h264_frame_mbs_only_flag == 0"
  "h264_mb_adaptive_frame_field_flag == 0")
  string(FIND "${C}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "fake-interlaced compliance regression: missing ${needle}")
  endif()
endforeach()
foreach(needle
  "25p (1080 AVC — x264 fake interlaced)"
  "29.97p (1080 AVC — x264 fake interlaced)"
  "50i"
  "59.94i")
  string(FIND "${G}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "fake-interlaced GUI regression: missing ${needle}")
  endif()
endforeach()
string(FIND "${M}" "\"fake-interlaced\"" protected_pos)
if(protected_pos EQUAL -1)
  message(FATAL_ERROR "fake-interlaced must be protected from codec-private override")
endif()
message(STATUS "fake-interlaced progressive Blu-ray AVC source checks ok")
