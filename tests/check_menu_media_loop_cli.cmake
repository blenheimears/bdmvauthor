foreach(option --menu-loop --no-menu-loop)
  execute_process(COMMAND "${CLI}" -o "${WORK_DIR}/menu-loop-test.iso" ${option} missing.mkv
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(rc EQUAL 0 OR NOT err MATCHES "missing title source")
    message(FATAL_ERROR "${option} was not accepted before project validation: ${err}")
  endif()
endforeach()
