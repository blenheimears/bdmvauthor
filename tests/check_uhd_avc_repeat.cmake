file(READ "${SOURCE_DIR}/include/bdmvauthor/model.hpp" MODEL)
file(READ "${SOURCE_DIR}/src/author.cpp" AUTHOR)
file(READ "${SOURCE_DIR}/src/compliance.cpp" COMPLIANCE)
file(READ "${SOURCE_DIR}/src/hdmv.cpp" HDMV)
file(READ "${SOURCE_DIR}/src/project_file.cpp" PROJECT)
file(READ "${SOURCE_DIR}/src/gui.cpp" GUI)
file(READ "${SOURCE_DIR}/src/cli.cpp" CLI)

foreach(needle
  "codec == VideoCodec::Hevc || codec == VideoCodec::X264"
  "target == DiscTarget::UltraHdBluRay2160 && codec == VideoCodec::X264"
  "return 40000"
  "std::uint16_t repeat_count = 1")
  string(FIND "${MODEL}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing UHD AVC/repeat model marker: ${needle}")
  endif()
endforeach()

foreach(needle
  "AVC/H.264 in Ultra HD Blu-ray v3 is limited to 1920x1080 16:9 at 23.976p or 24p"
  "codec == VideoCodec::X264"
  "width != 1920 || height != 1080"
  "bdmvauthor-video-cache-v13"
  "bdmvauthor-compliance-cache-v11"
  "rules=bd-uhd-primary-av-v11-h264-cpb-delay"
  "action.repeat_count == 0U"
  "action.repeat_count > 1U"
  "DVD stream-selection action cannot repeat forever")
  string(FIND "${AUTHOR}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing UHD AVC/repeat author marker: ${needle}")
  endif()
endforeach()

foreach(needle
  "Ultra HD Blu-ray primary video must use HEVC/H.265 or 1920x1080 AVC/H.264"
  "Ultra HD Blu-ray AVC/H.264 is limited to 1920x1080 at 23.976p or 24p"
  "Ultra HD Blu-ray AVC/H.264 primary video is SDR BT.709 only"
  "AVC/H.264 1920x1080 stream passes Ultra HD Blu-ray v3")
  string(FIND "${COMPLIANCE}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing UHD AVC compliance marker: ${needle}")
  endif()
endforeach()

foreach(needle
  "RepeatTargetGpr=4093"
  "sub_gpr_immediate"
  "0xffffffffU"
  "action.repeat_count==0U"
  "action.repeat_count-1U")
  string(FIND "${HDMV}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing HDMV repeat marker: ${needle}")
  endif()
endforeach()

foreach(needle
  "kProjectFormatVersion = 24"
  "repeatCount")
  string(FIND "${PROJECT}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing repeat persistence marker: ${needle}")
  endif()
endforeach()

foreach(needle
  "AVC / H.264 (1080p23.976/24 only)"
  "Set repeat…"
  "Repeat count (0 = forever)"
  "Repeat forever on an individual action is allowed only for the final Play Title action.")
  string(FIND "${GUI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing GUI UHD AVC/repeat marker: ${needle}")
  endif()
endforeach()

foreach(needle
  "UHD: hevc or x264 (1080p23.976/24)"
  "--button-action-repeat N"
  "--first-play-repeat N"
  "--button-action-repeat must be 0..1000"
  "--first-play-repeat must be 0..1000")
  string(FIND "${CLI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing CLI UHD AVC/repeat marker: ${needle}")
  endif()
endforeach()

message(STATUS "UHD v3 AVC and repeat/loop source checks ok")
