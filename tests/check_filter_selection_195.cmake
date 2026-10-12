if(NOT DEFINED SOURCE_DIR OR NOT DEFINED CLI)
  message(FATAL_ERROR "SOURCE_DIR and CLI required")
endif()
file(READ "${SOURCE_DIR}/src/author.cpp" AUTHOR)
file(READ "${SOURCE_DIR}/src/gui.cpp" GUI)
file(READ "${SOURCE_DIR}/src/cli.cpp" CLI_SOURCE)
foreach(marker
  "non_default_video_filter_requested(tools)"
  "tools.scaling_filter_backend == \"native\""
  "tonemap_vaapi=format=nv12:p=bt709:t=bt709:m=bt709"
  "Software scaling / zscale-Hable tone mapping was used instead"
  "fallback.scaling_filter_backend = tools.scaling_filter_backend == \"native\""
  "software_scale_fit(output_width,output_height,sar,target_dar,tools.scaling_filter_backend)")
  string(FIND "${AUTHOR}" "${marker}" index)
  if(index EQUAL -1)
    message(FATAL_ERROR "Missing selection/fallback/cache routing: ${marker}")
  endif()
endforeach()
foreach(marker
  "-filters"
  "ffmpeg_video_filter_names"
  "available.contains(\"scale_\"+value)"
  "available.contains(\"tonemap_vaapi\")"
  "FFmpeg native (scale + pad"
  "hardware/scalingFilter")
  string(FIND "${GUI}" "${marker}" index)
  if(index EQUAL -1)
    message(FATAL_ERROR "Missing FFmpeg filter capability discovery: ${marker}")
  endif()
endforeach()
foreach(marker "tools.scaling_filter_backend!=\"native\"" "--filter-scale must be software, native")
  string(FIND "${CLI_SOURCE}" "${marker}" index)
  if(index EQUAL -1)
    message(FATAL_ERROR "Missing native scaling CLI: ${marker}")
  endif()
endforeach()
execute_process(COMMAND "${CLI}" --help RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out MATCHES "native")
  message(FATAL_ERROR "CLI --help lacks native scaling choice")
endif()
execute_process(COMMAND "${CLI}" --filter-scale native -o should-not-exist.iso missing-input.mkv
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(rc EQUAL 0)
  message(FATAL_ERROR "Missing-input CLI was expected to fail")
endif()
if("${out}${err}" MATCHES "--filter-scale must be")
  message(FATAL_ERROR "Native scale backend incorrectly rejected")
endif()
find_program(FFMPEG_BIN ffmpeg)
if(FFMPEG_BIN)
  execute_process(COMMAND "${FFMPEG_BIN}" -hide_banner -loglevel error -y
    -f lavfi -i color=c=red:s=640x360:r=1 -frames:v 1
    -vf "scale=720:404,pad=720:480:0:38:color=black,setsar=32/27" -f null -
    RESULT_VARIABLE rv ERROR_VARIABLE diagnostic TIMEOUT 15)
  if(NOT rv EQUAL 0)
    message(FATAL_ERROR "FFmpeg native scale+pad test failed: ${diagnostic}")
  endif()
endif()
message(STATUS "Native scale/pad, cache separation, FFmpeg hardware-filter discovery and HDR route checked")
