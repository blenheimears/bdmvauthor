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
if(WIN32)
  set(BDMVAUTHOR_BUNDLED_MPLEX_DEFAULT ON)
else()
  set(BDMVAUTHOR_BUNDLED_MPLEX_DEFAULT OFF)
endif()
option(BDMVAUTHOR_BUILD_BUNDLED_MPLEX "Build the vendored mplex helper" ${BDMVAUTHOR_BUNDLED_MPLEX_DEFAULT})
if(WIN32)
  option(BDMVAUTHOR_BUILD_BUNDLED_DVDAUTHOR "Build the vendored dvdauthor/spumux helpers" ON)
else()
  option(BDMVAUTHOR_BUILD_BUNDLED_DVDAUTHOR "Build the vendored dvdauthor/spumux helpers" OFF)
endif()

if(BDMVAUTHOR_BUILD_BUNDLED_TSMUXER)
  # Only the tsMuxer CLI is needed.  Keep the upstream GUI source vendored but
  # do not pull a second GUI or its Qt requirements into BDMV Author.
  set(TSMUXER_GUI OFF CACHE BOOL "Build the vendored tsMuxer GUI" FORCE)
  set(TSMUXER_STATIC_BUILD OFF CACHE BOOL "Build vendored tsMuxer statically" FORCE)
  set(TSMUXER_VERSION_OVERRIDE "git-c6b1186-bdmvauthor" CACHE STRING "Vendored tsMuxer version" FORCE)
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

add_library(bdmvauthor_core
    src/author.cpp
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
  add_executable(bdmvauthor-tests tests/test_core.cpp)
  target_link_libraries(bdmvauthor-tests PRIVATE bdmvauthor_core)
  add_test(NAME bdmvauthor-core COMMAND bdmvauthor-tests)

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
add_test(NAME bdmvauthor-dvd-ffmpeg-rate-control-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_dvd_ffmpeg_rate_control.cmake)
add_test(NAME bdmvauthor-mpeg2-field-order-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_mpeg2_field_order.cmake)
add_test(NAME bdmvauthor-dvd-ffmpeg-runtime COMMAND ${CMAKE_COMMAND} -P ${CMAKE_SOURCE_DIR}/tests/check_dvd_ffmpeg_runtime.cmake)
  add_test(NAME bdmvauthor-button-style-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_button_styles.cmake)
  add_test(NAME bdmvauthor-button-image-layout-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_button_images_layout.cmake)
  add_test(NAME bdmvauthor-progress-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_progress_source.cmake)
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
  add_test(NAME bdmvauthor-menuless-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_menuless.cmake)
  add_test(NAME bdmvauthor-title-navigation-uop-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_title_navigation_uop.cmake)
  add_test(NAME bdmvauthor-stream-audio-settings-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_stream_audio_settings.cmake)
  add_test(NAME bdmvauthor-external-subtitle-downmix-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_external_subtitle_downmix.cmake)
  add_test(NAME bdmvauthor-subtitle-properties-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_subtitle_properties.cmake)
  add_test(NAME bdmvauthor-external-tools-gui-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_external_tools_gui.cmake)
  add_test(NAME bdmvauthor-output-image-selection-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_output_image_selection.cmake)
  add_test(NAME bdmvauthor-portable-linux-source COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_portable_linux.cmake)
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
  if(UNIX)
    add_executable(bdmvauthor-libbluray-runtime tests/test_libbluray_runtime.cpp)
    target_link_libraries(bdmvauthor-libbluray-runtime PRIVATE bdmvauthor_core ${CMAKE_DL_LIBS})
    add_test(NAME bdmvauthor-libbluray-runtime COMMAND bdmvauthor-libbluray-runtime)
  endif()
endif()

install(TARGETS bdmvauthor-cli RUNTIME DESTINATION bin)
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
