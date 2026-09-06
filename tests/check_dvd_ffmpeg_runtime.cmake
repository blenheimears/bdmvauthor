find_program(FFMPEG_EXECUTABLE NAMES ffmpeg)
if(NOT FFMPEG_EXECUTABLE)
  message(STATUS "ffmpeg not available; skipping DVD MPEG-2 encoder smoke test")
  return()
endif()

set(OUT "${CMAKE_CURRENT_BINARY_DIR}/bdmvauthor-dvd-ffmpeg-smoke.m2v")
execute_process(
  COMMAND "${FFMPEG_EXECUTABLE}"
    -hide_banner -loglevel error -y
    -f lavfi -i "color=c=black:s=720x480:r=60000/1001"
    -t 0.25
    -vf "fps=60000/1001,tinterlace=mode=interleave_top,setfield=tff,format=yuv420p"
    -c:v mpeg2video -b:v 8000k -minrate 0 -maxrate 9000k -bufsize 1835008
    -g 18 -bf 2 -mpv_flags +strict_gop -aspect 16:9
    -flags +ildct+ilme -field_order tt
    -f mpeg2video "${OUT}"
  RESULT_VARIABLE RV
  ERROR_VARIABLE ERR
  OUTPUT_VARIABLE STDOUT
)
if(NOT RV EQUAL 0)
  message(FATAL_ERROR "representative DVD MPEG-2 command failed with ffmpeg ${RV}: ${ERR}")
endif()
if(NOT EXISTS "${OUT}")
  message(FATAL_ERROR "representative DVD MPEG-2 command produced no output")
endif()
file(SIZE "${OUT}" OUT_SIZE)
if(OUT_SIZE EQUAL 0)
  message(FATAL_ERROR "representative DVD MPEG-2 output is empty")
endif()
file(REMOVE "${OUT}")
