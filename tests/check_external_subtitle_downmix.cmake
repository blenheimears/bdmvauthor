file(READ "${SOURCE_DIR}/include/bdmvauthor/model.hpp" model)
file(READ "${SOURCE_DIR}/include/bdmvauthor/author.hpp" author_h)
file(READ "${SOURCE_DIR}/src/author.cpp" author)
file(READ "${SOURCE_DIR}/src/gui.cpp" gui)
file(READ "${SOURCE_DIR}/src/project_file.cpp" project)
file(READ "${SOURCE_DIR}/src/cli.cpp" cli)
foreach(needle
    "int output_channels = 0"
    "struct ExternalSubtitle"
    "std::vector<ExternalSubtitle> external_subtitles"
    "validate_encoding_profile_for_target"
    "resolved_audio_output_channels"
    "BDMV Author does not upmix"
    "stream_settings->output_channels > 0"
    "external_source.empty() ? source : track.external_source"
    "externalSubtitles"
    "outputChannels"
    "Attach subtitle file…"
    "Output channels"
    "--audio-stream-channels"
    "--subtitle-file"
    "kProjectFormatVersion = 24")
  set(found FALSE)
  foreach(text "${model}" "${author_h}" "${author}" "${gui}" "${project}" "${cli}")
    string(FIND "${text}" "${needle}" pos)
    if(NOT pos EQUAL -1)
      set(found TRUE)
    endif()
  endforeach()
  if(NOT found)
    message(FATAL_ERROR "external subtitle/downmix regression: missing ${needle}")
  endif()
endforeach()
# Catch the 0.1.56 Qt build regression specifically: GUI must call an exported declaration.
string(FIND "${author_h}" "static void validate_encoding_profile_for_target" decl)
string(FIND "${gui}" "AuthorEngine::validate_encoding_profile_for_target" use)
if(decl EQUAL -1 OR use EQUAL -1)
  message(FATAL_ERROR "GUI encoding validator is not declared through AuthorEngine")
endif()
