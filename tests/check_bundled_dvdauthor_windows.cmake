if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "SOURCE_DIR not supplied")
endif()

file(READ "${SOURCE_DIR}/CMakeLists.txt" TOP)
file(READ "${SOURCE_DIR}/windows/build-msys2-ucrt64.sh" WIN)
file(READ "${SOURCE_DIR}/third_party/dvdauthor/CMakeLists.txt" DVDCMAKE)
file(READ "${SOURCE_DIR}/third_party/dvdauthor/src/compat.h" COMPAT_H)
file(READ "${SOURCE_DIR}/third_party/dvdauthor/src/compat.c" COMPAT_C)
file(READ "${SOURCE_DIR}/third_party/dvdauthor/src/dvdauthor.c" DVDA)
file(READ "${SOURCE_DIR}/third_party/dvdauthor/src/subgen.c" SPU)
file(READ "${SOURCE_DIR}/src/author.cpp" AUTHOR)
file(READ "${SOURCE_DIR}/src/font_renderer.cpp" FONT)

foreach(needle
  "BDMVAUTHOR_BUILD_BUNDLED_DVDAUTHOR"
  "add_subdirectory(third_party/dvdauthor")
  string(FIND "${TOP}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "bundled dvdauthor top-level integration missing: ${needle}")
  endif()
endforeach()

foreach(needle
  "add_executable(dvdauthor"
  "add_executable(spumux"
  "LibXml2::LibXml2"
  "PNG::PNG"
  "Freetype::Freetype"
  "PkgConfig::DVDA_FONTCONFIG"
  "PkgConfig::DVDA_FRIBIDI"
  "Iconv::Iconv"
  "ws2_32"
  "_FILE_OFFSET_BITS=64"
  "RUNTIME_OUTPUT_DIRECTORY")
  string(FIND "${DVDCMAKE}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "bundled dvdauthor CMake port missing: ${needle}")
  endif()
endforeach()

foreach(needle
  "#define win32_setmode _setmode"
  "int portable_mkdir(const char *path)")
  string(FIND "${COMPAT_H}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "dvdauthor Win32 compatibility header missing: ${needle}")
  endif()
endforeach()
foreach(needle "return _mkdir(path);" "return mkdir(path, 0777);")
  string(FIND "${COMPAT_C}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "dvdauthor portable mkdir implementation missing: ${needle}")
  endif()
endforeach()
string(FIND "${DVDA}" "portable_mkdir(fbase)" mkdir_call)
if(mkdir_call EQUAL -1)
  message(FATAL_ERROR "dvdauthor output-directory creation still assumes POSIX mkdir(path, mode)")
endif()
foreach(needle "#include <winsock2.h>" "#include <netinet/in.h>")
  string(FIND "${SPU}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "spumux byte-order portability guard missing: ${needle}")
  endif()
endforeach()

foreach(needle
  "mingw-w64-ucrt-x86_64-libxml2"
  "mingw-w64-ucrt-x86_64-libpng"
  "mingw-w64-ucrt-x86_64-fribidi"
  "mingw-w64-ucrt-x86_64-libiconv"
  "flex"
  "bison"
  "-DBDMVAUTHOR_BUILD_BUNDLED_DVDAUTHOR=ON"
  "bundled dvdauthor.exe/spumux.exe were not built"
  "cp -f \"$build_dir/dvdauthor.exe\""
  "cp -f \"$build_dir/spumux.exe\"")
  string(FIND "${WIN}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "Windows package script does not build/stage bundled dvdauthor: ${needle}")
  endif()
endforeach()

foreach(needle
  "detail::configure_fontconfig_runtime_environment();"
  "tools_.spumux=resolve_sibling_default_tool")
  string(FIND "${AUTHOR}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "bundled spumux runtime integration missing: ${needle}")
  endif()
endforeach()
string(FIND "${FONT}" "void configure_fontconfig_runtime_environment()" font_env)
if(font_env EQUAL -1)
  message(FATAL_ERROR "private Fontconfig environment is not exposed for spumux children")
endif()

if(NOT EXISTS "${SOURCE_DIR}/third_party/dvdauthor/COPYING")
  message(FATAL_ERROR "vendored dvdauthor GPL license is missing")
endif()

string(FIND "${WIN}" "third_party-source" source_copy_pos)
if(NOT source_copy_pos EQUAL -1)
  message(FATAL_ERROR "Windows runtime package must not copy vendored dvdauthor source")
endif()

message(STATUS "bundled native-Windows dvdauthor/spumux source checks ok")
