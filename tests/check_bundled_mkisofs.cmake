if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "SOURCE_DIR is required")
endif()
file(READ "${SOURCE_DIR}/CMakeLists.txt" TOP)
file(READ "${SOURCE_DIR}/third_party/mkisofs/CMakeLists.txt" MKCMAKE)
file(READ "${SOURCE_DIR}/third_party/mkisofs/cmake/xconfig.h.in" XCONFIG)
file(READ "${SOURCE_DIR}/third_party/mkisofs/support/getargs.c" GETARGS)
file(READ "${SOURCE_DIR}/third_party/mkisofs/include/schily/schily.h" SCHILY_H)
file(READ "${SOURCE_DIR}/third_party/mkisofs/include/schily/nlsdefs.h" NLSDEFS)
file(READ "${SOURCE_DIR}/third_party/mkisofs/include/schily/ccomdefs.h" CCOMDEFS)
file(READ "${SOURCE_DIR}/third_party/mkisofs/src/multi.c" MULTI)
file(READ "${SOURCE_DIR}/third_party/mkisofs/src/eltorito.c" ELTORITO)
file(READ "${SOURCE_DIR}/third_party/mkisofs/src/tree.c" TREE)
file(READ "${SOURCE_DIR}/third_party/mkisofs/src/rock.c" ROCK)
file(READ "${SOURCE_DIR}/third_party/mkisofs/support/compat.c" COMPAT)
file(READ "${SOURCE_DIR}/windows/build-msys2-ucrt64.sh" WIN)
file(READ "${SOURCE_DIR}/src/author.cpp" AUTHOR)
file(READ "${SOURCE_DIR}/src/gui.cpp" GUI)
file(READ "${SOURCE_DIR}/flake.nix" FLAKE)
file(READ "${SOURCE_DIR}/tests/check_bundled_mkisofs_runtime.cmake" RUNTIME)

foreach(needle
  "BDMVAUTHOR_BUILD_BUNDLED_MKISOFS"
  "add_subdirectory(third_party/mkisofs"
  "bdmvauthor-bundled-mkisofs-runtime"
  "bdmvauthor-mkisofs-fixture-writer")
  string(FIND "${TOP}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "top-level bundled mkisofs integration missing: ${needle}")
  endif()
endforeach()
foreach(needle
  "add_executable(bdmvauthor_mkisofs"
  "CMAKE_REQUIRED_DEFINITIONS -D_FILE_OFFSET_BITS=64"
  "BDMVA_MKISOFS_OFF_T_64"
  "_FILE_OFFSET_BITS=64"
  "OUTPUT_NAME mkisofs"
  "src/dvd_file.c"
  "src/dvd_reader.c"
  "src/ifo_read.c"
  "src/udf.c"
  "support/getargs.c"
  "support/fnmatch.c"
  "support/sha3.c"
  "support/strlcpy.c"
  "support/strlcat.c"
  "support/compat.c"
  "support/siconv_compat.c"
  "DVD_AUD_VID"
  "UDF"
  "Iconv::Iconv"
  "BDMVA_MKISOFS_MAP_FSEEKO"
  "HAVE_UID_T"
  "HAVE_GID_T"
  "HAVE_NLINK_T"
  "HAVE_ERRNO_DEF"
  "HAVE_ENVIRON_DEF"
  "strlcpy strlcat"
  "_CRT_SECURE_NO_WARNINGS")
  string(FIND "${MKCMAKE}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "vendored mkisofs CMake port missing: ${needle}")
  endif()
endforeach()

string(FIND "${GETARGS}" "raisecond(\"getarg_bad_format\", (void *)fmt)" pos)
if(pos EQUAL -1)
  message(FATAL_ERROR "mkisofs getargs pointer-safe raisecond fix missing")
endif()
string(FIND "${SCHILY_H}" "raisecond __PR((const char *, void *))" pos)
if(pos EQUAL -1)
  message(FATAL_ERROR "mkisofs raisecond declaration is not LLP64-safe")
endif()
string(FIND "${NLSDEFS}" "defined(HAVE_LIBINTL_H) && !defined(NO_NLS)" pos)
if(pos EQUAL -1)
  message(FATAL_ERROR "mkisofs NO_NLS libintl guard missing")
endif()
foreach(needle
  "defined(__MINGW32__) && !defined(__clang__)"
  "__format__(__gnu_printf__, fmtarg, firstvararg)"
  "__format__(__gnu_scanf__, fmtarg, firstvararg)"
  "__format__(__printf__, fmtarg, firstvararg)")
  string(FIND "${CCOMDEFS}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "mkisofs MinGW/UCRT format-attribute compatibility marker missing: ${needle}")
  endif()
endforeach()
foreach(needle
  "Bad directory length %zu (> %zu available)"
  "Path name '%s%s%s' exceeds max length %zd")
  string(FIND "${MULTI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "mkisofs C99 size-format regression marker missing: ${needle}")
  endif()
endforeach()

foreach(needle
  "#define fseeko _fseeki64"
  "#define ftello _ftelli64"
  "#define uid_t int"
  "#define gid_t int"
  "#define nlink_t unsigned long"
  "#cmakedefine HAVE_ERRNO_DEF 1"
  "#cmakedefine HAVE_ENVIRON_DEF 1"
  "#cmakedefine HAVE_STRLCPY 1"
  "#cmakedefine HAVE_STRLCAT 1")
  string(FIND "${XCONFIG}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "UCRT64/generated-config compatibility marker missing: ${needle}")
  endif()
endforeach()
foreach(needle
  "if (lseek(bootimage, (off_t)8, SEEK_SET) == (off_t)-1)"
  "ssize_t n = write(bootimage, bp, left)"
  "if (n < 0 && geterrno() == EINTR)"
  "if (close(bootimage) < 0)")
  string(FIND "${ELTORITO}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "mkisofs El Torito checked-I/O marker missing: ${needle}")
  endif()
endforeach()
foreach(needle
  "#ifdef S_IFLNK\nLOCAL\tUchar\tsymlink_buff"
  "int nchar;"
  "((Llong)190 * (Llong)0x3FFFF800UL)")
  string(FIND "${TREE}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "mkisofs UCRT64 tree.c warning/LLP64 fix missing: ${needle}")
  endif()
endforeach()
foreach(needle
  "#ifdef S_IFLNK\nLOCAL\tUchar\tsymlink_buff"
  "#endif")
  string(FIND "${ROCK}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "mkisofs UCRT64 rock.c symlink warning fix missing: ${needle}")
  endif()
endforeach()
file(READ "${SOURCE_DIR}/third_party/mkisofs/support/strlcpy.c" STRLCPY)
file(READ "${SOURCE_DIR}/third_party/mkisofs/support/strlcat.c" STRLCAT)
foreach(pair
  "STRLCPY|#ifndef\tHAVE_STRLCPY"
  "STRLCPY|strlcpy(s1, s2, len)"
  "STRLCAT|#ifndef\tHAVE_STRLCAT"
  "STRLCAT|strlcat(s1, s2, len)")
  string(REPLACE "|" ";" fields "${pair}")
  list(GET fields 0 var)
  list(GET fields 1 needle)
  string(FIND "${${var}}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "mkisofs strl fallback marker missing: ${needle}")
  endif()
endforeach()

foreach(needle
  "if (err >= 0)"
  "compat_progname"
  "error(const char *fmt, ...)"
  "js_error(const char *fmt, ...)")
  string(FIND "${COMPAT}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "mkisofs libschily-compatible diagnostic fix missing: ${needle}")
  endif()
endforeach()

foreach(needle
  "-DBDMVAUTHOR_BUILD_BUNDLED_MKISOFS=ON"
  "bundled cdrtools mkisofs.exe was not built by CMake"
  "cp -f \"$build_dir/mkisofs.exe\" \"$dist_dir/mkisofs.exe\""
  "Source for vendored helper tools and their licensing information")
  string(FIND "${WIN}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "Windows CMake-built mkisofs packaging marker missing: ${needle}")
  endif()
endforeach()
foreach(forbidden "schilytools-mkisofs" "Building bundled SchilyTools mkisofs" "BDMVAUTHOR_SCHILY_WORK_DIR")
  string(FIND "${WIN}" "${forbidden}" pos)
  if(NOT pos EQUAL -1)
    message(FATAL_ERROR "stale SchilyTools Windows build path remains: ${forbidden}")
  endif()
endforeach()
foreach(needle
  "tools_.mkisofs=resolve_sibling_default_tool"
  "c.mkisofs=resolve_tool(config.mkisofs,\"mkisofs\",true)")
  string(FIND "${AUTHOR}${GUI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "cross-platform sibling mkisofs marker missing: ${needle}")
  endif()
endforeach()
foreach(needle
  "builtins.readFile ./VERSION"
  "-DBDMVAUTHOR_BUILD_BUNDLED_MKISOFS=ON"
  "cmake -S third_party/mkisofs -B mkisofs-portable-build"
  "mkisofsSourceVersion = \"cdrtools-3.02a09\""
  "mkisofsBundled = true")
  string(FIND "${FLAKE}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "Nix CMake-built mkisofs marker missing: ${needle}")
  endif()
endforeach()
foreach(forbidden "mkSchilyMkisofs" "schilytools/archive" "make -C mkisofs" "psmake")
  string(FIND "${FLAKE}" "${forbidden}" pos)
  if(NOT pos EQUAL -1)
    message(FATAL_ERROR "stale SchilyTools/smake Nix build path remains: ${forbidden}")
  endif()
endforeach()
foreach(needle "FIXTURE_WRITER" "VIDEO_TS.IFO" "-dvd-video" "Missing pathspec" "Unknown error -1" "4e53523032")
  string(FIND "${RUNTIME}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "mkisofs runtime DVD-Video regression marker missing: ${needle}")
  endif()
endforeach()
if(EXISTS "${SOURCE_DIR}/tests/fixtures/minimal-video-ts.ifo")
  message(FATAL_ERROR "binary mkisofs fixture should be generated by the test helper, not stored in source")
endif()
foreach(forbidden_dir "${SOURCE_DIR}/third_party/schilytools-mkisofs")
  if(EXISTS "${forbidden_dir}")
    message(FATAL_ERROR "stale fetched-SchilyTools vendoring directory remains: ${forbidden_dir}")
  endif()
endforeach()
foreach(forbidden_file
  "${SOURCE_DIR}/third_party/mkisofs/src/cdrecord.c"
  "${SOURCE_DIR}/third_party/mkisofs/src/cdda2wav.c"
  "${SOURCE_DIR}/third_party/mkisofs/src/readcd.c"
  "${SOURCE_DIR}/third_party/mkisofs/src/scsi.c"
  "${SOURCE_DIR}/third_party/mkisofs/src/apple.c"
  "${SOURCE_DIR}/third_party/mkisofs/src/volume.c"
  "${SOURCE_DIR}/third_party/mkisofs/src/desktop.c"
  "${SOURCE_DIR}/third_party/mkisofs/src/mac_label.c")
  if(EXISTS "${forbidden_file}")
    message(FATAL_ERROR "unrelated cdrtools program was copied into minimal mkisofs subset: ${forbidden_file}")
  endif()
endforeach()
message(STATUS "minimal CMake-built cdrtools mkisofs source checks ok")
