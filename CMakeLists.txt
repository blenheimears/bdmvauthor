cmake_minimum_required(VERSION 3.20)

# VERSION is the single source of truth for the BDMV Author release version.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" BDMVAUTHOR_VERSION)
string(STRIP "${BDMVAUTHOR_VERSION}" BDMVAUTHOR_VERSION)
if(NOT BDMVAUTHOR_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
  message(FATAL_ERROR "Invalid BDMV Author VERSION: '${BDMVAUTHOR_VERSION}'")
endif()
project(bdmvauthor VERSION "${BDMVAUTHOR_VERSION}" LANGUAGES C CXX)

option(BDMVAUTHOR_BUILD_GUI "Build the Qt 6 GUI when Qt is available" ON)
option(BDMVAUTHOR_WARNINGS_AS_ERRORS "Treat BDMV Author warnings as errors" OFF)
option(BDMVAUTHOR_BUILD_BUNDLED_TSMUXER "Build the vendored tsMuxer helper" ON)
option(BDMVAUTHOR_BUILD_BUNDLED_MKISOFS "Build the vendored cdrtools mkisofs helper" ON)
if(WIN32 OR APPLE)
  set(BDMVAUTHOR_BUNDLED_MPLEX_DEFAULT ON)
else()
  set(BDMVAUTHOR_BUNDLED_MPLEX_DEFAULT OFF)
endif()
option(BDMVAUTHOR_BUILD_BUNDLED_MPLEX "Build the vendored mplex helper" ${BDMVAUTHOR_BUNDLED_MPLEX_DEFAULT})
if(WIN32 OR APPLE)
  option(BDMVAUTHOR_BUILD_BUNDLED_DVDAUTHOR "Build the vendored dvdauthor/spumux helpers" ON)
else()
  option(BDMVAUTHOR_BUILD_BUNDLED_DVDAUTHOR "Build the vendored dvdauthor/spumux helpers" OFF)
endif()

if(BDMVAUTHOR_BUILD_BUNDLED_TSMUXER)
  # Only the tsMuxer CLI is needed.  Keep the upstream GUI source vendored but
  # do not pull a second GUI or its Qt requirements into BDMV Author.
  set(TSMUXER_GUI OFF CACHE BOOL "Build the vendored tsMuxer GUI" FORCE)
  set(TSMUXER_STATIC_BUILD OFF CACHE BOOL "Build vendored tsMuxer statically" FORCE)
  set(TSMUXER_VERSION_OVERRIDE "git-c6b1186-bdmvauthor-210" CACHE STRING "Vendored tsMuxer version" FORCE)
  add_subdirectory(third_party/tsmuxer third_party/tsmuxer-build)
  # Put the private helper beside bdmvauthor/bdmvauthor-cli in build trees as
  # well as installed trees.  AuthorEngine resolves this sibling first.
  set_target_properties(tsmuxer PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
  )
endif()

if(BDMVAUTHOR_BUILD_BUNDLED_DVDAUTHOR)
  add_subdirectory(third_party/dvdauthor third_party/dvdauthor-build)
endif()

if(BDMVAUTHOR_BUILD_BUNDLED_MKISOFS)
  add_subdirectory(third_party/mkisofs third_party/mkisofs-build)
endif()

if(BDMVAUTHOR_BUILD_BUNDLED_MPLEX)
  add_subdirectory(third_party/mplex third_party/mplex-build)
endif()

# bdsup2sub is a separately distributed C++ command-line utility. Do not ship
# its sources in BDMV Author: when a builder places its CMake project under
# third_party/bdsup2sub, compile the CLI automatically beside BDMV Author.
option(BDMVAUTHOR_BUILD_BUNDLED_BDSUP2SUB "Build optional native bitmap subtitle converter when builder supplies its source" ON)
if(BDMVAUTHOR_BUILD_BUNDLED_BDSUP2SUB AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/bdsup2sub/CMakeLists.txt")
  set(BDSUP2SUB_NATIVE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  add_subdirectory(third_party/bdsup2sub third_party/bdsup2sub-build)
  if(TARGET bdsup2sub)
    set_target_properties(bdsup2sub PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    message(STATUS "BDMV Author: native bdsup2sub helper enabled from third_party/bdsup2sub")
  else()
    message(FATAL_ERROR "third_party/bdsup2sub must create the bdsup2sub executable target")
  endif()
else()
  message(STATUS "BDMV Author: optional native bdsup2sub source absent; runtime PATH lookup remains available")
endif()

# On macOS, builders may drop an unmodified FFmpeg source tree into
# third_party/ffmpeg. Build ffmpeg/ffprobe out of tree as private sibling
# processes. Decoder/demuxer/parser support remains at FFmpeg defaults; only
# the encoder set is pruned to the codecs BDMV Author actually uses. Source
# releases intentionally do not redistribute FFmpeg itself.
if(APPLE)
  set(BDMVAUTHOR_BUNDLED_FFMPEG_DEFAULT ON)
else()
  set(BDMVAUTHOR_BUNDLED_FFMPEG_DEFAULT OFF)
endif()
option(BDMVAUTHOR_BUILD_BUNDLED_FFMPEG "Build builder-supplied third_party/ffmpeg on macOS" ${BDMVAUTHOR_BUNDLED_FFMPEG_DEFAULT})
# The private-source FFmpeg path exists solely to make the self-contained macOS
# .app/.dmg independent of a Nix-store FFmpeg. Linux must always use the normal
# runtime/system FFmpeg path, even if a third_party/ffmpeg tree happens to be
# present or this cache option is passed accidentally.
if(NOT APPLE AND BDMVAUTHOR_BUILD_BUNDLED_FFMPEG)
  message(STATUS "BDMV Author FFmpeg: ignoring BDMVAUTHOR_BUILD_BUNDLED_FFMPEG on non-macOS")
  set(BDMVAUTHOR_BUILD_BUNDLED_FFMPEG OFF CACHE BOOL "Build builder-supplied third_party/ffmpeg on macOS" FORCE)
endif()

set(BDMVAUTHOR_HAVE_BUNDLED_FFMPEG OFF)
if(APPLE AND BDMVAUTHOR_BUILD_BUNDLED_FFMPEG AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg/configure")
    include(ExternalProject)
    find_program(BDMVAUTHOR_FFMPEG_MAKE NAMES gmake make REQUIRED)
    set(BDMVAUTHOR_FFMPEG_BUILD_DIR "${CMAKE_BINARY_DIR}/third_party/ffmpeg-build")
    set(BDMVAUTHOR_FFMPEG_INSTALL_DIR "${CMAKE_BINARY_DIR}/third_party/ffmpeg-install")
    ExternalProject_Add(bdmvauthor_bundled_ffmpeg
      SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg"
      BINARY_DIR "${BDMVAUTHOR_FFMPEG_BUILD_DIR}"
      INSTALL_DIR "${BDMVAUTHOR_FFMPEG_INSTALL_DIR}"
      DOWNLOAD_COMMAND ""
      UPDATE_COMMAND ""
      PATCH_COMMAND ""
      CONFIGURE_COMMAND
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/configure_bundled_ffmpeg.sh"
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg"
        "${BDMVAUTHOR_FFMPEG_BUILD_DIR}"
        "${BDMVAUTHOR_FFMPEG_INSTALL_DIR}"
        "${CMAKE_C_COMPILER}"
        "${CMAKE_CXX_COMPILER}"
        "${CMAKE_AR}"
        "${CMAKE_RANLIB}"
        "${CMAKE_STRIP}"
      BUILD_COMMAND
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/build_bundled_ffmpeg.sh"
        "${BDMVAUTHOR_FFMPEG_MAKE}"
      INSTALL_COMMAND "${BDMVAUTHOR_FFMPEG_MAKE}" install
      BUILD_BYPRODUCTS
        "${BDMVAUTHOR_FFMPEG_INSTALL_DIR}/bin/ffmpeg"
        "${BDMVAUTHOR_FFMPEG_INSTALL_DIR}/bin/ffprobe"
      USES_TERMINAL_CONFIGURE TRUE
      USES_TERMINAL_BUILD TRUE
      USES_TERMINAL_INSTALL TRUE)
    set(BDMVAUTHOR_HAVE_BUNDLED_FFMPEG ON)
    message(STATUS "BDMV Author FFmpeg: builder-supplied third_party/ffmpeg will be built with all decoders and the required encoder subset")
elseif(APPLE AND BDMVAUTHOR_BUILD_BUNDLED_FFMPEG)
  message(STATUS "BDMV Author FFmpeg: third_party/ffmpeg not found; using runtime-discovered FFmpeg/ffprobe")
endif()

# VC-1 is deliberately optional and experimental.  If a libvc1 source tree is
# present (supplied separately by the person building BDMV Author), build only
# its static vc1enc frontend as a private sibling process. BDMV Author never
# links against libvc1. Removing third_party/libvc1 must leave a fully
# functional build with VC-1 omitted. Source releases intentionally do not ship
# libvc1 themselves.
option(BDMVAUTHOR_BUILD_BUNDLED_LIBVC1 "Build experimental VC-1 support when builder-supplied third_party/libvc1 is present" ON)
set(BDMVAUTHOR_HAVE_VC1ENC OFF)
if(BDMVAUTHOR_BUILD_BUNDLED_LIBVC1)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libvc1/CMakeLists.txt")
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libvc1/VERSION" BDMVAUTHOR_LIBVC1_VERSION LIMIT_COUNT 1)
    if(BDMVAUTHOR_LIBVC1_VERSION VERSION_LESS "0.2.35")
      message(WARNING "BDMV Author VC-1 requires libvc1/vc1enc 0.2.35 or newer for raw primary output; found ${BDMVAUTHOR_LIBVC1_VERSION}; continuing without VC-1 support")
    else()
    set(LIBVC1_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(LIBVC1_BUILD_STATIC ON CACHE BOOL "" FORCE)
    set(LIBVC1_BUILD_FRONTEND ON CACHE BOOL "" FORCE)
    set(LIBVC1_FRONTEND_LINK_STATIC ON CACHE BOOL "" FORCE)
    set(LIBVC1_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    # BDMV Author consumes vc1enc raw elementary output directly, so avoid
    # pulling in libmatroska/libebml just to provide the private encoder helper.
    set(LIBVC1_ENABLE_MATROSKA OFF CACHE BOOL "" FORCE)
    add_subdirectory(third_party/libvc1 third_party/libvc1-build EXCLUDE_FROM_ALL)
    if(TARGET vc1enc)
      set_target_properties(vc1enc PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
      set(BDMVAUTHOR_HAVE_VC1ENC ON)
      message(STATUS "BDMV Author VC-1: experimental support enabled using builder-supplied libvc1/vc1enc")
    else()
      message(WARNING "third_party/libvc1 exists but did not define vc1enc; continuing without VC-1 support")
    endif()
    endif()
  else()
    message(STATUS "BDMV Author VC-1: third_party/libvc1 not found; continuing without VC-1 support")
  endif()
endif()

# Builder-supplied eac371enc is entirely optional. It is used only as a
# process helper for Blu-ray/UHD AC-3 output that resolves to 7.1 channels.
# The helper is deliberately built without libsndfile because BDMV Author
# feeds canonical 48-kHz 7.1 f32le PCM through stdin.
option(BDMVAUTHOR_ENABLE_EAC371ENC "Build optional builder-supplied third_party/eac371enc frontend when present" ON)
set(BDMVAUTHOR_HAVE_EAC371ENC OFF)
if(BDMVAUTHOR_ENABLE_EAC371ENC)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/eac371enc/CMakeLists.txt")
    set(EAC371_ENABLE_SNDFILE_INPUT OFF CACHE BOOL "" FORCE)
    set(EAC371_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    add_subdirectory(third_party/eac371enc third_party/eac371enc-build EXCLUDE_FROM_ALL)
    if(TARGET eac371enc)
      set_target_properties(eac371enc PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
      set(BDMVAUTHOR_HAVE_EAC371ENC ON)
      message(STATUS "BDMV Author eac371enc: optional raw-PCM 7.1 AC-3/E-AC-3 frontend enabled")
    else()
      message(WARNING "third_party/eac371enc did not provide eac371enc; 7.1 AC-3 support disabled")
    endif()
  else()
    message(STATUS "BDMV Author eac371enc: source absent; 7.1 AC-3 support disabled (<=5.1 FFmpeg AC-3 unaffected)")
  endif()
endif()

# Builder-supplied libhdma is entirely optional. Only hdmaenc is used at
# runtime, as a sibling process; never link hdma into bdmvauthor_core.
option(BDMVAUTHOR_BUILD_BUNDLED_LIBHDMA "Build optional experimental libhdma frontend" ON)
set(BDMVAUTHOR_HAVE_HDMAENC OFF)
if(BDMVAUTHOR_BUILD_BUNDLED_LIBHDMA)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libhdma/CMakeLists.txt")
    # Older frontends may emit out-of-range XLL segments. Reject them
    # at configure time rather than silently encoding with older framing.
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libhdma/VERSION")
      message(FATAL_ERROR "Builder-supplied libhdma must declare VERSION >= 0.0.37 for the decorrelation-on default")
    endif()
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libhdma/VERSION" HDMA_BUNDLED_VERSION LIMIT_COUNT 1)
    if(NOT HDMA_BUNDLED_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
      message(FATAL_ERROR "Builder-supplied libhdma VERSION is not a semantic version: '${HDMA_BUNDLED_VERSION}'")
    endif()
    if(HDMA_BUNDLED_VERSION VERSION_LESS "0.0.37")
      message(FATAL_ERROR "libhdma ${HDMA_BUNDLED_VERSION} predates the decorrelation-on default; supply libhdma 0.0.37 or newer")
    endif()
    set(LIBHDMA_TESTS OFF CACHE BOOL "" FORCE)
    set(LIBHDMA_ENABLE_SNDFILE_INPUT OFF CACHE BOOL "" FORCE)
    add_subdirectory(third_party/libhdma third_party/libhdma-build EXCLUDE_FROM_ALL)
    if(TARGET hdmaenc)
      set_target_properties(hdmaenc PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
      set(BDMVAUTHOR_HAVE_HDMAENC ON)
      message(STATUS "BDMV Author libhdma: experimental raw-PCM hdmaenc frontend enabled")
    else()
      message(WARNING "third_party/libhdma did not provide hdmaenc; support disabled")
    endif()
  else()
    message(STATUS "BDMV Author libhdma: source absent; support disabled")
  endif()
endif()

add_library(bdmvauthor_core
    src/author.cpp
    src/bluray_preflight.cpp
    src/dvd_subpicture_palette.cpp
    src/menu_image.cpp
    src/compliance.cpp
    src/audio_tools.cpp
    src/progress.cpp
    src/media_fingerprint.cpp
    src/hdmv.cpp
    src/font_renderer.cpp
    third_party/udf25mkiso/udf_writer.cpp
)
target_compile_features(bdmvauthor_core PUBLIC cxx_std_20)
target_include_directories(bdmvauthor_core PUBLIC include PRIVATE third_party/udf25mkiso)
target_compile_definitions(bdmvauthor_core PUBLIC BDMVAUTHOR_VERSION=\"${PROJECT_VERSION}\")
find_package(OpenSSL REQUIRED)
target_link_libraries(bdmvauthor_core PRIVATE OpenSSL::Crypto)

# Menu button text is rendered in the core so CLI and GUI author identical IG
# objects.  On Linux/NixOS use Fontconfig to resolve families and FreeType to
# rasterize glyphs.  A small built-in fallback remains for minimal builds.
find_package(Freetype QUIET)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(FONTCONFIG QUIET IMPORTED_TARGET fontconfig)
endif()
if(TARGET PkgConfig::FONTCONFIG)
  target_link_libraries(bdmvauthor_core PRIVATE PkgConfig::FONTCONFIG)
  target_compile_definitions(bdmvauthor_core PRIVATE BDMVAUTHOR_HAVE_FONTCONFIG=1)
endif()
if(Freetype_FOUND AND TARGET PkgConfig::FONTCONFIG)
  target_link_libraries(bdmvauthor_core PRIVATE Freetype::Freetype)
  target_compile_definitions(bdmvauthor_core PRIVATE BDMVAUTHOR_HAVE_FREETYPE_FONTCONFIG=1)
  message(STATUS "BDMV Author menu fonts: FreeType + Fontconfig; DVD subtitle families resolve to font files")
elseif(TARGET PkgConfig::FONTCONFIG)
  message(STATUS "BDMV Author menu fonts: built-in 5x7 fallback; DVD subtitle families resolve with Fontconfig")
else()
  message(STATUS "BDMV Author menu fonts: built-in 5x7 fallback (FreeType/Fontconfig development files not found); DVD subtitle fonts must be file paths")
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(bdmvauthor_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
  if(BDMVAUTHOR_WARNINGS_AS_ERRORS)
    target_compile_options(bdmvauthor_core PRIVATE -Werror)
  endif()
elseif(MSVC)
  target_compile_options(bdmvauthor_core PRIVATE /W4 /permissive-)
endif()
if(TARGET tsmuxer)
  add_dependencies(bdmvauthor_core tsmuxer)
endif()
if(BDMVAUTHOR_HAVE_VC1ENC)
  target_compile_definitions(bdmvauthor_core PUBLIC BDMVAUTHOR_HAVE_VC1ENC=1)
  add_dependencies(bdmvauthor_core vc1enc)
endif()
if(BDMVAUTHOR_HAVE_HDMAENC)
  target_compile_definitions(bdmvauthor_core PUBLIC BDMVAUTHOR_HAVE_HDMAENC=1)
  add_dependencies(bdmvauthor_core hdmaenc)
endif()
if(BDMVAUTHOR_HAVE_EAC371ENC)
  target_compile_definitions(bdmvauthor_core PUBLIC BDMVAUTHOR_HAVE_EAC371ENC=1)
  add_dependencies(bdmvauthor_core eac371enc)
endif()

add_executable(bdmvauthor-cli src/cli.cpp)
target_link_libraries(bdmvauthor-cli PRIVATE bdmvauthor_core)

if(BDMVAUTHOR_BUILD_GUI)
  find_package(Qt6 6.4 COMPONENTS Core Gui Widgets Concurrent QUIET)
  if(Qt6_FOUND)
    set(CMAKE_AUTOMOC ON)
    qt_add_executable(bdmvauthor src/gui.cpp src/project_file.cpp)
    if(WIN32)
      # Embed the approved application icon into the PE executable itself.
      # QApplication::setWindowIcon() covers Qt-owned windows at runtime, while
      # Explorer, taskbar shortcuts, thumbnails, and installed-app surfaces read
      # the executable's native icon resource.
      set(BDMVAUTHOR_WINDOWS_ICON "${CMAKE_CURRENT_SOURCE_DIR}/resources/bdmvauthor.ico")
      configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/bdmvauthor.rc.in"
        "${CMAKE_CURRENT_BINARY_DIR}/bdmvauthor.rc"
        @ONLY)
      target_sources(bdmvauthor PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/bdmvauthor.rc")

      # GUI-subsystem executable: launching bdmvauthor.exe from Explorer should
      # not open an otherwise unused console window.
      set_target_properties(bdmvauthor PROPERTIES WIN32_EXECUTABLE TRUE)
    endif()
    target_link_libraries(bdmvauthor PRIVATE bdmvauthor_core Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Concurrent)
  else()
    message(STATUS "Qt 6 development files not found; building CLI/core only")
  endif()
endif()

include(CTest)
if(BUILD_TESTING)
  add_executable(bdmvauthor-dedicated-pcr-208-test tests/test_bluray_pcr_preflight.cpp src/bluray_preflight.cpp)
  target_include_directories(bdmvauthor-dedicated-pcr-208-test PRIVATE include)
  target_compile_features(bdmvauthor-dedicated-pcr-208-test PRIVATE cxx_std_20)
  add_test(NAME bdmvauthor-dedicated-pcr-208-runtime COMMAND bdmvauthor-dedicated-pcr-208-test)
  add_test(NAME bdmvauthor-dedicated-pcr-208-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_dedicated_pcr_208.cmake)
  add_test(NAME bdmvauthor-hdma-name-agnostic-210-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_hdma_name_210.cmake)
  add_test(NAME bdmvauthor-pat-pmt-discovery-209-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_pat_discovery_209.cmake)
  add_test(NAME bdmvauthor-bdcmf-segment-alignment-207-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_bdcmf_segment_alignment_207.cmake)
  add_test(NAME bdmvauthor-clpi-pcr-pid-205-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_clpi_pcr_pid_205.cmake)
  add_test(NAME bdmvauthor-dvd-bitmap-clut-202 COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_dvd_bitmap_clut_202.cmake)
  add_executable(bdmvauthor-dvd-subpicture-palette-test tests/check_dvd_subpicture_palette.cpp)
  target_link_libraries(bdmvauthor-dvd-subpicture-palette-test PRIVATE bdmvauthor_core)
  add_test(NAME bdmvauthor-dvd-subpicture-palette-runtime COMMAND bdmvauthor-dvd-subpicture-palette-test)
  add_executable(bdmvauthor-menu-layout-tests tests/test_menu_layout.cpp)
  target_include_directories(bdmvauthor-menu-layout-tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
  target_compile_features(bdmvauthor-menu-layout-tests PRIVATE cxx_std_20)
  add_test(NAME bdmvauthor-menu-layout COMMAND bdmvauthor-menu-layout-tests)
  add_test(NAME bdmvauthor-menu-layout-firstpass-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_menu_layout_fast_firstpass.cmake)
  add_test(NAME bdmvauthor-canvas-undo-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_canvas_undo.cmake)
  add_test(NAME bdmvauthor-storage-io-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_storage_io.cmake)
  add_test(NAME bdmvauthor-hardware-video-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_hardware_video.cmake)
  add_test(NAME bdmvauthor-hardware-filters COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -DCLI=$<TARGET_FILE:bdmvauthor-cli> -P ${CMAKE_SOURCE_DIR}/tests/check_hardware_filters.cmake)
  add_test(NAME bdmvauthor-filter-selection-native-195 COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -DCLI=$<TARGET_FILE:bdmvauthor-cli> -P ${CMAKE_SOURCE_DIR}/tests/check_filter_selection_195.cmake)
  add_test(NAME bdmvauthor-ffmpeg-filter-detection-196 COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_ffmpeg_filter_detection_196.cmake)
  add_test(NAME bdmvauthor-vaapi-dvd-retry-197 COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_vaapi_dvd_retry_197.cmake)
  add_test(NAME bdmvauthor-fast-hdr-tonemap-198 COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_fast_hdr_tonemap_198.cmake)
  add_executable(bdmvauthor-menu-image-tests tests/test_menu_image.cpp)
  target_link_libraries(bdmvauthor-menu-image-tests PRIVATE bdmvauthor_core)
  find_program(BDMVAUTHOR_TEST_FFMPEG NAMES ffmpeg)
  if(BDMVAUTHOR_TEST_FFMPEG)
    add_test(NAME bdmvauthor-menu-image COMMAND bdmvauthor-menu-image-tests "${BDMVAUTHOR_TEST_FFMPEG}")
  else()
    add_test(NAME bdmvauthor-menu-image COMMAND bdmvauthor-menu-image-tests)
  endif()
  add_test(NAME bdmvauthor-menu-image-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_menu_image_source.cmake)
  add_executable(bdmvauthor-tests tests/test_core.cpp)
  target_link_libraries(bdmvauthor-tests PRIVATE bdmvauthor_core)
  add_test(NAME bdmvauthor-core COMMAND bdmvauthor-tests)
  add_executable(bdmvauthor-preloaded-igs-window-test tests/test_preloaded_igs_window.cpp)
  add_test(NAME bdmvauthor-preloaded-igs-window-runtime COMMAND bdmvauthor-preloaded-igs-window-test)
  add_test(NAME bdmvauthor-cmf-igs-204-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_cmf_ig_204.cmake)
  if(BDMVAUTHOR_HAVE_HDMAENC)
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
      add_test(NAME bdmvauthor-hdma-stdio-204-runtime COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_libhdma_stdio.py $<TARGET_FILE:hdmaenc>)
    endif()
  endif()


  # Reproduce the native-Windows authoring worker's constrained stack.  The
  # UDF writer must stream large payloads without placing its 1 MiB I/O buffer
  # on the worker stack.
  add_executable(bdmvauthor-udf-small-stack-test
    tests/test_udf_small_stack.cpp
    third_party/udf25mkiso/udf_writer.cpp)
  target_compile_features(bdmvauthor-udf-small-stack-test PRIVATE cxx_std_20)
  target_include_directories(bdmvauthor-udf-small-stack-test PRIVATE third_party/udf25mkiso)
  if(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(bdmvauthor-udf-small-stack-test PRIVATE Threads::Threads)
  endif()
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(bdmvauthor-udf-small-stack-test PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    if(BDMVAUTHOR_WARNINGS_AS_ERRORS)
      target_compile_options(bdmvauthor-udf-small-stack-test PRIVATE -Werror)
    endif()
  endif()
  add_test(NAME bdmvauthor-udf-small-stack-runtime COMMAND bdmvauthor-udf-small-stack-test)
  add_test(NAME bdmvauthor-version-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_version_source.cmake)
  add_test(NAME bdmvauthor-native-igs-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_native_igs.cmake)
  add_test(NAME bdmvauthor-encoding-profile-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_encoding_profiles.cmake)
  add_test(NAME bdmvauthor-macos-title-scrollbar-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_macos_title_scrollbar.cmake)
add_test(NAME bdmvauthor-dvd-ffmpeg-rate-control-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_dvd_ffmpeg_rate_control.cmake)
add_test(NAME bdmvauthor-mpeg2-field-order-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_mpeg2_field_order.cmake)
add_test(NAME bdmvauthor-dvd-ffmpeg-runtime COMMAND ${CMAKE_COMMAND} -P ${CMAKE_SOURCE_DIR}/tests/check_dvd_ffmpeg_runtime.cmake)
  add_test(NAME bdmvauthor-button-style-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_button_styles.cmake)
  add_test(NAME bdmvauthor-button-image-layout-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_button_images_layout.cmake)
  add_test(NAME bdmvauthor-progress-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_progress_source.cmake)
  add_test(NAME bdmvauthor-audio-preflight-progress-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_audio_preflight_progress.cmake)
  add_test(NAME bdmvauthor-menu-navigation-editor-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_menu_navigation_editor.cmake)
  add_test(NAME bdmvauthor-navigation-sequences-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_navigation_sequences.cmake)
  add_test(NAME bdmvauthor-submenus-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_submenus.cmake)
  add_test(NAME bdmvauthor-labels-auto-submenu-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_labels_auto_submenu.cmake)
  add_test(NAME bdmvauthor-project-files-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_project_files.cmake)
  add_test(NAME bdmvauthor-qvariant-gui-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_qvariant_gui.cmake)
  add_test(NAME bdmvauthor-encode-cache-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_encode_cache.cmake)
  add_test(NAME bdmvauthor-audio-probe-directories-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_audio_probe_directories.cmake)
  add_test(NAME bdmvauthor-menu-auto-duration-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_menu_auto_duration.cmake)
  add_test(NAME bdmvauthor-menu-media-loop-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_menu_media_loop.cmake)
  add_test(NAME bdmvauthor-cache-manager-controls-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_cache_manager_controls.cmake)
  add_test(NAME bdmvauthor-size-budget-cancel-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_size_budget_cancel.cmake)
  add_test(NAME bdmvauthor-title-management-gui-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_title_management_gui.cmake)
  add_test(NAME bdmvauthor-defaults-audio-bitrate-gui-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_defaults_audio_bitrate_gui.cmake)
  add_test(NAME bdmvauthor-passthrough-compliance-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_passthrough_compliance.cmake)
  add_test(NAME bdmvauthor-compliance-sampling-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_compliance_sampling.cmake)
  add_test(NAME bdmvauthor-compliance-cache-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_compliance_cache.cmake)
  add_test(NAME bdmvauthor-h264-title-startup-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_h264_title_startup.cmake)
  add_test(NAME bdmvauthor-bluray-transport-startup-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_bluray_transport_startup.cmake)
  add_test(NAME bdmvauthor-ts-continuity-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_ts_continuity.cmake)
  add_test(NAME bdmvauthor-lossless-audio-muxing-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_lossless_audio_muxing.cmake)
  add_test(NAME bdmvauthor-lpcm-wav-metadata-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_lpcm_wav_metadata.cmake)
  add_test(NAME bdmvauthor-bitrate-compliance-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_bitrate_compliance.cmake)
  add_test(NAME bdmvauthor-dynamic-av-bitrate-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_dynamic_av_bitrate.cmake)
  add_test(NAME bdmvauthor-debug-transport-limits-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_debug_transport_limits.cmake)
  add_test(NAME bdmvauthor-video-rate-limits-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_video_rate_limits.cmake)
  add_test(NAME bdmvauthor-stream-selection-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_stream_selection.cmake)
  add_test(NAME bdmvauthor-active-stream-state-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_active_stream_state.cmake)
  add_test(NAME bdmvauthor-menu-selection-loop-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_menu_selection_loop.cmake)
  add_test(NAME bdmvauthor-preloaded-menu-loop-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_preloaded_menu_loop.cmake)
  add_test(NAME bdmvauthor-bluray-menu-playlist-restart-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_bluray_menu_playlist_restart.cmake)
  add_test(NAME bdmvauthor-igs-subclip-transport-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_igs_subclip_transport.cmake)
  add_test(NAME bdmvauthor-menu-video-background-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_menu_video_background.cmake)
  add_test(NAME bdmvauthor-output-targets-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_output_targets.cmake)
  add_test(NAME bdmvauthor-target-codec-constraints-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_target_codec_constraints.cmake)
  add_test(NAME bdmvauthor-avchd-export-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_avchd_export.cmake)
  add_test(NAME bdmvauthor-frame-rates-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_frame_rates.cmake)
  add_test(NAME bdmvauthor-uhd-experimental-lossless-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_uhd_experimental_lossless.cmake)
  add_test(NAME bdmvauthor-fake-interlaced-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_fake_interlaced.cmake)
  add_test(NAME bdmvauthor-video-modes-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_video_modes.cmake)
  add_test(NAME bdmvauthor-menu-mode-state-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_menu_mode_state.cmake)
  add_test(NAME bdmvauthor-default-font-resolution-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_default_font_resolution.cmake)
  add_test(NAME bdmvauthor-bluray-menu-return-mode-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_bluray_menu_return_mode.cmake)
  add_test(NAME bdmvauthor-menu-resolution-defaults-gui-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_menu_resolution_defaults_gui.cmake)
  add_test(NAME bdmvauthor-gui-menus-unsaved-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_gui_menus_unsaved.cmake)
  add_test(NAME bdmvauthor-uhd-avc-repeat-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_uhd_avc_repeat.cmake)
  add_test(NAME bdmvauthor-repeat-groups-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_repeat_groups.cmake)
  add_test(NAME bdmvauthor-advanced-codec-options-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_advanced_codec_options.cmake)
  add_test(NAME bdmvauthor-hdma-integration-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_hdma_integration.cmake)
  add_test(NAME bdmvauthor-eac371-integration-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_eac371_integration.cmake)
  add_test(NAME bdmvauthor-portable-linux-libhdma-version-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_portable_linux_libhdma_version.cmake)
  add_test(NAME bdmvauthor-vc1-integration-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_vc1_integration.cmake)
  add_test(NAME bdmvauthor-menuless-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_menuless.cmake)
  add_test(NAME bdmvauthor-title-navigation-uop-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_title_navigation_uop.cmake)
  add_test(NAME bdmvauthor-stream-audio-settings-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_stream_audio_settings.cmake)
  add_test(NAME bdmvauthor-stream-removal-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_stream_removal.cmake)
  add_test(NAME bdmvauthor-stream-index-and-remap-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_stream_index_and_remap.cmake)
  add_test(NAME bdmvauthor-external-subtitle-downmix-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_external_subtitle_downmix.cmake)
  add_test(NAME bdmvauthor-subtitle-properties-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_subtitle_properties.cmake)
  add_test(NAME bdmvauthor-external-tools-gui-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_external_tools_gui.cmake)
  add_test(NAME bdmvauthor-remote-encoding-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_remote_encoding.cmake)
  add_test(NAME bdmvauthor-remote-hardware-configuration-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_remote_hardware_configuration.cmake)
  add_test(NAME bdmvauthor-encoding-scheduler-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_encoding_scheduler.cmake)
  add_test(NAME bdmvauthor-worker-affinity-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_worker_affinity.cmake)
  add_test(NAME bdmvauthor-worker-capability-cache-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_worker_capability_cache.cmake)
  add_test(NAME bdmvauthor-pre-remote-cache-compat-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_pre_remote_cache_compat.cmake)
  add_test(NAME bdmvauthor-remote-error-diagnostics-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_remote_error_diagnostics.cmake)
  add_test(NAME bdmvauthor-remote-hardware-preflight-ffvhuff-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_remote_hardware_preflight_ffvhuff.cmake)
  add_test(NAME bdmvauthor-output-image-selection-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_output_image_selection.cmake)
  add_test(NAME bdmvauthor-portable-linux-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_portable_linux.cmake)
  add_test(NAME bdmvauthor-macos-port-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_macos_port.cmake)
  add_test(NAME bdmvauthor-bundled-ffmpeg-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_bundled_ffmpeg.cmake)
  add_test(NAME bdmvauthor-bundled-ffmpeg-platform-scope COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_bundled_ffmpeg_platform_scope.cmake)
  if(APPLE)
    # This executes the private FFmpeg configure helper and validates Darwin's
    # required encoder set. It is a macOS packaging test, not a Linux runtime
    # requirement.
    add_test(NAME bdmvauthor-bundled-ffmpeg-encoder-list COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_bundled_ffmpeg_encoder_list.cmake)
  endif()
  add_test(NAME bdmvauthor-windows-ucrt64-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_windows_ucrt64.cmake)
  add_test(NAME bdmvauthor-windows-installer-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_windows_installer.cmake)
  add_test(NAME bdmvauthor-windows-sibling-tools-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_windows_sibling_tools.cmake)
  add_test(NAME bdmvauthor-windows-shell-launch-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_windows_shell_launch.cmake)
  add_test(NAME bdmvauthor-bundled-dvdauthor-windows-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_bundled_dvdauthor_windows.cmake)
  add_test(NAME bdmvauthor-bundled-dvdauthor-warnings-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_bundled_dvdauthor_warnings.cmake)
  add_test(NAME bdmvauthor-bundled-mkisofs-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_bundled_mkisofs.cmake)
  add_test(NAME bdmvauthor-bundled-mplex-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_bundled_mplex.cmake)
  if(BDMVAUTHOR_BUILD_BUNDLED_MPLEX)
    add_test(NAME bdmvauthor-bundled-mplex-runtime COMMAND ${CMAKE_COMMAND}
      -DMPLEX=$<TARGET_FILE:mplex>
      -P ${CMAKE_SOURCE_DIR}/tests/check_bundled_mplex_runtime.cmake)
  endif()
  if(BDMVAUTHOR_BUILD_BUNDLED_MKISOFS)
    add_executable(bdmvauthor-mkisofs-fixture-writer tests/write_minimal_dvd_fixture.c)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
      target_compile_options(bdmvauthor-mkisofs-fixture-writer PRIVATE -Wall -Wextra)
      if(BDMVAUTHOR_WARNINGS_AS_ERRORS)
        target_compile_options(bdmvauthor-mkisofs-fixture-writer PRIVATE -Werror)
      endif()
    endif()
    add_test(NAME bdmvauthor-bundled-mkisofs-runtime COMMAND ${CMAKE_COMMAND}
      -DMKISOFS=$<TARGET_FILE:bdmvauthor_mkisofs>
      -DFIXTURE_WRITER=$<TARGET_FILE:bdmvauthor-mkisofs-fixture-writer>
      -DWORK_DIR=${CMAKE_BINARY_DIR}
      -P ${CMAKE_SOURCE_DIR}/tests/check_bundled_mkisofs_runtime.cmake)
  endif()
  add_test(NAME bdmvauthor-app-icon-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_app_icon.cmake)
  add_test(NAME bdmvauthor-video-modes-cli COMMAND ${CMAKE_COMMAND} -DCLI=$<TARGET_FILE:bdmvauthor-cli> -DWORK_DIR=${CMAKE_BINARY_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_video_modes_cli.cmake)
  add_test(NAME bdmvauthor-menu-media-loop-cli COMMAND ${CMAKE_COMMAND} -DCLI=$<TARGET_FILE:bdmvauthor-cli> -DWORK_DIR=${CMAKE_BINARY_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_menu_media_loop_cli.cmake)
  add_test(NAME bdmvauthor-bitmap-subtitle-conversion-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_bitmap_subtitle_conversion.cmake)
  add_test(NAME bdmvauthor-dvd-palette-cleanup-audio-labels-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -DWORK_DIR=${CMAKE_BINARY_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_dvd_palette_cleanup_audio_labels.cmake)
  add_test(NAME bdmvauthor-optional-native-subtitle-tool-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_native_subtitle_component.cmake)
  add_test(NAME bdmvauthor-dvd-backend-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_dvd_backend.cmake)
  add_test(NAME bdmvauthor-dvd-active-stream-unsupported-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_dvd_active_stream_unsupported.cmake)
  add_test(NAME bdmvauthor-chapters-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_chapters.cmake)
  add_test(NAME bdmvauthor-chapters-cli COMMAND ${CMAKE_COMMAND} -DCLI=$<TARGET_FILE:bdmvauthor-cli> -P ${CMAKE_SOURCE_DIR}/tests/check_chapters_cli.cmake)
  add_test(NAME bdmvauthor-repeat-cli COMMAND ${CMAKE_COMMAND} -DCLI=$<TARGET_FILE:bdmvauthor-cli> -DWORK_DIR=${CMAKE_BINARY_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_repeat_cli.cmake)
  add_test(NAME bdmvauthor-advanced-codec-cli COMMAND ${CMAKE_COMMAND} -DCLI=$<TARGET_FILE:bdmvauthor-cli> -DWORK_DIR=${CMAKE_BINARY_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_advanced_codec_cli.cmake)
  add_test(NAME bdmvauthor-menuless-cli COMMAND ${CMAKE_COMMAND} -DCLI=$<TARGET_FILE:bdmvauthor-cli> -DWORK_DIR=${CMAKE_BINARY_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_menuless_cli.cmake)
  add_test(NAME bdmvauthor-target-codec-cli COMMAND ${CMAKE_COMMAND} -DCLI=$<TARGET_FILE:bdmvauthor-cli> -P ${CMAKE_SOURCE_DIR}/tests/check_target_codec_cli.cmake)
  add_test(NAME bdmvauthor-frame-rate-cli COMMAND ${CMAKE_COMMAND} -DCLI=$<TARGET_FILE:bdmvauthor-cli> -DWORK_DIR=${CMAKE_BINARY_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_frame_rate_cli.cmake)
  add_test(NAME bdmvauthor-external-subtitle-downmix-cli COMMAND ${CMAKE_COMMAND} -DCLI=$<TARGET_FILE:bdmvauthor-cli> -DWORK_DIR=${CMAKE_BINARY_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_external_subtitle_downmix_cli.cmake)
  add_test(NAME bdmvauthor-subtitle-properties-cli COMMAND ${CMAKE_COMMAND} -DCLI=$<TARGET_FILE:bdmvauthor-cli> -DWORK_DIR=${CMAKE_BINARY_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_subtitle_properties_cli.cmake)
  if(BDMVAUTHOR_HAVE_HDMAENC)
    add_test(NAME bdmvauthor-hdma-cli COMMAND ${CMAKE_COMMAND} -DCLI=$<TARGET_FILE:bdmvauthor-cli> -P ${CMAKE_SOURCE_DIR}/tests/check_hdma_cli.cmake)
  endif()
  if(BDMVAUTHOR_HAVE_VC1ENC)
    add_test(NAME bdmvauthor-vc1-cli COMMAND ${CMAKE_COMMAND} -DCLI=$<TARGET_FILE:bdmvauthor-cli> -P ${CMAKE_SOURCE_DIR}/tests/check_vc1_cli.cmake)
  endif()
  if(UNIX)
    add_executable(bdmvauthor-libbluray-runtime tests/test_libbluray_runtime.cpp)
    target_link_libraries(bdmvauthor-libbluray-runtime PRIVATE bdmvauthor_core ${CMAKE_DL_LIBS})
    add_test(NAME bdmvauthor-libbluray-runtime COMMAND bdmvauthor-libbluray-runtime)
  endif()
endif()

install(TARGETS bdmvauthor-cli RUNTIME DESTINATION bin)
if(TARGET bdsup2sub)
  install(TARGETS bdsup2sub RUNTIME DESTINATION bin)
endif()
if(BDMVAUTHOR_HAVE_BUNDLED_FFMPEG)
  install(PROGRAMS
    "${BDMVAUTHOR_FFMPEG_INSTALL_DIR}/bin/ffmpeg"
    "${BDMVAUTHOR_FFMPEG_INSTALL_DIR}/bin/ffprobe"
    DESTINATION bin)
  install(FILES
    third_party/ffmpeg/COPYING.GPLv2
    third_party/ffmpeg/COPYING.LGPLv2.1
    DESTINATION share/doc/bdmvauthor/third_party/ffmpeg
    OPTIONAL)
endif()
if(BDMVAUTHOR_HAVE_HDMAENC)
  install(TARGETS hdmaenc RUNTIME DESTINATION bin)
  install(FILES third_party/libhdma/COPYING.LGPL-2.1 third_party/libhdma/LICENSE-ORIGINS.md DESTINATION share/doc/bdmvauthor/third_party/libhdma)
endif()
if(BDMVAUTHOR_HAVE_EAC371ENC)
  install(TARGETS eac371enc RUNTIME DESTINATION bin)
  install(FILES
    third_party/eac371enc/COPYING.LGPLv2.1
    third_party/eac371enc/FFMPEG-LICENSE.md
    third_party/eac371enc/NOTICE.md
    DESTINATION share/doc/bdmvauthor/third_party/eac371enc OPTIONAL)
endif()
if(BDMVAUTHOR_HAVE_VC1ENC)
  # Install the frontend only. It is linked statically to libvc1 and remains a
  # process-level helper rather than becoming a BDMV Author library dependency.
  install(TARGETS vc1enc RUNTIME DESTINATION bin)
  install(FILES third_party/libvc1/LICENSE DESTINATION share/doc/bdmvauthor/third_party/libvc1)
endif()
if(TARGET bdmvauthor)
  install(TARGETS bdmvauthor RUNTIME DESTINATION bin)
endif()
install(FILES data/bdmvauthor.desktop DESTINATION share/applications)
install(FILES VERSION DESTINATION share/doc/bdmvauthor)
install(FILES resources/bdmvauthor-icon.svg DESTINATION share/icons/hicolor/scalable/apps RENAME bdmvauthor.svg)
install(FILES
  third_party/tsmuxer/LICENSE
  third_party/tsmuxer/BDMVAUTHOR-VENDORING.md
  DESTINATION share/doc/bdmvauthor/third_party/tsmuxer
)
install(FILES
  third_party/dvdauthor/COPYING
  third_party/dvdauthor/BDMVAUTHOR_WINDOWS_PORT.md
  DESTINATION share/doc/bdmvauthor/third_party/dvdauthor
)
