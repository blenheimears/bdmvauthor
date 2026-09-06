file(READ "${SOURCE_DIR}/include/bdmvauthor/model.hpp" MODEL)
file(READ "${SOURCE_DIR}/include/bdmvauthor/hdmv.hpp" HDMV_H)
file(READ "${SOURCE_DIR}/src/hdmv.cpp" HDMV)
file(READ "${SOURCE_DIR}/src/author.cpp" AUTHOR)
file(READ "${SOURCE_DIR}/src/project_file.cpp" PROJECT)
file(READ "${SOURCE_DIR}/src/gui.cpp" GUI)
file(READ "${SOURCE_DIR}/src/cli.cpp" CLI)
foreach(NEEDED
    "enum class NavigationActionKind { PlayTitle, Menu, AudioTrack, SubtitleTrack, SubtitleOff, RepeatBegin, RepeatEnd }"
    "std::vector<NavigationAction> actions"
    "std::vector<NavigationAction> first_play_actions"
    "std::uint16_t repeat_count = 1"
    "button_action_sequence"
    "remove_title_references(std::vector<NavigationAction>& actions"
)
  string(FIND "${MODEL}" "${NEEDED}" POS)
  if(POS EQUAL -1)
    message(FATAL_ERROR "missing navigation model marker: ${NEEDED}")
  endif()
endforeach()
foreach(NEEDED
    "make_navigation_sequence_commands"
    "make_immediate_navigation_sequence_commands"
    "first_play_object"
    "extra_objects"
    "button_commands"
    "command_base+6U"
    "ReturnMenuGpr=4091"
    "action.kind==NavigationActionKind::PlayTitle&&final_action"
    "jump_title(static_cast<std::uint16_t>(action.target_title))"
    "RepeatTargetGpr=4093"
    "sub_gpr_immediate"
    "action.repeat_count==0U"
)
  string(FIND "${HDMV_H}${HDMV}" "${NEEDED}" POS)
  if(POS EQUAL -1)
    message(FATAL_ERROR "missing HDMV action-sequence marker: ${NEEDED}")
  endif()
endforeach()
foreach(NEEDED
    "navigation_objects"
    "button_commands"
    "make_immediate_navigation_sequence_commands"
    "compiled.push_back({hdmv::jump_object(add_navigation_object"
    "make_title_stream_initialization_commands"
    "navigation_objects, first_play_object"
)
  string(FIND "${AUTHOR}" "${NEEDED}" POS)
  if(POS EQUAL -1)
    message(FATAL_ERROR "missing author navigation-plan marker: ${NEEDED}")
  endif()
endforeach()
foreach(NEEDED
    "kProjectFormatVersion = 24"
    "firstPlayActions"
    "repeatCount"
    "actions_json(button_action_sequence(b))"
)
  string(FIND "${PROJECT}" "${NEEDED}" POS)
  if(POS EQUAL -1)
    message(FATAL_ERROR "missing project persistence marker: ${NEEDED}")
  endif()
endforeach()
foreach(NEEDED
    "class ActionSequenceDialog"
    "Edit actions…"
    "Startup sequence…"
    "edit_startup_sequence"
    "Set repeat…"
    "Repeat count (0 = forever)"
)
  string(FIND "${GUI}" "${NEEDED}" POS)
  if(POS EQUAL -1)
    message(FATAL_ERROR "missing GUI navigation-sequence marker: ${NEEDED}")
  endif()
endforeach()
foreach(NEEDED
    "--button-action-title"
    "--button-action-menu"
    "--first-play-title"
    "--first-play-menu"
    "--button-action-repeat"
    "--first-play-repeat"
)
  string(FIND "${CLI}" "${NEEDED}" POS)
  if(POS EQUAL -1)
    message(FATAL_ERROR "missing CLI navigation-sequence marker: ${NEEDED}")
  endif()
endforeach()
