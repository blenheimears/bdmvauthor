# Minimal BDMV Author build glue for the mplex source subset taken from
# mjpegtools SVN r3517.  Only mplex and support code directly required by mplex
# are retained in the BDMV Author source tree.
cmake_minimum_required(VERSION 3.20)

set(MPLEX_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${MPLEX_GENERATED_DIR}")
configure_file(config.h.cmake "${MPLEX_GENERATED_DIR}/config.h" COPYONLY)

add_executable(mplex
  mplex/main.cpp
  mplex/ac3strm_in.cpp
  mplex/audiostrm_out.cpp
  mplex/bits.cpp
  mplex/decodebufmodel.cpp
  mplex/dtsstrm_in.cpp
  mplex/inputstrm.cpp
  mplex/interact.cpp
  mplex/lpcmstrm_in.cpp
  mplex/mpastrm_in.cpp
  mplex/multiplexor.cpp
  mplex/padstrm.cpp
  mplex/stillsstream.cpp
  mplex/stream_params.cpp
  mplex/systems.cpp
  mplex/videostrm_in.cpp
  mplex/videostrm_out.cpp
  mplex/subpstream.cpp
  utils/mjpeg_logging.c
  utils/mpegconsts.c
  utils/yuv4mpeg_ratio.c
)

target_compile_features(mplex PRIVATE cxx_std_11)
target_compile_definitions(mplex PRIVATE HAVE_CONFIG_H=1)
target_include_directories(mplex PRIVATE
  "${MPLEX_GENERATED_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/utils"
  "${CMAKE_CURRENT_SOURCE_DIR}/mplex"
)
# This is old upstream source and intentionally keeps its original warning
# profile. The parent project's warnings-as-errors setting applies to BDMV
# Author and its maintained platform ports, not unchanged historical mplex code.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(mplex PRIVATE -Wno-deprecated-declarations)
endif()
if(NOT WIN32)
  target_link_libraries(mplex PRIVATE m)
endif()

# BDMV Author resolves bundled helpers beside its own executable first.
set_target_properties(mplex PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
