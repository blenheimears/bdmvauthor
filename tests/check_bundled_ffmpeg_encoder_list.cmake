cmake_minimum_required(VERSION 3.20)
if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "SOURCE_DIR is required")
endif()

set(WORK "${CMAKE_CURRENT_BINARY_DIR}/bundled-ffmpeg-encoder-list-fixture")
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}/src" "${WORK}/build" "${WORK}/install")

# FFmpeg 9.0.x formats --list-encoders in multiple whitespace-separated columns.
# Keep rawvideo out of column 1 so the historical line-oriented grep would fail.
file(WRITE "${WORK}/src/configure" [=[#!/usr/bin/env bash
set -euo pipefail
if [[ "${1:-}" == "--list-encoders" ]]; then
cat <<'LIST'
a64multi                hevc_mediacodec         pcm_s16le
ac3                     hevc_videotoolbox       pcm_s24be
dca                     libx264                 pcm_s24le
ffvhuff                 libx265                 pcm_s16be
mpeg2video              hevc_videotoolbox       pcm_s32le
png                     srt                     rawvideo
truehd                  h264_videotoolbox       pcm_s16le
LIST
exit 0
fi
printf '%s\n' "$@" > "$(dirname "$0")/configure.args"
]=])
execute_process(COMMAND chmod +x "${WORK}/src/configure" COMMAND_ERROR_IS_FATAL ANY)

file(WRITE "${WORK}/tool" "#!/usr/bin/env sh\nexit 0\n")
execute_process(COMMAND chmod +x "${WORK}/tool" COMMAND_ERROR_IS_FATAL ANY)

execute_process(
  COMMAND "${SOURCE_DIR}/scripts/configure_bundled_ffmpeg.sh"
          "${WORK}/src" "${WORK}/build" "${WORK}/install"
          "${WORK}/tool" "${WORK}/tool" "${WORK}/tool" "${WORK}/tool" "${WORK}/tool"
  RESULT_VARIABLE RV
  OUTPUT_VARIABLE OUT
  ERROR_VARIABLE ERR)
if(NOT RV EQUAL 0)
  message(FATAL_ERROR "multi-column encoder-list regression failed (${RV}):\n${OUT}\n${ERR}")
endif()

file(READ "${WORK}/src/configure.args" ARGS)
foreach(required
    "--cc=${WORK}/tool"
    "--cxx=${WORK}/tool"
    "--ar=${WORK}/tool"
    "--ranlib=${WORK}/tool"
    "--strip=${WORK}/tool"
    "--disable-encoders"
    "--enable-encoder=rawvideo"
    "--enable-encoder=png"
    "--enable-encoder=ffvhuff"
    "--enable-encoder=mpeg2video"
    "--enable-encoder=ac3"
    "--enable-encoder=dca"
    "--enable-encoder=truehd"
    "--enable-encoder=pcm_s16le"
    "--enable-encoder=pcm_s24le"
    "--enable-encoder=pcm_s16be"
    "--enable-encoder=pcm_s24be"
    "--enable-encoder=pcm_s32le"
    "--enable-encoder=srt"
    "--enable-encoder=libx264"
    "--enable-encoder=libx265")
  string(FIND "${ARGS}" "${required}" POS)
  if(POS EQUAL -1)
    message(FATAL_ERROR "missing expected configure argument: ${required}")
  endif()
endforeach()

message(STATUS "FFmpeg multi-column --list-encoders parsing regression passed")
