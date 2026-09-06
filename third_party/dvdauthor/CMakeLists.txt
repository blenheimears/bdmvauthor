# Minimal CMake build glue for the vendored dvdauthor/spumux subset.
# BDMV Author uses it for native Windows/UCRT64 and portable Linux builds.
# Upstream's Autotools files are retained for other source-tree users.
cmake_minimum_required(VERSION 3.20)

find_package(BISON REQUIRED)
find_package(FLEX REQUIRED)
find_package(LibXml2 REQUIRED)
find_package(PNG REQUIRED)
find_package(Freetype REQUIRED)
find_package(Iconv REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(DVDA_FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)
pkg_check_modules(DVDA_FRIBIDI REQUIRED IMPORTED_TARGET fribidi)

set(DVDA_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${DVDA_GENERATED_DIR}")
if(WIN32)
  configure_file(src/config.h.cmake "${DVDA_GENERATED_DIR}/config.h" COPYONLY)
else()
  configure_file(src/config-linux.h.cmake "${DVDA_GENERATED_DIR}/config.h" COPYONLY)
endif()

BISON_TARGET(dvdvm_parser src/dvdvmy.y "${DVDA_GENERATED_DIR}/dvdvmy.c"
  DEFINES_FILE "${DVDA_GENERATED_DIR}/dvdvmy.h"
  COMPILE_FLAGS "-p dvdvm")
FLEX_TARGET(dvdvm_lexer src/dvdvml.l "${DVDA_GENERATED_DIR}/dvdvml.c"
  COMPILE_FLAGS "-s -B -Cem -Pdvdvm")
ADD_FLEX_BISON_DEPENDENCY(dvdvm_lexer dvdvm_parser)

set(DVDA_COMMON_COMPILE_DEFINITIONS
  _FILE_OFFSET_BITS=64
)
set(DVDA_COMMON_INCLUDE_DIRS
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${DVDA_GENERATED_DIR}"
)

add_executable(dvdauthor
  src/dvdauthor.c
  src/dvdcompile.c
  "${DVDA_GENERATED_DIR}/dvdvml.c"
  "${DVDA_GENERATED_DIR}/dvdvmy.c"
  src/dvdifo.c
  src/dvdvob.c
  src/dvdpgc.c
  src/dvdcli.c
  src/readxml.c
  src/conffile.c
  src/compat.c
)
target_include_directories(dvdauthor PRIVATE ${DVDA_COMMON_INCLUDE_DIRS})
target_compile_definitions(dvdauthor PRIVATE ${DVDA_COMMON_COMPILE_DEFINITIONS})
target_compile_options(dvdauthor PRIVATE -Wall -Wextra)
if(BDMVAUTHOR_WARNINGS_AS_ERRORS)
  target_compile_options(dvdauthor PRIVATE -Werror)
endif()
target_link_libraries(dvdauthor PRIVATE LibXml2::LibXml2 Iconv::Iconv)
if(WIN32)
  target_link_libraries(dvdauthor PRIVATE ws2_32)
endif()

add_executable(spumux
  src/subgen.c
  src/subgen-parse-xml.c
  src/readxml.c
  src/subgen-encode.c
  src/subgen-image.c
  src/conffile.c
  src/compat.c
  src/subrender.c
  src/subreader.c
  src/subfont.c
)
target_include_directories(spumux PRIVATE ${DVDA_COMMON_INCLUDE_DIRS})
target_compile_definitions(spumux PRIVATE ${DVDA_COMMON_COMPILE_DEFINITIONS})
target_compile_options(spumux PRIVATE -Wall -Wextra)
if(BDMVAUTHOR_WARNINGS_AS_ERRORS)
  target_compile_options(spumux PRIVATE -Werror)
endif()
target_link_libraries(spumux PRIVATE
  LibXml2::LibXml2
  PNG::PNG
  Freetype::Freetype
  PkgConfig::DVDA_FRIBIDI
  PkgConfig::DVDA_FONTCONFIG
  Iconv::Iconv
)
if(WIN32)
  target_link_libraries(spumux PRIVATE ws2_32)
else()
  target_link_libraries(spumux PRIVATE m)
endif()

# BDMV Author resolves private helpers beside its own executables.
set_target_properties(dvdauthor spumux PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
