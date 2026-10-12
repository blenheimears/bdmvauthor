# Regression for the opt-in HDR10 downscale-before-tone-map CPU pipeline.
file(READ "${SOURCE_DIR}/src/author.cpp" author)
file(READ "${SOURCE_DIR}/src/gui.cpp" gui)
file(READ "${SOURCE_DIR}/src/cli.cpp" cli)
file(READ "${SOURCE_DIR}/include/bdmvauthor/author.hpp" header)
foreach(needle
    "tonemap_filter_backend == \"software-fast\""
    "return software_scale_fit(width,height,sar,dar,tools.scaling_filter_backend) + color.filter_prefix"
    "return chain + \"hwdownload,format=p010le,\" + color.filter_prefix"
    "color_plan.hdr10_to_sdr && tools.tonemap_filter_backend == \"software-fast\""
    "tools.tonemap_filter_backend == \"vaapi\""
    "fallback.tonemap_filter_backend = \"software-fast\""
    "scalingFilter=" "tonemapFilter=")
  string(FIND "${author}" "${needle}" at)
  if(at EQUAL -1)
    message(FATAL_ERROR "Missing fast HDR routing or independent cache key: ${needle}")
  endif()
endforeach()
foreach(needle "Software fast (downscale HDR before zscale/Hable)" "software-fast" "hardware/tonemapFilter")
  string(FIND "${gui}" "${needle}" at)
  if(at EQUAL -1)
    message(FATAL_ERROR "GUI must preserve opt-in fast software setting: ${needle}")
  endif()
endforeach()
foreach(needle "--filter-tonemap" "software-fast")
  string(FIND "${cli}" "${needle}" at)
  if(at EQUAL -1)
    message(FATAL_ERROR "CLI missing fast software setting: ${needle}")
  endif()
endforeach()
string(FIND "${header}" "tonemap_filter_backend = \"software\"" default_at)
if(default_at EQUAL -1)
  message(FATAL_ERROR "Fast HDR mode must NOT replace existing default")
endif()
# The fast path is still a software filter: it must not enter the VAAPI retry
# loop unless the scaling backend independently requests GPU processing.
string(FIND "${author}" "tools.tonemap_filter_backend == \"vaapi\";" hardware_at)
if(hardware_at EQUAL -1)
  message(FATAL_ERROR "software-fast incorrectly classified as hardware")
endif()
find_program(FFMPEG_BIN ffmpeg)
if(NOT FFMPEG_BIN)
  message(STATUS "ffmpeg absent; source/CLI checks only")
  return()
endif()
execute_process(COMMAND "${FFMPEG_BIN}" -hide_banner -filters OUTPUT_VARIABLE filters ERROR_VARIABLE ignored)
if(NOT filters MATCHES "[ \t]zscale[ \t]" OR NOT filters MATCHES "[ \t]tonemap[ \t]")
  message(STATUS "zscale/tonemap absent; source/CLI checks only")
  return()
endif()
# Exercise actual HDR10/PQ BT.2020 -> DVD anamorphic BT.709 SDR filter graph.
# Scaling first is deliberate: this is a user-selected speed/quality tradeoff.
foreach(shape "720;480;32/27" "720;576;64/45")
  list(GET shape 0 w)
  list(GET shape 1 h)
  list(GET shape 2 sar)
  set(vf "scale=w='if(gt(dar,16/9),${w},trunc((${h}*dar/(${sar}))/2)*2)':h='if(gt(dar,16/9),trunc((${w}*(${sar})/dar)/2)*2,${h})':flags=lanczos,zscale=pin=bt2020:tin=smpte2084:min=bt2020nc:rin=limited:t=linear:npl=100,format=gbrpf32le,zscale=p=bt709,tonemap=tonemap=hable:desat=0,zscale=t=bt709:m=bt709:r=limited,pad=${w}:${h}:(ow-iw)/2:(oh-ih)/2:color=black,setsar=${sar},format=yuv420p")
  execute_process(COMMAND "${FFMPEG_BIN}" -hide_banner -v error -f lavfi
      -i "testsrc2=size=1280x720:rate=1,format=yuv420p10le,setparams=range=limited:color_primaries=bt2020:color_trc=smpte2084:colorspace=bt2020nc"
      -frames:v 1 -vf "${vf}" -f null -
    RESULT_VARIABLE rc ERROR_VARIABLE error)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "HDR10 downscale-before-tonemap failed for ${w}x${h}: ${error}")
  endif()
endforeach()
message(STATUS "Fast HDR CPU path and NTSC/PAL filter processing verified")
