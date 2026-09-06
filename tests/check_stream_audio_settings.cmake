if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "SOURCE_DIR not supplied")
endif()
file(READ "${SOURCE_DIR}/include/bdmvauthor/model.hpp" MODEL)
file(READ "${SOURCE_DIR}/src/project_file.cpp" PROJECT)
file(READ "${SOURCE_DIR}/src/author.cpp" AUTHOR)
file(READ "${SOURCE_DIR}/src/gui.cpp" GUI)
file(READ "${SOURCE_DIR}/src/cli.cpp" CLI)
foreach(needle
  "struct AudioStreamSettings"
  "struct SubtitleStreamSettings"
  "lpcm_sample_rate_hz = 0"
  "lpcm_bit_depth = 0"
  "lpcm_bitrate_kbps"
  "target_max_combined_av_bitrate_kbps"
  "effective_audio_encoding_for_stream"
)
  string(FIND "${MODEL}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing stream-level model marker: ${needle}")
  endif()
endforeach()
foreach(needle
  "kProjectFormatVersion = 24"
  "audioStreams"
  "subtitleStreams"
  "lpcmSampleRateHz"
  "lpcmBitDepth"
)
  string(FIND "${PROJECT}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing stream-level project marker: ${needle}")
  endif()
endforeach()
foreach(needle
  "configured_audio_rate_kbps"
  "effective_audio_language"
  "effective_subtitle_language"
  "video peak budget"
  "pcm_s24le"
  "pcm_s24be"
  "stream.override_encoding"
)
  string(FIND "${AUTHOR}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing stream-level author marker: ${needle}")
  endif()
endforeach()
foreach(needle
  "TitleStreamsDialog"
  "Language override"
  "Custom encoding"
  "Lossless rate"
  "Lossless depth"
  "Configured encode ceiling"
  "Configured fixed-rate ceiling"
  "TrueHD track(s): peak bitrate is measured during authoring"
  "row_peak_measured_during_authoring"
  "RoleAudioStreamSettings"
  "RoleSubtitleStreamSettings"
  "Streams…"
  "setReadOnly(true)"
  "titles_=new QTableWidget(0,19)"
  "setWindowTitle(QStringLiteral(\"%1[*]\").arg(name))"
)
  string(FIND "${GUI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing stream-level GUI marker: ${needle}")
  endif()
endforeach()
foreach(needle
  "--audio-stream-codec S MODE"
  "--audio-stream-bitrate S K"
  "--audio-stream-lpcm-rate S HZ"
  "--audio-stream-lpcm-depth S BITS"
  "--audio-stream-option S N=V"
  "--audio-stream-language S ISO3|source"
  "--subtitle-stream-language S ISO3|source"
)
  string(FIND "${CLI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing stream-level CLI marker: ${needle}")
  endif()
endforeach()

file(READ "${SOURCE_DIR}/include/bdmvauthor/model.hpp" model57)
file(READ "${SOURCE_DIR}/src/author.cpp" author57)
file(READ "${SOURCE_DIR}/src/gui.cpp" gui57)
file(READ "${SOURCE_DIR}/src/cli.cpp" cli57)
file(READ "${SOURCE_DIR}/src/project_file.cpp" project57)
set(all_source "${model57}${author57}${gui57}${cli57}${project57}")

foreach(needle
  "output_channels"
  "outputChannels"
  "ExternalSubtitle"
  "externalSubtitles"
  "Attach subtitle file"
  "Output channels"
  "--audio-stream-channels"
  "--subtitle-file"
  "resolved_audio_output_channels"
  "BDMV Author does not upmix"
  "external_source"
  "AuthorEngine::validate_encoding_profile_for_target")
  string(FIND "${all_source}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing subtitle/downmix marker: ${needle}")
  endif()
endforeach()

string(FIND "${GUI}" "if(e.audio_codec==AudioCodec::TrueHdAc3)return 18640" STALE_TRUEHD_GUI)
if(NOT STALE_TRUEHD_GUI EQUAL -1)
  message(FATAL_ERROR "GUI must not hard-reserve a fixed 18.64 Mb/s TrueHD rate; authoring measures the encoded peak")
endif()
