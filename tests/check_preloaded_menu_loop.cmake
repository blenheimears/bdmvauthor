if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "SOURCE_DIR not supplied")
endif()

file(READ "${SOURCE_DIR}/src/author.cpp" AUTHOR)
file(READ "${SOURCE_DIR}/src/hdmv.cpp" HDMV)
file(READ "${SOURCE_DIR}/third_party/tsmuxer/tsMuxer/main.cpp" TSMUXER_MAIN)
file(READ "${SOURCE_DIR}/third_party/tsmuxer/tsMuxer/blurayHelper.cpp" BLURAY_HELPER)
file(READ "${SOURCE_DIR}/third_party/tsmuxer/tsMuxer/tsPacket.cpp" TS_PACKET)
file(READ "${SOURCE_DIR}/third_party/tsmuxer/tsMuxer/tsPacket.h" TS_PACKET_H)

foreach(needle
  "rest.push_back(0x80); // stream_model=1 (preloaded), ui_model=0 (Always-On)"
  "preloaded composition does not carry composition/selection timeout clocks")
  string(FIND "${HDMV}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing preloaded IGS marker: ${needle}")
  endif()
endforeach()

foreach(needle
  "Looping menus use exactly one PlayItem"
  "real Play_PL restart gives the player a clean transport-clock epoch"
  "menu_main_clip = static_cast<unsigned>(mi) * 2U"
  "menu_igs_clip = menu_main_clip + 1U"
  ", subClip")
  string(FIND "${AUTHOR}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing preloaded/single-PlayItem menu authoring marker: ${needle}")
  endif()
endforeach()

string(FIND "${AUTHOR}" "--repeat-playitems=" author_repeat_option)
if(NOT author_repeat_option EQUAL -1)
  message(FATAL_ERROR "BDMV Author must not request repeated PlayItems for menu loops")
endif()
string(FIND "${AUTHOR}" "kMenuPlayItemRepeats" author_repeat_count)
if(NOT author_repeat_count EQUAL -1)
  message(FATAL_ERROR "legacy 999-PlayItem menu loop is still present")
endif()

foreach(needle
  "--repeat-playitems must be between 1 and 999"
  "isPreloadedIGSubMuxer"
  "subMuxer && !preloadedIGSubMuxer")
  string(FIND "${TSMUXER_MAIN}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing tsMuxer preloaded-menu marker: ${needle}")
  endif()
endforeach()

foreach(needle
  "application_type = 5"
  "number_of_SubPaths = 1"
  "subPath_type = 3"
  "m_preloadIGSubPathM2tsOffset = subMuxer->getFirstFileNum()"
  "m_repeatPlayItems = std::max(1U, repeatPlayItems)")
  string(FIND "${BLURAY_HELPER}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing Blu-ray preloaded-IG helper marker: ${needle}")
  endif()
endforeach()

foreach(needle
  "if (application_type == 5)"
  "outputPlayItemCount = sourcePlayItemCount * repeatCount"
  "composePlayItem(writer, outputPlayItem, sourcePlayItem, mainStreamInfo.m_index)"
  "type == 3 ? 1 : 0"
  "m_preloadIGSubPathIN_time"
  "m_preloadIGSubPathOUT_time")
  string(FIND "${TS_PACKET}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing MPLS/CLPI preloaded-menu marker: ${needle}")
  endif()
endforeach()

foreach(needle
  "m_preloadIGSubPathM2tsOffset"
  "m_repeatPlayItems")
  string(FIND "${TS_PACKET_H}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing preloaded-menu parser state: ${needle}")
  endif()
endforeach()

# Ordinary titles must get an explicit unique clip number but no repeated-PlayItem request.
string(FIND "${AUTHOR}" "meta_header(playlist, resolved_title_chapters[i], p.target, static_cast<unsigned>(combined_transport_limit_kbps), 0U, title_clip)" title_call)
if(title_call EQUAL -1)
  message(FATAL_ERROR "ordinary title muxing no longer uses an isolated non-repeating clip call")
endif()
string(FIND "${AUTHOR}" "title_clip, kMenuPlayItemRepeats" repeated_title)
if(NOT repeated_title EQUAL -1)
  message(FATAL_ERROR "menu repeat count leaked into ordinary title muxing")
endif()

message(STATUS "Preloaded IG / clean playlist-restart menu-loop regression checks ok")
