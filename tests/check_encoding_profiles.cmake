file(READ "${SOURCE_DIR}/include/bdmvauthor/model.hpp" MODEL)
file(READ "${SOURCE_DIR}/src/author.cpp" AUTHOR)
file(READ "${SOURCE_DIR}/src/cli.cpp" CLI)
file(READ "${SOURCE_DIR}/src/gui.cpp" GUI)

foreach(needle
    "enum class AudioCodec { Ac3, Lpcm, Dca, TrueHdAc3 }"
    "enum class VideoCodec { X264, Mpeg2, Hevc }"
    "VideoCodec video_codec = VideoCodec::X264"
    "int video_bitrate_kbps = 24000"
    "std::string x264_preset = \"medium\""
    "bool two_pass = false"
    "AudioCodec audio_codec = AudioCodec::Ac3"
    "EncodingProfile encoding;"
)
  string(FIND "${MODEL}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing encoding-profile marker: ${needle}")
  endif()
endforeach()

foreach(needle
    "--preset "
    "--pass 1 --stats"
    "--pass 2 --stats"
    "-c:v mpeg2video"
    "k -minrate "
    "k -maxrate " << peak_bitrate_limit_kbps << "k -bufsize 9781248"
    "-pass 1 -passlogfile"
    "V_MPEG-2, "
    "-c:a ac3"
    "pcm_s24le"
    "-c:a dca"
    "-c:a truehd"
    "merge_truehd_ac3_core"
    "A_DTS, "
    "A_LPCM, "
    "A_AC3, "
)
  string(FIND "${AUTHOR}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing encoder implementation marker: ${needle}")
  endif()
endforeach()

foreach(needle
    "--video-codec MODE"
    "--video-bitrate KBPS"
    "--x264-preset PRESET"
    "--two-pass"
    "--single-pass"
    "--audio-codec MODE"
    "--menu-video-codec MODE"
    "--menu-video-bitrate KBPS"
    "--menu-x264-preset PRESET"
    "--menu-two-pass"
    "--menu-audio-codec MODE"
    "--menu-audio FILE"
)
  string(FIND "${CLI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing CLI encoding option: ${needle}")
  endif()
endforeach()

foreach(needle
    "Titles — a title may be linked from any number of menu pages"
    "x264 / H.264"
    "FFmpeg MPEG-2"
    "AC-3 (default)"
    "LPCM"
    "DTS / DCA"
    "TrueHD + AC-3 core (experimental)"
    "Menu encoding"
    "Enable two-pass encoding"
    "Menu audio"
)
  string(FIND "${GUI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing GUI encoding control: ${needle}")
  endif()
endforeach()
