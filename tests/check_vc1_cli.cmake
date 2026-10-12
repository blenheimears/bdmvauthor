if(NOT DEFINED CLI)
  message(FATAL_ERROR "CLI path not supplied")
endif()

execute_process(
  COMMAND "${CLI}" --target bluray --video-codec vc1 --vc1-preset fast
          --video-option trellis=0 --video-option aq-strength=0.8 dummy.mp4
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
set(text "${out}${err}")
string(FIND "${text}" "missing title source" missing_pos)
if(missing_pos EQUAL -1)
  message(FATAL_ERROR "experimental VC-1 codec/preset/advanced options did not pass normal CLI validation: ${text}")
endif()

execute_process(COMMAND "${CLI}" --help RESULT_VARIABLE help_rc OUTPUT_VARIABLE help_out ERROR_VARIABLE help_err)
if(NOT help_rc EQUAL 0)
  message(FATAL_ERROR "CLI --help failed")
endif()
string(FIND "${help_out}${help_err}" "vc1enc preset: normal/fast (experimental)" help_pos)
if(help_pos EQUAL -1)
  message(FATAL_ERROR "CLI help does not identify the normal experimental VC-1 preset control")
endif()

execute_process(
  COMMAND "${CLI}" --target bluray --video-codec vc1 --video-option es-out=forbidden.vc1 dummy.mp4
  RESULT_VARIABLE raw_rc OUTPUT_VARIABLE raw_out ERROR_VARIABLE raw_err)
if(raw_rc EQUAL 0)
  message(FATAL_ERROR "VC-1 advanced options unexpectedly allow raw elementary-stream sidecar output")
endif()
string(FIND "${raw_out}${raw_err}" "controls a disc-spec parameter" raw_msg)
if(raw_msg EQUAL -1)
  message(FATAL_ERROR "VC-1 raw-sidecar rejection did not report the protected-option error: ${raw_out}${raw_err}")
endif()

foreach(protected_option "format=m2ts" "container=m2ts")
  execute_process(
    COMMAND "${CLI}" --target bluray --video-codec vc1 --video-option "${protected_option}" dummy.mp4
    RESULT_VARIABLE protected_rc OUTPUT_VARIABLE protected_out ERROR_VARIABLE protected_err)
  if(protected_rc EQUAL 0)
    message(FATAL_ERROR "VC-1 advanced options unexpectedly allow ${protected_option}")
  endif()
  string(FIND "${protected_out}${protected_err}" "controls a disc-spec parameter" protected_msg)
  if(protected_msg EQUAL -1)
    message(FATAL_ERROR "VC-1 protected-output rejection did not report the expected error: ${protected_out}${protected_err}")
  endif()
endforeach()

message(STATUS "experimental VC-1 CLI checks ok")
