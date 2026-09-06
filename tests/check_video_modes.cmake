file(READ "${SOURCE_DIR}/include/bdmvauthor/model.hpp" MODEL)
file(READ "${SOURCE_DIR}/include/bdmvauthor/hdmv.hpp" HDMV_H)
file(READ "${SOURCE_DIR}/src/author.cpp" AUTHOR)
file(READ "${SOURCE_DIR}/src/hdmv.cpp" HDMV)
file(READ "${SOURCE_DIR}/src/project_file.cpp" PROJECT)
file(READ "${SOURCE_DIR}/src/gui.cpp" GUI)
file(READ "${SOURCE_DIR}/src/cli.cpp" CLI)

foreach(needle
  "int keyframe_interval_frames = 0"
  "std::string resolution = \"auto\""
  "std::string uhd_resolution = \"auto\""
  "std::string dvd_resolution = \"auto\""
  "std::string aspect_ratio = \"auto\""
  "std::string menu_resolution = \"1920x1080\""
  "std::string menu_uhd_resolution = \"1920x1080\""
  "std::string menu_dvd_resolution = \"720x480\""
  "std::string menu_aspect_ratio = \"16:9\""
  "std::string menu_uhd_aspect_ratio = \"16:9\""
  "std::string menu_dvd_aspect_ratio = \"16:9\"")
  string(FIND "${MODEL}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "video-mode model regression: missing ${needle}")
  endif()
endforeach()

foreach(needle
  "{3840,2160,\"16:9\"}"
  "{1920,1080,\"16:9\"}"
  "{1440,1080,\"16:9\"}"
  "{1280,720,\"16:9\"}"
  "{720,576,\"16:9\"}"
  "{720,576,\"4:3\"}"
  "{720,480,\"16:9\"}"
  "{720,480,\"4:3\"}"
  "{704,576,\"4:3\"}"
  "{352,576,\"4:3\"}"
  "{352,288,\"4:3\"}"
  "{704,480,\"4:3\"}"
  "{352,480,\"4:3\"}"
  "{352,240,\"4:3\"}"
  "choose_raster"
  "score < best_score-1e-9"
  "best_down && !down"
  "sample_aspect_ratio_for_raster"
  "scale_menu_from_design"
  "aspect_ratio == \"4:3\""
  "effective_keyframe_interval_frames"
  "keyframe/GOP interval"
  "--keyint "
  "-g \" << dvd_gop"
  "-g \" << mpeg2_gop"
  "source_matches_requested_keyframe_interval"
  "hdmv_frame_rate_code(menu_timing)"
  "if (target == DiscTarget::DvdVideo480p) return dvd_timing_is_pal(timing) ? 15 : 18;"
  "if (e.video_codec == VideoCodec::Mpeg2) return timing.frame_rate_value > 26.0 ? 15 : 12;"
  "return std::max(1, static_cast<int>(std::ceil(timing.frame_rate_value)));")
  string(FIND "${AUTHOR}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "video-mode backend regression: missing ${needle}")
  endif()
endforeach()

foreach(needle
  "keyframeIntervalFrames"
  "blurayResolution"
  "uhdResolution"
  "dvdResolution"
  "blurayAspectRatio"
  "uhdAspectRatio"
  "dvdAspectRatio"
  "blurayMenuResolution"
  "uhdMenuResolution"
  "dvdMenuResolution"
  "blurayMenuAspectRatio"
  "uhdMenuAspectRatio"
  "dvdMenuAspectRatio"
  "kProjectFormatVersion = 24")
  string(FIND "${PROJECT}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "video-mode persistence regression: missing ${needle}")
  endif()
endforeach()

foreach(needle
  "Menu resolution"
  "Menu aspect"
  "GOP / keyframe interval"
  "Resolution"
  "Aspect"
  "Auto (closest source)"
  "1920×1080"
  "1440×1080 anamorphic"
  "704×480 NTSC"
  "352×288 PAL"
  "Ultra HD Blu-ray primary video may be 3840×2160 or 1920×1080"
  "populate_aspect_choices"
  "titles_=new QTableWidget(0,19)")
  string(FIND "${GUI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "video-mode GUI regression: missing ${needle}")
  endif()
endforeach()

foreach(needle
  "--menu-resolution RES"
  "--menu-aspect ASPECT"
  "--title-resolution RES"
  "--title-aspect ASPECT"
  "--keyframe-interval N"
  "--menu-keyframe-interval N")
  string(FIND "${CLI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "video-mode CLI regression: missing ${needle}")
  endif()
endforeach()

foreach(needle "frame_rate_code = 1" "frame_rate_code>7")
  string(FIND "${HDMV_H}${HDMV}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "dynamic IG frame-rate regression: missing ${needle}")
  endif()
endforeach()


foreach(forbidden
  "{704,576,\"16:9\"}"
  "{352,576,\"16:9\"}"
  "{352,288,\"16:9\"}"
  "{704,480,\"16:9\"}"
  "{352,480,\"16:9\"}"
  "{352,240,\"16:9\"}")
  string(FIND "${AUTHOR}" "${forbidden}" pos)
  if(NOT pos EQUAL -1)
    message(FATAL_ERROR "reduced DVD raster must remain 4:3-only: ${forbidden}")
  endif()
endforeach()

message(STATUS "resolution/aspect/GOP source checks ok")
