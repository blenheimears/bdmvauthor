if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "SOURCE_DIR not supplied")
endif()

file(READ "${SOURCE_DIR}/third_party/tsmuxer/tsMuxer/tsMuxer.cpp" TSMUXER)
file(READ "${SOURCE_DIR}/src/author.cpp" AUTHOR)

foreach(needle
    "if (!m_subMode && m_cbrBitrate != -1 && m_minBitrate != -1 && m_lastPCR != -1)"
    "Never let the final M2TS arrival-clock"
    "newPCR = FFMAX(newPCR, m_lastPCR)"
    "const unsigned menu_transport_floor = kInteractiveMenuTransportFloorKbps"
    ", subClip")
  string(FIND "${TSMUXER}\n${AUTHOR}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "preloaded IGS transport regression: missing ${needle}")
  endif()
endforeach()

# The menu transport floor is intentional, but it must not be inherited by
# TSMuxer's subClip muxer.  This protects both regular BD and UHD menus because
# both use the same preloaded IGS path.
string(FIND "${TSMUXER}" "if (m_cbrBitrate != -1 && m_minBitrate != -1 && m_lastPCR != -1)" old_floor)
if(NOT old_floor EQUAL -1)
  message(FATAL_ERROR "unqualified min-bitrate padding can still affect preloaded subclips")
endif()

message(STATUS "Blu-ray/UHD preloaded IGS subclips avoid transport-floor padding and backward ATS flushes")
