if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "SOURCE_DIR not supplied")
endif()
file(READ "${SOURCE_DIR}/flake.nix" FLAKE)
file(READ "${SOURCE_DIR}/nix/build-portable-linux.sh" HELPER)
file(READ "${SOURCE_DIR}/third_party/dvdauthor/CMakeLists.txt" DVDA_CMAKE)
file(READ "${SOURCE_DIR}/third_party/dvdauthor/src/config-linux.h.cmake" DVDA_LINUX_CONFIG)
file(READ "${SOURCE_DIR}/src/gui.cpp" GUI)
foreach(needle
  "github:NixOS/nixpkgs/nixos-26.05"
  "github:NixOS/nixpkgs/nixos-22.11"
  "\"portable-linux\" = portableLinux"
  "portableCompileCC = buildPkgs.gcc16Stdenv.cc"
  "oldCxx = abiPkgs.stdenv.cc"
  "-nostdinc++"
  "-fabi-version=16"
  "expected Qt 6.4.x"
  "abiPkgs.qt6.qtbase abiPkgs.openssl abiPkgs.fontconfig abiPkgs.freetype abiPkgs.zlib abiPkgs.libxml2 abiPkgs.libpng abiPkgs.fribidi"
  "buildPkgs.flex buildPkgs.bison"
  "Build the vendored tsMuxer CLI"
  "-DTSMUXER_GUI=OFF"
  "-DTSMUXER_STATIC_BUILD=OFF"
  "-DTSMUXER_VERSION_OVERRIDE=git-c6b1186-bdmvauthor"
  "install -Dm755 tsmuxer-portable \"$out/bin/tsmuxer\""
  "cmake -S third_party/mkisofs -B mkisofs-portable-build"
  "install -Dm755 mkisofs-portable \"$out/bin/mkisofs\""
  "cmake -S third_party/dvdauthor -B dvdauthor-portable-build"
  "cmake --build dvdauthor-portable-build --target dvdauthor spumux"
  "install -Dm755 dvdauthor-portable \"$out/bin/dvdauthor\""
  "install -Dm755 spumux-portable \"$out/bin/spumux\""
  "cmake -S third_party/mplex -B mplex-portable-build"
  "install -Dm755 mplex-portable \"$out/bin/mplex\""
  "audit_elf \"$out/bin/tsmuxer\""
  "audit_elf \"$out/bin/mkisofs\""
  "audit_elf \"$out/bin/dvdauthor\""
  "audit_elf \"$out/bin/spumux\""
  "audit_elf \"$out/bin/mplex\""
  "tsmuxer-needed-libraries.txt"
  "dvdauthor-needed-libraries.txt"
  "spumux-needed-libraries.txt"
  "mplex-needed-libraries.txt"
  "bundled-tsmuxer=git-c6b1186-bdmvauthor"
  "bundled-mkisofs=cdrtools-3.02a09-minimal"
  "bundled-dvdauthor=0.7.2+-bdmvauthor"
  "bundled-spumux=0.7.2+-bdmvauthor"
  "bundled-mplex=mjpegtools-r3517-minimal"
  "system_pkg_cflags"
  "--cflags-only-I"
  "result=\"$result -isystem"
  "/lib64/ld-linux-x86-64.so.2"
  "/lib/ld-linux-aarch64.so.1"
  "NIX_DONT_SET_RPATH = \"1\""
  "NIX_NO_SELF_RPATH = \"1\""
  "portable ELF unexpectedly contains RPATH/RUNPATH"
  "'/nix/store/'"
  "GLIBC_2.36"
  "GLIBCXX_3.4.30"
  "CXXABI_1.3.13"
  "libavcodec.so libavformat.so libx264.so libx265.so"
  "external-tools=runtime-discovered"
  "resources/bdmvauthor-icon.svg"
  "data/bdmvauthor.desktop")
  string(FIND "${FLAKE}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing portable-linux flake marker: ${needle}")
  endif()
endforeach()
foreach(needle
  "nix build .#portable-linux"
  "github:NixOS/nixpkgs/nixos-26.05"
  "github:NixOS/nixpkgs/nixos-22.11"
  "< \"$root/VERSION\""
  "cp -L \"$out_path/bin/bdmvauthor\""
  "cp -L \"$out_path/bin/bdmvauthor-cli\""
  "test -x \"$out_path/bin/tsmuxer\""
  "test -x \"$out_path/bin/mkisofs\""
  "test -x \"$out_path/bin/dvdauthor\""
  "test -x \"$out_path/bin/spumux\""
  "test -x \"$out_path/bin/mplex\""
  "bundle_name=\"bdmvauthor-"
  "-linux-"
  "-portable\""
  "cp -L \"$out_path/bin/tsmuxer\" \"$bundle_dir/tsmuxer\""
  "cp -L \"$out_path/bin/mkisofs\" \"$bundle_dir/mkisofs\""
  "cp -L \"$out_path/bin/dvdauthor\" \"$bundle_dir/dvdauthor\""
  "cp -L \"$out_path/bin/spumux\" \"$bundle_dir/spumux\""
  "cp -L \"$out_path/bin/mplex\" \"$bundle_dir/mplex\""
  "mkisofs-needed-libraries.txt"
  "dvdauthor-needed-libraries.txt"
  "spumux-needed-libraries.txt"
  "mplex-needed-libraries.txt"
  "nixpkgs#binutils"
  "--inputs-from ."
  "Stripping generic Linux release binaries"
  [=["$strip_tool" --strip-all "$binary"]=])
  string(FIND "${HELPER}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing portable-linux helper marker: ${needle}")
  endif()
endforeach()


foreach(needle
  "configure_file(src/config-linux.h.cmake"
  "if(WIN32)"
  "target_link_libraries(dvdauthor PRIVATE ws2_32)"
  "target_link_libraries(spumux PRIVATE m)")
  string(FIND "${DVDA_CMAKE}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing portable dvdauthor CMake marker: ${needle}")
  endif()
endforeach()
foreach(needle
  "#define HAVE_STRNDUP 1"
  "#define HAVE_DECL_O_BINARY 0"
  "/* #undef HAVE_IO_H */")
  string(FIND "${DVDA_LINUX_CONFIG}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing portable dvdauthor Linux feature marker: ${needle}")
  endif()
endforeach()
foreach(needle
  "resolve_tool(config.dvdauthor,\"dvdauthor\",true)"
  "resolve_tool(config.spumux,\"spumux\",true)"
  "resolve_tool(config.mplex,\"mplex\",true)")
  string(FIND "${GUI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "portable Linux GUI does not prefer bundled sibling helper: ${needle}")
  endif()
endforeach()

string(FIND "${HELPER}" "third_party-source" source_copy_pos)
if(NOT source_copy_pos EQUAL -1)
  message(FATAL_ERROR "portable Linux runtime package must not copy third-party source trees")
endif()

string(FIND "${FLAKE}" "no FFmpeg/x264/x265/tsMuxer/DVD helper bundled" stale_unbundled)
if(NOT stale_unbundled EQUAL -1)
  message(FATAL_ERROR "portable-linux regression: stale unbundled-tsMuxer manifest remains")
endif()

foreach(stale_text
  "dvdauthor/spumux/mplex remain host-provided on Linux"
  "FFmpeg/x264/x265/dvdauthor/spumux/mplex remain host-provided on Linux")
  string(FIND "${FLAKE}" "${stale_text}" stale_pos)
  if(NOT stale_pos EQUAL -1)
    message(FATAL_ERROR "portable-linux regression: stale external DVD-helper manifest remains: ${stale_text}")
  endif()
endforeach()

# Current glibc redirects the std::stoi/stol/stoul implementation family to
# GLIBC_2.38 __isoc23_* symbols under g++'s default GNU feature set. Portable
# application code must use from_chars so the Debian-12 ABI floor is real.
set(PARSE_SOURCES
  "${SOURCE_DIR}/include/bdmvauthor/model.hpp"
  "${SOURCE_DIR}/src/author.cpp"
  "${SOURCE_DIR}/src/progress.cpp"
  "${SOURCE_DIR}/src/cli.cpp"
  "${SOURCE_DIR}/src/gui.cpp")
foreach(parse_source IN LISTS PARSE_SOURCES)
  file(READ "${parse_source}" PARSE_TEXT)
  foreach(forbidden "std::stoi(" "std::stol(" "std::stoul(" "std::stoll(" "std::stoull(")
    string(FIND "${PARSE_TEXT}" "${forbidden}" parse_pos)
    if(NOT parse_pos EQUAL -1)
      message(FATAL_ERROR "portable parsing regression: ${parse_source} contains ${forbidden}")
    endif()
  endforeach()
endforeach()
file(READ "${SOURCE_DIR}/include/bdmvauthor/model.hpp" MODEL)
string(FIND "${MODEL}" "std::from_chars" from_chars_pos)
if(from_chars_pos EQUAL -1)
  message(FATAL_ERROR "portable parsing regression: from_chars helper missing")
endif()

message(STATUS "portable-linux source checks ok")
