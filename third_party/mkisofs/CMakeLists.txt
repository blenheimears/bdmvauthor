cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(bdmvauthor_mkisofs_standalone LANGUAGES C)
  option(BDMVAUTHOR_WARNINGS_AS_ERRORS "Treat bundled mkisofs warnings as errors" ON)
endif()

include(CheckIncludeFile)
include(CheckFunctionExists)
include(CheckSymbolExists)
include(CheckTypeSize)
include(CheckCSourceCompiles)

find_package(Iconv REQUIRED)

# cdrtools large-file mode requires off_t and struct stat::st_size to be 64-bit.
# This is especially important on MinGW-w64, where the default off_t may remain
# 32-bit unless _FILE_OFFSET_BITS=64 is visible before CRT headers are included.
# Use the same feature definition for configure probes and the final target.
list(APPEND CMAKE_REQUIRED_DEFINITIONS -D_FILE_OFFSET_BITS=64)

set(_mkisofs_headers
  assert.h stdarg.h stdint.h inttypes.h stddef.h stdlib.h string.h strings.h errno.h
  ctype.h unistd.h io.h direct.h fcntl.h dirent.h sys/types.h sys/stat.h sys/time.h
  sys/resource.h sys/param.h time.h limits.h memory.h wchar.h wctype.h iconv.h)
foreach(_header IN LISTS _mkisofs_headers)
  string(TOUPPER "${_header}" _key)
  string(REGEX REPLACE "[^A-Z0-9]" "_" _key "${_key}")
  check_include_file("${_header}" "HAVE_${_key}")
endforeach()

# The original cdrtools configure script supplies fallback POSIX ownership/link
# types on systems whose sys/types.h does not define them.  Native MinGW-w64
# intentionally lacks these typedefs even though struct stat contains analogous
# short fields, so reproduce the upstream configure fallbacks in xconfig.h.
check_c_source_compiles("#include <sys/types.h>\nint main(void) { uid_t v = 0; (void)v; return 0; }" HAVE_UID_T)
check_c_source_compiles("#include <sys/types.h>\nint main(void) { gid_t v = 0; (void)v; return 0; }" HAVE_GID_T)
check_c_source_compiles("#include <sys/types.h>\nint main(void) { nlink_t v = 0; (void)v; return 0; }" HAVE_NLINK_T)

check_c_source_compiles("#include <sys/types.h>\ntypedef char bdmvauthor_off_t_must_be_64_bit[(sizeof(off_t) >= 8) ? 1 : -1];\nint main(void) { return (int)sizeof(bdmvauthor_off_t_must_be_64_bit); }" BDMVA_MKISOFS_OFF_T_64)
if(NOT BDMVA_MKISOFS_OFF_T_64)
  message(FATAL_ERROR "Bundled mkisofs requires a 64-bit off_t with _FILE_OFFSET_BITS=64")
endif()

# Avoid redeclaring CRT globals.  UCRT/MSVCRT exposes errno and environ through
# accessor-backed declarations/macros carrying dllimport attributes; a plain
# Schily extern declaration conflicts with those declarations under -Werror.
if(WIN32)
  set(HAVE_ERRNO_DEF 1)
  set(HAVE_ENVIRON_DEF 1)
else()
  check_c_source_compiles("#include <errno.h>\nint main(void) { int v = errno; (void)v; return 0; }" HAVE_ERRNO_DEF)
  check_c_source_compiles("#include <stdlib.h>\nint main(void) { char **v = environ; (void)v; return 0; }" HAVE_ENVIRON_DEF)
endif()

if(HAVE_TIME_H AND HAVE_SYS_TIME_H)
  set(TIME_WITH_SYS_TIME 1)
endif()
if(NOT WIN32)
  set(HAVE__DEV_NULL 1)
endif()
set(_mkisofs_functions
  fseeko ftello snprintf vsnprintf strcasecmp strncasecmp strlcpy strlcat memmove memcpy memset
  getcwd strerror rename putenv lstat readlink symlink gettimeofday gethostname
  select getpagesize malloc calloc realloc ftruncate chown fchown chmod fchmod
  mkdir rmdir link unlink access dup dup2 pipe waitpid fork vfork nice sleep usleep
  setreuid seteuid setuid iswprint mbtowc wctomb mbrtowc wcrtomb mbsinit)
foreach(_fn IN LISTS _mkisofs_functions)
  string(TOUPPER "${_fn}" _key)
  check_function_exists("${_fn}" "HAVE_${_key}")
endforeach()

# cdrtools assumes HAVE_LARGEFILES means fseeko/ftello are available. Native
# UCRT exposes the equivalent 64-bit interfaces as _fseeki64/_ftelli64; the
# generated xconfig maps them when the POSIX names are absent.
if((HAVE_FSEEKO AND HAVE_FTELLO) OR WIN32)
  set(HAVE_LARGEFILES 1)
endif()
if(WIN32 AND NOT HAVE_FSEEKO)
  set(BDMVA_MKISOFS_MAP_FSEEKO 1)
endif()
if(WIN32 AND NOT HAVE_FTELLO)
  set(BDMVA_MKISOFS_MAP_FTELLO 1)
endif()

check_symbol_exists(stat "sys/stat.h" HAVE_DECL_STAT)
check_symbol_exists(lstat "sys/stat.h" HAVE_DECL_LSTAT)
check_c_source_compiles("#include <sys/time.h>\nint main(void) { struct timeval v; (void)v; return 0; }" HAVE_STRUCT_TIMEVAL)
check_c_source_compiles("#include <sys/time.h>\nint main(void) { struct timezone v; (void)v; return 0; }" HAVE_STRUCT_TIMEZONE)
check_c_source_compiles("#include <time.h>\nint main(void) { struct timespec v; (void)v; return 0; }" HAVE_STRUCT_TIMESPEC)

check_type_size("char" SIZEOF_CHAR LANGUAGE C)
check_type_size("short int" SIZEOF_SHORT_INT LANGUAGE C)
check_type_size("int" SIZEOF_INT LANGUAGE C)
check_type_size("long int" SIZEOF_LONG_INT LANGUAGE C)
check_type_size("long long" SIZEOF_LONG_LONG LANGUAGE C)
if(SIZEOF_LONG_LONG)
  set(HAVE_LONGLONG 1)
endif()
set(SIZEOF_CHAR_P "${CMAKE_SIZEOF_VOID_P}")
check_type_size("unsigned char" SIZEOF_UNSIGNED_CHAR LANGUAGE C)
check_type_size("unsigned short int" SIZEOF_UNSIGNED_SHORT_INT LANGUAGE C)
check_type_size("unsigned int" SIZEOF_UNSIGNED_INT LANGUAGE C)
check_type_size("unsigned long int" SIZEOF_UNSIGNED_LONG_INT LANGUAGE C)
check_type_size("unsigned long long" SIZEOF_UNSIGNED_LONG_LONG LANGUAGE C)

if(Iconv_FOUND)
  set(HAVE_ICONV 1)
  set(HAVE_ICONV_OPEN 1)
  set(HAVE_ICONV_CLOSE 1)
  set(HAVE_ICONV_H 1)
endif()

set(BDMVA_MKISOFS_HOST_CPU "${CMAKE_SYSTEM_PROCESSOR}")
if(NOT BDMVA_MKISOFS_HOST_CPU)
  set(BDMVA_MKISOFS_HOST_CPU "unknown")
endif()
string(TOLOWER "${CMAKE_SYSTEM_NAME}" BDMVA_MKISOFS_HOST_OS)

set(_generated "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${_generated}")
configure_file(cmake/xconfig.h.in "${_generated}/xconfig.h" @ONLY)

set(_mkisofs_program_sources
  src/mkisofs.c src/tree.c src/write.c src/hash.c src/rock.c src/inode.c
  src/udf.c src/multi.c src/joliet.c src/match.c src/name.c src/eltorito.c
  src/boot.c src/isonum.c src/stream.c src/ifo_read.c src/dvd_file.c src/dvd_reader.c
  src/walk.c)
set(_mkisofs_support_sources
  support/getargs.c support/astoll.c support/fnmatch.c support/sha3.c
  support/strlcpy.c support/strlcat.c support/compat.c support/siconv_compat.c)

add_executable(bdmvauthor_mkisofs ${_mkisofs_program_sources} ${_mkisofs_support_sources})
set_target_properties(bdmvauthor_mkisofs PROPERTIES
  OUTPUT_NAME mkisofs
  C_STANDARD 90
  C_STANDARD_REQUIRED OFF
  C_EXTENSIONS ON)

target_include_directories(bdmvauthor_mkisofs PRIVATE
  "${_generated}"
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${CMAKE_CURRENT_SOURCE_DIR}/support")
target_compile_definitions(bdmvauthor_mkisofs PRIVATE
  _FILE_OFFSET_BITS=64
  SCHILY_BUILD
  NO_NLS
  NO_SCHILY_FEXEC
  NO_SCHILY_FSPAWN
  NO_SCHILY_GETLINE
  UDF
  DVD_AUD_VID
  SORTING
  DUPLICATES_ONCE
  SCHILY_PRINT
  USE_ICONV)
if(HAVE_LARGEFILES)
  target_compile_definitions(bdmvauthor_mkisofs PRIVATE USE_LARGEFILES)
endif()
if(WIN32)
  target_compile_definitions(bdmvauthor_mkisofs PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(bdmvauthor_mkisofs PRIVATE
    -Wall -Wextra
    # Upstream getargs intentionally stores short initializers in a larger
    # option-record struct and uses its historical callback ABI with casts.
    -Wno-unused-parameter
    -Wno-missing-field-initializers
    -Wno-cast-function-type)
  if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    # cdrtools 3.02a09 intentionally uses K&R definitions throughout.  Keep
    # the target in GNU90 rather than rewriting upstream solely for Clang's
    # C23-removal deprecation diagnostic.
    target_compile_options(bdmvauthor_mkisofs PRIVATE -Wno-deprecated-non-prototype)
  endif()
  if(BDMVAUTHOR_WARNINGS_AS_ERRORS)
    target_compile_options(bdmvauthor_mkisofs PRIVATE -Werror)
  endif()
endif()

target_link_libraries(bdmvauthor_mkisofs PRIVATE Iconv::Iconv)
if(WIN32)
  target_link_libraries(bdmvauthor_mkisofs PRIVATE ws2_32)
endif()

set_target_properties(bdmvauthor_mkisofs PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

install(TARGETS bdmvauthor_mkisofs RUNTIME DESTINATION bin)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/"
  DESTINATION share/doc/bdmvauthor/third_party/mkisofs
  FILES_MATCHING
  PATTERN "*.c" PATTERN "*.h" PATTERN "*.md" PATTERN "COPYING.GPL2" PATTERN "CDDL.Schily.txt")
