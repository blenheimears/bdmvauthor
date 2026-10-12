cmake_minimum_required(VERSION 3.20)
if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "SOURCE_DIR is required")
endif()

file(READ "${SOURCE_DIR}/CMakeLists.txt" ROOT_CMAKE)
file(READ "${SOURCE_DIR}/flake.nix" FLAKE)
file(READ "${SOURCE_DIR}/third_party/README-ffmpeg.md" README_FFMPEG)

foreach(needle
    "if(NOT APPLE AND BDMVAUTHOR_BUILD_BUNDLED_FFMPEG)"
    "set(BDMVAUTHOR_BUILD_BUNDLED_FFMPEG OFF CACHE BOOL"
    "if(APPLE AND BDMVAUTHOR_BUILD_BUNDLED_FFMPEG AND EXISTS"
    "if(APPLE)\n    # This executes the private FFmpeg configure helper"
    "add_test(NAME bdmvauthor-bundled-ffmpeg-encoder-list")
  string(FIND "${ROOT_CMAKE}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "bundled FFmpeg platform scope missing from CMake: ${needle}")
  endif()
endforeach()

foreach(needle
    "lib.optionals (!isDarwin)"
    "-DBDMVAUTHOR_BUILD_BUNDLED_FFMPEG=OFF"
    "lib.optionals isDarwin"
    "-DBDMVAUTHOR_BUILD_BUNDLED_FFMPEG=ON"
    "runtimeTools = [ pkgs.ffmpeg")
  string(FIND "${FLAKE}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "bundled FFmpeg platform scope missing from flake: ${needle}")
  endif()
endforeach()

foreach(needle
    "only used by the macOS app/DMG build"
    "builds ignore third_party/ffmpeg"
    "pkgs.ffmpeg")
  string(FIND "${README_FFMPEG}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "bundled FFmpeg platform documentation missing: ${needle}")
  endif()
endforeach()

message(STATUS "Bundled FFmpeg macOS-only platform scope regression passed")
