if(NOT EXISTS "${MPLEX}")
  message(FATAL_ERROR "bundled mplex executable missing: ${MPLEX}")
endif()
execute_process(
  COMMAND "${MPLEX}" --help
  RESULT_VARIABLE rc
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err
)
set(text "${out}\n${err}")
if(NOT text MATCHES "mjpegtools mplex-2 version 2\\.2\\.3 \\(2\\.2\\.7\\)")
  message(FATAL_ERROR "unexpected bundled mplex help/version output: ${text}")
endif()
if(NOT text MATCHES "DVD with NAV sectors")
  message(FATAL_ERROR "bundled mplex help does not expose DVD NAV-sector profile")
endif()
