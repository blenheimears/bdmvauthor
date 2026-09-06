if(NOT DEFINED CLI OR NOT DEFINED WORK_DIR)
  message(FATAL_ERROR "CLI/WORK_DIR not supplied")
endif()
set(dummy "${WORK_DIR}/repeat-cli-dummy.mp4")
set(image "${WORK_DIR}/repeat-cli-output.iso")
file(WRITE "${dummy}" "")

execute_process(COMMAND "${CLI}" -o "${image}" --first-play-title 1 --first-play-repeat 0 "${dummy}"
  RESULT_VARIABLE loop_rc OUTPUT_VARIABLE loop_out ERROR_VARIABLE loop_err)
set(loop_text "${loop_out}${loop_err}")
string(FIND "${loop_text}" "ffprobe could not inspect title streams" loop_pos)
if(loop_pos EQUAL -1)
  message(FATAL_ERROR "valid infinite Play Title repeat did not survive project validation: ${loop_text}")
endif()

execute_process(COMMAND "${CLI}" -o "${image}" --first-play-menu top --first-play-repeat 2 "${dummy}"
  RESULT_VARIABLE menu_rc OUTPUT_VARIABLE menu_out ERROR_VARIABLE menu_err)
set(menu_text "${menu_out}${menu_err}")
string(FIND "${menu_text}" "cannot repeat a menu jump" menu_pos)
if(menu_pos EQUAL -1)
  message(FATAL_ERROR "repeated menu jump was not rejected: ${menu_text}")
endif()

execute_process(COMMAND "${CLI}" -o "${image}" --button-action-repeat 2 "${dummy}"
  RESULT_VARIABLE missing_rc OUTPUT_VARIABLE missing_out ERROR_VARIABLE missing_err)
set(missing_text "${missing_out}${missing_err}")
string(FIND "${missing_text}" "requires a button option first" missing_pos)
if(missing_pos EQUAL -1)
  message(FATAL_ERROR "repeat option without a selected button was not rejected: ${missing_text}")
endif()


execute_process(COMMAND "${CLI}" -o "${image}" --first-play-title 1 --first-play-repeat-group-begin 3 --first-play-title 1 --first-play-repeat-group-end "${dummy}"
  RESULT_VARIABLE group_rc OUTPUT_VARIABLE group_out ERROR_VARIABLE group_err)
set(group_text "${group_out}${group_err}")
string(FIND "${group_text}" "ffprobe could not inspect title streams" group_pos)
if(group_pos EQUAL -1)
  message(FATAL_ERROR "valid finite First Playback repeat group did not survive project validation: ${group_text}")
endif()

execute_process(COMMAND "${CLI}" -o "${image}" --title-button 1 Loop --button-repeat-group-begin 0 --button-action-title 1 --button-repeat-group-end "${dummy}"
  RESULT_VARIABLE button_group_rc OUTPUT_VARIABLE button_group_out ERROR_VARIABLE button_group_err)
set(button_group_text "${button_group_out}${button_group_err}")
string(FIND "${button_group_text}" "ffprobe could not inspect title streams" button_group_pos)
if(button_group_pos EQUAL -1)
  message(FATAL_ERROR "valid infinite button repeat group did not survive project validation: ${button_group_text}")
endif()

file(REMOVE "${dummy}" "${image}")
message(STATUS "repeat/loop CLI validation checks ok")
