if(NOT DEFINED CLI)
  message(FATAL_ERROR "CLI not supplied")
endif()

function(expect_valid_duration value)
  execute_process(
    COMMAND "${CLI}" -o out.iso --chapters-every "${value}" /definitely/missing-bdmvauthor-chapter-test.mp4
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  set(text "${out}${err}")
  string(FIND "${text}" "missing title source" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "valid chapter duration '${value}' failed before source validation: ${text}")
  endif()
endfunction()

function(expect_invalid_duration value expected)
  execute_process(
    COMMAND "${CLI}" -o out.iso --chapters-every "${value}" /definitely/missing-bdmvauthor-chapter-test.mp4
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  set(text "${out}${err}")
  string(FIND "${text}" "${expected}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "invalid chapter duration '${value}' was not rejected as expected: ${text}")
  endif()
  string(FIND "${text}" "missing title source" source_pos)
  if(NOT source_pos EQUAL -1)
    message(FATAL_ERROR "invalid chapter duration '${value}' reached source validation")
  endif()
endfunction()

expect_valid_duration("300s")
expect_valid_duration("5m")
expect_valid_duration("05:00")
expect_valid_duration("1:02:03")
expect_invalid_duration("00:60" "invalid MM:SS duration")
expect_invalid_duration("1:60:00" "invalid HH:MM:SS duration")
expect_invalid_duration("nan" "invalid duration")
expect_invalid_duration("inf" "invalid duration")
expect_invalid_duration("-5" "invalid duration")

message(STATUS "chapter CLI duration parsing checks ok")
