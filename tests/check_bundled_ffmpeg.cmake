cmake_minimum_required(VERSION 3.20)
if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "SOURCE_DIR is required")
endif()

file(READ "${SOURCE_DIR}/CMakeLists.txt" ROOT_CMAKE)
file(READ "${SOURCE_DIR}/flake.nix" FLAKE)
file(READ "${SOURCE_DIR}/scripts/configure_bundled_ffmpeg.sh" CONFIGURE_SCRIPT)
file(READ "${SOURCE_DIR}/scripts/build_bundled_ffmpeg.sh" BUILD_SCRIPT)
file(READ "${SOURCE_DIR}/third_party/README-ffmpeg.md" README_FFMPEG)
file(READ "${SOURCE_DIR}/src/gui.cpp" GUI_SOURCE)

foreach(needle
    "BDMVAUTHOR_BUILD_BUNDLED_FFMPEG"
    "third_party/ffmpeg/configure"
    "include(ExternalProject)"
    "ExternalProject_Add(bdmvauthor_bundled_ffmpeg"
    "scripts/configure_bundled_ffmpeg.sh"
    "scripts/build_bundled_ffmpeg.sh"
    "BDMVAUTHOR_FFMPEG_INSTALL_DIR"
    "CMAKE_C_COMPILER"
    "CMAKE_CXX_COMPILER"
    "CMAKE_AR"
    "CMAKE_RANLIB"
    "CMAKE_STRIP"
    "ffmpeg-install"
    "bin/ffmpeg"
    "bin/ffprobe")
  string(FIND "${ROOT_CMAKE}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "bundled FFmpeg CMake contract missing: ${needle}")
  endif()
endforeach()

foreach(needle
    "pkgs.gnumake"
    "pkgs.nasm"
    "pkgs.x264 pkgs.x265"
    "-DBDMVAUTHOR_BUILD_BUNDLED_FFMPEG=OFF"
    "-DBDMVAUTHOR_BUILD_BUNDLED_FFMPEG=ON"
    "\${base}/bin/ffmpeg"
    "\${base}/bin/ffprobe")
  string(FIND "${FLAKE}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "bundled FFmpeg Nix contract missing: ${needle}")
  endif()
endforeach()

foreach(needle
    "--cc=$cc"
    "--cxx=$cxx"
    "--ar=$ar"
    "--ranlib=$ranlib"
    "--strip=$strip"
    "FFmpeg never falls back to an unavailable host gcc"
    "--disable-encoders"
    "--enable-gpl"
    "--enable-libx264"
    "--enable-libx265"
    "--enable-videotoolbox"
    "rawvideo"
    "png"
    "ffvhuff"
    "mpeg2video"
    "ac3"
    "dca"
    "truehd"
    "pcm_s16le"
    "pcm_s24le"
    "pcm_s16be"
    "pcm_s24be"
    "pcm_s32le"
    "srt"
    "libx264"
    "libx265"
    "h264_videotoolbox"
    "hevc_videotoolbox"
    "--list-encoders"
    "for (i = 1; i <= NF; ++i) print $i")
  string(FIND "${CONFIGURE_SCRIPT}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "bundled FFmpeg encoder/configure contract missing: ${needle}")
  endif()
endforeach()

foreach(forbidden
    "--disable-decoders"
    "--disable-demuxers"
    "--disable-parsers"
    "--disable-protocols"
    "--disable-filters"
    "--disable-bsfs")
  string(FIND "${CONFIGURE_SCRIPT}" "${forbidden}" pos)
  if(NOT pos EQUAL -1)
    # The explanatory comment intentionally names these forbidden options.
    # Reject them only if they occur as an actual shell argument line.
    string(FIND "${CONFIGURE_SCRIPT}" "  ${forbidden}" arg_pos)
    if(NOT arg_pos EQUAL -1)
      message(FATAL_ERROR "bundled FFmpeg unexpectedly prunes input capability: ${forbidden}")
    endif()
  endif()
endforeach()

foreach(needle "NIX_BUILD_CORES" "CMAKE_BUILD_PARALLEL_LEVEL" "-j\"$cores\"")
  string(FIND "${BUILD_SCRIPT}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "bundled FFmpeg parallel build contract missing: ${needle}")
  endif()
endforeach()

foreach(needle
    "source releases do not redistribute FFmpeg"
    "third_party/ffmpeg/configure"
    "all decoders"
    "libx264"
    "libx265")
  string(FIND "${README_FFMPEG}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "bundled FFmpeg documentation contract missing: ${needle}")
  endif()
endforeach()
foreach(needle
    "A saved absolute path from a Nix/developer build can disappear"
    "if(sibling)"
    "sibling_info.isFile()&&sibling_info.isExecutable()")
  string(FIND "${GUI_SOURCE}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "packaged FFmpeg stale-path fallback missing: ${needle}")
  endif()
endforeach()

