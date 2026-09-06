if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "SOURCE_DIR not supplied")
endif()
file(READ "${SOURCE_DIR}/include/bdmvauthor/author.hpp" HEADER)
file(READ "${SOURCE_DIR}/src/author.cpp" AUTHOR)
file(READ "${SOURCE_DIR}/src/gui.cpp" GUI)
file(READ "${SOURCE_DIR}/src/cli.cpp" CLI)

foreach(needle
  "VideoEncoderProvider { Ffmpeg, Standalone }"
  "std::string x264 = \"x264\""
  "std::string x265 = \"x265\""
  "VideoEncoderProvider h264_provider = VideoEncoderProvider::Ffmpeg"
  "VideoEncoderProvider hevc_provider = VideoEncoderProvider::Ffmpeg")
  string(FIND "${HEADER}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing external-tool/provider API marker: ${needle}")
  endif()
endforeach()

foreach(needle
  "tools/ffmpeg" "tools/ffprobe" "tools/x264" "tools/x265" "tools/tsmuxer"
  "tools/dvdauthor" "tools/spumux" "tools/mplex" "tools/mkisofs"
  "tools/h264Provider" "tools/hevcProvider"
  "QStandardPaths::findExecutable"
  "resolve_tool(config.tsmuxer,\"tsmuxer\",true)"
  "External tools and encoders"
  "FFmpeg/libx264 (preferred)" "Standalone x264 (preferred)"
  "FFmpeg/libx265 (preferred)" "Standalone x265 (preferred)"
  "-encoders" "libx264" "libx265" "mpeg2video" "ac3" "dca" "truehd"
  "standalone x264 will be used instead"
  "standalone x265 will be used instead"
  "Ultra HD Blu-ray remains available only for legal 1080p AVC/H.264 projects"
  "DVD-Video authoring is disabled"
  "set_combo_item_enabled"
  "target_available"
  "video_codec_available"
  "audio_codec_available"
  "Unavailable codec choices and targets have been disabled")
  string(FIND "${GUI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing runtime tool-discovery GUI marker: ${needle}")
  endif()
endforeach()

foreach(needle
  "tools.h264_provider == VideoEncoderProvider::Standalone"
  "tools.hevc_provider == VideoEncoderProvider::Standalone"
  "-c:v libx264"
  "shq_s(tools.x264)"
  "shq_s(tools.x265)"
  "tools.h264_provider==VideoEncoderProvider::Ffmpeg"
  "bdmvauthor-video-cache-v13")
  string(FIND "${AUTHOR}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing encoder-provider author marker: ${needle}")
  endif()
endforeach()
foreach(needle "--x265 PATH" "--h264-provider MODE" "--hevc-provider MODE")
  string(FIND "${CLI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing encoder-provider CLI marker: ${needle}")
  endif()
endforeach()
message(STATUS "external-tool discovery/provider source checks ok")
