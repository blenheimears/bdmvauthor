file(READ "${SOURCE_DIR}/include/bdmvauthor/model.hpp" MODEL)
file(READ "${SOURCE_DIR}/src/author.cpp" AUTHOR)
file(READ "${SOURCE_DIR}/src/project_file.cpp" PROJECT)
file(READ "${SOURCE_DIR}/src/gui.cpp" GUI)
file(READ "${SOURCE_DIR}/src/cli.cpp" CLI)

foreach(needle
  "default_dvd_encoding_profile"
  "e.video_codec = VideoCodec::Mpeg2"
  "e.video_bitrate_kbps = 8000"
  "e.ac3_bitrate_kbps = 448"
  "video_codec_allowed_for_target"
  "audio_codec_allowed_for_target"
  "target_max_video_bitrate_kbps"
  "video_codec_max_bitrate_kbps"
  "target_max_ac3_bitrate_kbps"
  "EncodingProfile dvd_encoding = default_dvd_encoding_profile()"
  "EncodingProfile title_dvd_encoding = default_dvd_encoding_profile()"
  "EncodingProfile menu_dvd_encoding = default_dvd_encoding_profile()"
)
  string(FIND "${MODEL}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing target codec model marker: ${needle}")
  endif()
endforeach()

foreach(needle
  "validate_encoding_for_target"
  "must use HEVC/H.265 or AVC/H.264 video for Ultra HD Blu-ray"
  "must use MPEG-2 video for DVD-Video"
  "must use H.264/AVC or MPEG-2 video for standard Blu-ray"
  "DVD-Video audio must use AC-3 or LPCM"
  "DVD-Video AC-3 bitrate must not exceed 448 kb/s"
  "t.dvd_encoding"
  "menu.dvd_encoding"
)
  string(FIND "${AUTHOR}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing target codec validation marker: ${needle}")
  endif()
endforeach()

foreach(needle
  "kProjectFormatVersion = 24"
  "blurayEncoding"
  "uhdEncoding"
  "dvdEncoding"
  "default_dvd_encoding_profile()"
)
  string(FIND "${PROJECT}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing independent target profile persistence marker: ${needle}")
  endif()
endforeach()

foreach(needle
  "populate_video_combo"
  "populate_audio_combo"
  "FFmpeg MPEG-2 (DVD-Video)"
  "target != DiscTarget::DvdVideo480p"
  "target_max_ac3_bitrate_kbps(target)"
  "video_codec_max_bitrate_kbps(target,codec)"
  "RoleDvdProfile"
  "New titles — DVD"
  "Menus — DVD"
  "defaults/titleDvdEncoding"
  "defaults/menuDvdEncoding"
)
  string(FIND "${GUI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing target-restricted GUI marker: ${needle}")
  endif()
endforeach()

foreach(needle
  "next_title_dvd = default_dvd_encoding_profile()"
  "allows hevc or x264/AVC for Ultra HD Blu-ray"
  "allows only mpeg2 for DVD-Video"
  "allows only x264 or mpeg2 for standard Blu-ray"
  "allows only ac3 or lpcm for DVD-Video"
  "target_max_ac3_bitrate_kbps(p.target)"
  "video_codec_max_bitrate_kbps(p.target,codec)"
  "t.dvd_encoding = next_title_dvd"
  "std::map<DiscTarget,int> next_title_audio_bitrate_overrides"
)
  string(FIND "${CLI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing target-restricted CLI marker: ${needle}")
  endif()
endforeach()

message(STATUS "target-specific codec restrictions and independent profiles source checks ok")
