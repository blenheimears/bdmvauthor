file(READ "${SOURCE_DIR}/third_party/tsmuxer/tsMuxer/tsMuxer.cpp" TSMUXER)

# MPEG-TS continuity_counter only advances for packets carrying payload.  An
# adaptation-only PCR packet on the video/PCR PID therefore has to repeat the
# counter of the preceding payload packet.  Using the stream's next counter
# value creates a discontinuity before and after every injected PCR packet.
string(REGEX MATCHALL "tsPacket->counter = \\(m_streamInfo\\[m_pmt\\.pcr_pid\\]\\.m_tsCnt - 1\\) & 0x0f" FIXED_ASSIGNMENTS "${TSMUXER}")
list(LENGTH FIXED_ASSIGNMENTS FIXED_COUNT)
if(NOT FIXED_COUNT EQUAL 2)
  message(FATAL_ERROR "expected both adaptation-only PCR writers to repeat the previous continuity counter; found ${FIXED_COUNT}")
endif()

string(FIND "${TSMUXER}" "tsPacket->counter = m_streamInfo[m_pmt.pcr_pid].m_tsCnt;" OLD_ASSIGNMENT)
if(NOT OLD_ASSIGNMENT EQUAL -1)
  message(FATAL_ERROR "found the old PCR continuity assignment that advances an adaptation-only packet")
endif()
