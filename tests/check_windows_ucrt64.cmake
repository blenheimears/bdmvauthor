if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "SOURCE_DIR not supplied")
endif()

file(READ "${SOURCE_DIR}/windows/build-msys2-ucrt64.sh" SCRIPT)
file(READ "${SOURCE_DIR}/CMakeLists.txt" CMAKE)
file(READ "${SOURCE_DIR}/src/gui.cpp" GUI)
file(READ "${SOURCE_DIR}/src/font_renderer.cpp" FONT_RENDERER)
file(READ "${SOURCE_DIR}/tests/test_core.cpp" CORE_TEST)

foreach(needle
    "MSYSTEM:-}"
    "UCRT64"
    "mingw-w64-ucrt-x86_64-gcc"
    "mingw-w64-ucrt-x86_64-binutils"
    "mingw-w64-ucrt-x86_64-cmake"
    "mingw-w64-ucrt-x86_64-ninja"
    "mingw-w64-ucrt-x86_64-qt6-base"
    "mingw-w64-ucrt-x86_64-openssl"
    "mingw-w64-ucrt-x86_64-zlib"
    "windeployqt6.exe"
    "objdump.exe"
    "pe_imports"
    "is_windows_system_dll"
    "find_ucrt_dll"
    "platforms/qwindows.dll"
    "fontconfig/fonts.conf"
    "cp -Lf"
    "bdmvauthor.exe"
    "bdmvauthor-cli.exe"
    "tsmuxer.exe"
    "mkisofs.exe"
    "-DBDMVAUTHOR_BUILD_BUNDLED_MKISOFS=ON"
    "-DBDMVAUTHOR_BUILD_BUNDLED_MPLEX=ON"
    "mingw-w64-ucrt-x86_64-ffmpeg"
    "mingw-w64-ucrt-x86_64-nsis"
    "makensis.exe"
    "mplex.exe"
    "ffmpeg.exe"
    "ffprobe.exe"
    "--ffmpeg-dir"
    "ffmpeg_dir_arg"
    "ffmpeg_source_dir"
    "custom ffmpeg.exe failed its self-test"
    "custom ffprobe.exe failed its self-test"
    "refusing to fall back to MSYS2 FFmpeg"
    "find_custom_ffmpeg_dll"
    "strip.exe"
    "Stripping staged executables and DLLs"
    [=[--strip-all "$pe"]=]
    "Verifying stripped runtime dependency closure")
  string(FIND "${SCRIPT}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "Windows UCRT64 packaging regression: missing script marker: ${needle}")
  endif()
endforeach()

foreach(needle
    "set_target_properties(bdmvauthor PROPERTIES WIN32_EXECUTABLE TRUE)"
    "bdmvauthor-windows-ucrt64-source")
  string(FIND "${CMAKE}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "Windows UCRT64 packaging regression: missing CMake marker: ${needle}")
  endif()
endforeach()

foreach(needle
    "QString sibling_name=command;"
    "sibling_name.endsWith(QStringLiteral(\".exe\"),Qt::CaseInsensitive)"
    "applicationDirPath()+QDir::separator()+sibling_name")
  string(FIND "${GUI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "Windows sibling-tool regression: missing GUI marker: ${needle}")
  endif()
endforeach()

foreach(needle
    "configure_bundled_fontconfig"
    "FONTCONFIG_FILE"
    "fontconfig\"/L\"fonts.conf")
  string(FIND "${FONT_RENDERER}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "Windows bundled Fontconfig regression: missing marker: ${needle}")
  endif()
endforeach()

# Windows does not allow deleting an open file. Scope both verification streams
# so their Win32 handles are destroyed before cleanup, and tolerate a short
# antivirus/indexer sharing window after the process closes them.
foreach(needle
    "remove_test_tree(audio_tmp)"
    "std::ifstream merged_file"
    "std::ifstream merged_448_file"
    "std::this_thread::sleep_for(std::chrono::milliseconds(50))")
  string(FIND "${CORE_TEST}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "Windows temporary-file cleanup regression: missing core-test marker: ${needle}")
  endif()
endforeach()

# --ffmpeg-dir is a strict override: the MSYS2 FFmpeg package is appended only
# when no override was supplied, and the selected directory is the sole source
# of ffmpeg.exe/ffprobe.exe staging.
foreach(needle
    [=[if [[ -z "$ffmpeg_dir_arg" ]]; then]=]
    [=[packages+=(mingw-w64-ucrt-x86_64-ffmpeg)]=]
    [=[cp -f "$ffmpeg_source_dir/ffmpeg.exe" "$dist_dir/ffmpeg.exe"]=]
    [=[cp -f "$ffmpeg_source_dir/ffprobe.exe" "$dist_dir/ffprobe.exe"]=]
    [=[PATH="$ffmpeg_source_dir:$PATH" "$ffmpeg_source_dir/ffmpeg.exe" -hide_banner -version]=]
    [=[PATH="$ffmpeg_source_dir:$PATH" "$ffmpeg_source_dir/ffprobe.exe" -hide_banner -version]=])
  string(FIND "${SCRIPT}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "Windows custom FFmpeg regression: missing strict override marker: ${needle}")
  endif()
endforeach()

foreach(forbidden
    [=[cp -f "$ucrt_prefix/bin/ffmpeg.exe" "$dist_dir/ffmpeg.exe"]=]
    [=[cp -f "$ucrt_prefix/bin/ffprobe.exe" "$dist_dir/ffprobe.exe"]=])
  string(FIND "${SCRIPT}" "${forbidden}" pos)
  if(NOT pos EQUAL -1)
    message(FATAL_ERROR "Windows custom FFmpeg regression: hard-coded MSYS2 staging remains: ${forbidden}")
  endif()
endforeach()

# Invalid custom selections must be rejected before package installation, so a
# bad override cannot cause package changes and then fall through to defaults.
string(FIND "${SCRIPT}" [=[if [[ ! -d "$ffmpeg_dir_posix" ]]; then]=] validate_dir_pos)
string(FIND "${SCRIPT}" [=[pacman -S --needed --noconfirm]=] pacman_pos)
if(validate_dir_pos EQUAL -1 OR pacman_pos EQUAL -1 OR validate_dir_pos GREATER pacman_pos)
  message(FATAL_ERROR "Windows custom FFmpeg regression: --ffmpeg-dir validation must precede pacman")
endif()

message(STATUS "MSYS2 UCRT64 build/deployment source checks ok")

string(FIND "${SCRIPT}" "third_party-source" source_copy_pos)
if(NOT source_copy_pos EQUAL -1)
  message(FATAL_ERROR "Windows runtime package must not contain copied third-party source trees")
endif()
