file(READ "${SOURCE_DIR}/include/bdmvauthor/model.hpp" MODEL)
file(READ "${SOURCE_DIR}/src/gui.cpp" GUI)
file(READ "${SOURCE_DIR}/src/author.cpp" AUTHOR)
file(READ "${SOURCE_DIR}/src/cli.cpp" CLI)

foreach(NEEDED
    "struct NewProjectDefaults"
    "apply_new_project_defaults"
    "make_title_from_defaults"
    "audio_codec_has_configurable_bitrate"
    "set_audio_bitrate_kbps"
)
  string(FIND "${MODEL}" "${NEEDED}" POS)
  if(POS EQUAL -1)
    message(FATAL_ERROR "missing new-project/audio-bitrate model marker: ${NEEDED}")
  endif()
endforeach()

foreach(NEEDED
    "Defaults for new projects"
    "QSettings settings"
    "defaults/titleEncoding"
    "defaults/menuEncoding"
    "Defaults for &new projects…"
    "titles_=new QTableWidget(0,19)"
    "Audio bitrate"
    "Force re-encode"
    "force_reencode_new_titles"
    "menu_audio_bitrate_"
    "make_title_from_defaults(project_defaults_)"
    "project_defaults_.button_width"
    "project_defaults_.label_font_size_px"
)
  string(FIND "${GUI}" "${NEEDED}" POS)
  if(POS EQUAL -1)
    message(FATAL_ERROR "missing GUI defaults/audio-bitrate marker: ${NEEDED}")
  endif()
endforeach()

foreach(NEEDED
    "--audio-bitrate KBPS"
    "--menu-audio-bitrate KBPS"
)
  string(FIND "${CLI}" "${NEEDED}" POS)
  if(POS EQUAL -1)
    message(FATAL_ERROR "missing CLI audio-bitrate marker: ${NEEDED}")
  endif()
endforeach()

string(FIND "${AUTHOR}" "std::to_string(e.ac3_bitrate_kbps)" POS)
if(POS EQUAL -1)
  message(FATAL_ERROR "TrueHD/AC-3 encoding must use the configured AC-3 bitrate")
endif()
