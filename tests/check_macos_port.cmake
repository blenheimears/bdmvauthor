cmake_minimum_required(VERSION 3.20)
if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "SOURCE_DIR is required")
endif()

file(READ "${SOURCE_DIR}/flake.nix" FLAKE)
file(READ "${SOURCE_DIR}/CMakeLists.txt" ROOT_CMAKE)
file(READ "${SOURCE_DIR}/third_party/dvdauthor/CMakeLists.txt" DVDA_CMAKE)
file(READ "${SOURCE_DIR}/third_party/mplex/CMakeLists.txt" MPLEX_CMAKE)
file(READ "${SOURCE_DIR}/third_party/mkisofs/CMakeLists.txt" MKISOFS_CMAKE)
file(READ "${SOURCE_DIR}/third_party/mkisofs/cmake/xconfig.h.in" MKISOFS_XCONFIG)
file(READ "${SOURCE_DIR}/tests/test_libbluray_runtime.cpp" BLURAY_TEST)

foreach(needle
    "x86_64-darwin"
    "aarch64-darwin"
    "DYLD_LIBRARY_PATH"
    "LD_LIBRARY_PATH"
    "pkgs.lib.optionalAttrs pkgs.stdenv.isLinux"
    "-DBDMVAUTHOR_BUILD_BUNDLED_DVDAUTHOR=ON"
    "-DBDMVAUTHOR_BUILD_BUNDLED_MPLEX=ON")
  string(FIND "${FLAKE}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "macOS flake contract missing: ${needle}")
  endif()
endforeach()

foreach(needle
    "if(WIN32 OR APPLE)"
    "bdmvauthor-macos-port-source")
  string(FIND "${ROOT_CMAKE}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "macOS CMake contract missing: ${needle}")
  endif()
endforeach()


foreach(needle
    "include(TestBigEndian)"
    "test_big_endian(WORDS_BIGENDIAN)"
    "set(HAVE_C_BIGENDIAN 1)")
  string(FIND "${MKISOFS_CMAKE}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "bundled mkisofs endian probe missing: ${needle}")
  endif()
endforeach()
foreach(needle "#cmakedefine HAVE_C_BIGENDIAN 1" "#cmakedefine WORDS_BIGENDIAN 1")
  string(FIND "${MKISOFS_XCONFIG}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "bundled mkisofs endian xconfig contract missing: ${needle}")
  endif()
endforeach()

string(FIND "${DVDA_CMAKE}" "install(TARGETS dvdauthor spumux RUNTIME DESTINATION bin)" dvda_install)
if(dvda_install EQUAL -1)
  message(FATAL_ERROR "bundled dvdauthor/spumux are not installed for macOS result/bin")
endif()
string(FIND "${MPLEX_CMAKE}" "install(TARGETS mplex RUNTIME DESTINATION bin)" mplex_install)
if(mplex_install EQUAL -1)
  message(FATAL_ERROR "bundled mplex is not installed for macOS result/bin")
endif()
foreach(needle "libbluray.2.dylib" "libbluray.dylib")
  string(FIND "${BLURAY_TEST}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "macOS libbluray runtime probe missing: ${needle}")
  endif()
endforeach()

foreach(forbidden
    "lib.getDev qt.qttools"
    "qtToolsBin"
    "qtToolsDev"
    "qtToolsOut"
    "macdeployqt"
    "compgen -G"
    "[[ -e \"$1\" ]]"
    "set --"
    "bundle_args=(-cd -of -b -ns -f)"
    "cp -RL \"$qtframework\" \"$frameworks/$framework_name\""
    "codesign --force --deep --sign -")
  string(FIND "${FLAKE}" "${forbidden}" pos)
  if(NOT pos EQUAL -1)
    message(FATAL_ERROR "macOS packaging regression reintroduced: ${forbidden}")
  endif()
endforeach()

file(READ "${SOURCE_DIR}/src/gui.cpp" GUI_SOURCE)
file(READ "${SOURCE_DIR}/src/author.cpp" AUTHOR_SOURCE)
file(READ "${SOURCE_DIR}/src/font_renderer.cpp" FONT_RENDERER_SOURCE)

foreach(needle
    "mkMacApp = system:"
    "mkMacDmg = system:"
    "pkgs.macdylibbundler"
    "pkgs.darwin.cctools"
    "command -v otool >/dev/null"
    "command -v install_name_tool >/dev/null"
    "qtPluginRoot ="
    "qtPluginPrefix"
    "for category in platforms styles imageformats iconengines printsupport"
    "cp -RL"
    "plugins/$category"
    "libqcocoa.dylib"
    "qtBaseLib ="
    "-maxdepth 1 -type d -name 'Qt*.framework' -print0"
    "resolved_framework=\"$(realpath \"$qtframework\")\""
    "cp -a \"$resolved_framework/.\" \"$frameworks/$framework_name/\""
    "scripts/macos_normalize_framework_symlinks.sh"
    "rewrite_qt_framework_refs()"
    "@executable_path/../Frameworks/$framework_name/$suffix"
    "@rpath/Qt*.framework/*"
    "bundle_args=(-cd -of -b -ns)"
    "find \"$macos\" \"$plugins\" \"$frameworks\" -type f -print0"
    "app=\"$out/BDMVAuthor.app\""
    "final_app=\"$out/BDMV Author.app\""
    "mv \"$app\" \"$final_app\""
    "BDMV Author.app"
    "Contents/MacOS"
    "Contents/Frameworks"
    "Contents/Resources"
    "fontconfig/fonts.conf"
    "etc/fonts/conf.d\" -maxdepth 1 -type f -name '*.conf' -print0"
    "/System/Library/Fonts"
    "~/Library/Fonts"
    "dylibbundler"
    "hdiutil create"
    "codesign --force --sign - \"$candidate\""
    "codesign --force --sign - \"$framework_bundle\""
    "codesign --force --sign - \"$app\""
    "codesign --verify --deep --strict \"$app\""
    "app = mkMacApp system;"
    "dmg = mkMacDmg system;")
  string(FIND "${FLAKE}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "macOS app/DMG packaging contract missing: ${needle}")
  endif()
endforeach()

string(FIND "${GUI_SOURCE}" "defined(_WIN32) || defined(__APPLE__)" gui_sibling)
if(gui_sibling EQUAL -1)
  message(FATAL_ERROR "macOS GUI does not prefer private sibling FFmpeg/x264/x265 tools")
endif()
foreach(needle
    "tools_.ffmpeg=resolve_sibling_default_tool"
    "tools_.ffprobe=resolve_sibling_default_tool"
    "tools_.x264=resolve_sibling_default_tool"
    "tools_.x265=resolve_sibling_default_tool"
    "tools_.dvdauthor=resolve_sibling_default_tool"
    "tools_.spumux=resolve_sibling_default_tool"
    "tools_.mplex=resolve_sibling_default_tool")
  string(FIND "${AUTHOR_SOURCE}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "macOS authoring sibling-tool resolution missing: ${needle}")
  endif()
endforeach()

foreach(needle
    "#elif defined(__APPLE__)"
    "_NSGetExecutablePath"
    "Resources"
    "fontconfig"
    "fonts.conf"
    "FONTCONFIG_FILE"
    "FONTCONFIG_PATH")
  string(FIND "${FONT_RENDERER_SOURCE}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "macOS app-local Fontconfig runtime contract missing: ${needle}")
  endif()
endforeach()

string(FIND "${AUTHOR_SOURCE}" "#if defined(_WIN32) || defined(__APPLE__)\n    // spumux is a child process" apple_fontconfig_child)
if(apple_fontconfig_child EQUAL -1)
  message(FATAL_ERROR "macOS AuthorEngine does not export app-local Fontconfig environment to spumux")
endif()


# Exercise the framework-symlink normalizer with the layout that failed on
# Darwin: a normal top-level alias depends on Versions/Current, while Current
# itself is an absolute Nix-store link. The helper must rewrite Current first
# and leave the complete alias chain resolving inside Contents/Frameworks.
set(FRAMEWORK_FIXTURE "${CMAKE_CURRENT_BINARY_DIR}/macos-framework-symlink-fixture")
file(REMOVE_RECURSE "${FRAMEWORK_FIXTURE}")
file(MAKE_DIRECTORY "${FRAMEWORK_FIXTURE}/QtGui.framework/Versions/A")
file(WRITE "${FRAMEWORK_FIXTURE}/QtGui.framework/Versions/A/QtGui" "fixture")
file(CREATE_LINK "/nix/store/fake-qt/QtGui.framework/Versions/A"
     "${FRAMEWORK_FIXTURE}/QtGui.framework/Versions/Current" SYMBOLIC)
file(CREATE_LINK "Versions/Current/QtGui"
     "${FRAMEWORK_FIXTURE}/QtGui.framework/QtGui" SYMBOLIC)
execute_process(
  COMMAND bash "${SOURCE_DIR}/scripts/macos_normalize_framework_symlinks.sh" "${FRAMEWORK_FIXTURE}"
  RESULT_VARIABLE framework_normalize_rc
  OUTPUT_VARIABLE framework_normalize_out
  ERROR_VARIABLE framework_normalize_err)
if(NOT framework_normalize_rc EQUAL 0)
  message(FATAL_ERROR "framework symlink normalization fixture failed: ${framework_normalize_err}")
endif()
file(READ_SYMLINK "${FRAMEWORK_FIXTURE}/QtGui.framework/Versions/Current" current_target)
if(NOT current_target STREQUAL "A")
  message(FATAL_ERROR "framework Versions/Current was not normalized to a relative in-bundle target: ${current_target}")
endif()
file(REAL_PATH "${FRAMEWORK_FIXTURE}" FRAMEWORK_FIXTURE_REAL)
file(REAL_PATH "${FRAMEWORK_FIXTURE}/QtGui.framework/QtGui" resolved_qtgui)
if(NOT resolved_qtgui STREQUAL "${FRAMEWORK_FIXTURE_REAL}/QtGui.framework/Versions/A/QtGui")
  message(FATAL_ERROR "normalized Qt framework alias does not resolve inside fixture: ${resolved_qtgui}")
endif()
file(REMOVE_RECURSE "${FRAMEWORK_FIXTURE}")
