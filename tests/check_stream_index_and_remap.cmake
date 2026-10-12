if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "SOURCE_DIR not supplied")
endif()
file(READ "${SOURCE_DIR}/src/author.cpp" AUTHOR)
file(READ "${SOURCE_DIR}/src/gui.cpp" GUI)

foreach(needle
    "int input_stream_index = -1"
    "stream=index,codec_type,codec_name"
    "ffmpeg_audio_map_spec"
    "track.input_stream_index"
    "source_track.input_stream_index")
  string(FIND "${AUTHOR}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing exact input-stream mapping marker: ${needle}")
  endif()
endforeach()

foreach(needle
    "authored_audio_stream_keys"
    "authored_subtitle_stream_keys"
    "remap_stream_actions"
    "old_audio_order"
    "new_audio_order"
    "old_subtitle_order"
    "new_subtitle_order"
    "remap_stream_actions(root_menu_"
    "remap_stream_actions(first_play_actions_"
    "RoleTitleMenuActions")
  string(FIND "${GUI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing stream-reference remap marker: ${needle}")
  endif()
endforeach()
