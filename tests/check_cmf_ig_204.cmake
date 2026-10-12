if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "SOURCE_DIR not set")
endif()
file(READ "${SOURCE_DIR}/src/author.cpp" AUTHOR)
file(READ "${SOURCE_DIR}/src/hdmv.cpp" HDMV)
file(READ "${SOURCE_DIR}/third_party/tsmuxer/tsMuxer/blurayHelper.cpp" BLURAY)
foreach(needle
  [[menu_main_clip = static_cast<unsigned>(mi) * 2U]]
  [[menu_igs_clip = menu_main_clip + 1U]]
  [[, subClip]]
  [[const auto title_clip = 2U * menu_count]]
  [[const fs::path out=dir/"audio.dca"]]
)
  string(FIND "${AUTHOR}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "preloaded IG/libhdma regression: missing ${needle}")
  endif()
endforeach()
string(FIND "${HDMV}" [[rest.push_back(0x80); // stream_model=1 (preloaded)]] pos)
if(pos EQUAL -1)
  message(FATAL_ERROR "preloaded graphics model was changed")
endif()
foreach(needle
  [[clpiParser.presentation_end_time = preloadedIG]]
  [[preloadedIGEnd45k(firstPts[i], lastPts[i])]]
  [[mplsParser.m_preloadIGSubPathOUT_time = preloadedIGEnd45k(]]
)
  string(FIND "${BLURAY}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "CLPI/MPLS window mismatch: ${needle}")
  endif()
endforeach()
message(STATUS "Preloaded graphics-only M2TS retained; CLPI and MPLS positive metadata window applied")
