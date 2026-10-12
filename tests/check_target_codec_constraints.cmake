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
  "must use H.264/AVC, MPEG-2, or experimental libvc1 VC-1 video for standard Blu-ray"
  "DVD-Video audio must use AC-3 or LPCM"
  "DVD-Video AC-3 bitrate must not exceed 448 kb/s"
  "encoding_for_target(t, p.target)"
  "encoding_for_target(menu, p.target)"
  "copy_audio_encoding_settings(effective, encoding_for_target(stream, p.target))"
)
  string(FIND "${AUTHOR}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing target codec validation marker: ${needle}")
  endif()
endforeach()

foreach(needle
  "kProjectFormatVersion = 29"
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
  "allows x264, mpeg2, or experimental vc1 for standard Blu-ray"
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


# Disc legality must not depend on whether process frontends were bundled into
# this particular BDMV Author build. The scheduler resolves tool capability.
string(FIND "${MODEL}" "BDMVAUTHOR_HAVE_VC1ENC" stale_vc1_legality)
string(FIND "${MODEL}" "BDMVAUTHOR_HAVE_HDMAENC" stale_hdma_legality)
if(NOT stale_vc1_legality EQUAL -1 OR NOT stale_hdma_legality EQUAL -1)
  message(FATAL_ERROR "disc-format legality is still coupled to build-time vc1enc/hdmaenc availability")
endif()

foreach(needle
  "local_worker_eligible_for_scheduler"
  "local_fallback_when_remote_unavailable"
  "remote_worker_pool_enabled"
  "any_remote_hardware_encoder_configured"
  "GUI availability is scheduler-wide"
  "const bool local=local_worker_eligible_for_scheduler()"
  "const bool remote=remote_worker_pool_enabled()")
  string(FIND "${GUI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing scheduler-aware GUI codec availability marker: ${needle}")
  endif()
endforeach()

message(STATUS "target-specific codec restrictions and independent profiles source checks ok")
