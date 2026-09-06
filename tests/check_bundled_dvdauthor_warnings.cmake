if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "SOURCE_DIR not supplied")
endif()

file(READ "${SOURCE_DIR}/third_party/dvdauthor/CMakeLists.txt" DVDCMAKE)
file(READ "${SOURCE_DIR}/third_party/dvdauthor/src/config.h.cmake" CONFIG)
file(READ "${SOURCE_DIR}/third_party/dvdauthor/src/compat.h" COMPAT_H)
file(READ "${SOURCE_DIR}/third_party/dvdauthor/src/compat.c" COMPAT_C)
file(READ "${SOURCE_DIR}/third_party/dvdauthor/src/dvdvob.c" DVDVOB)
file(READ "${SOURCE_DIR}/third_party/dvdauthor/src/dvdauthor.c" DVDA)
file(READ "${SOURCE_DIR}/third_party/dvdauthor/src/dvdvml.l" DVDVML)
file(READ "${SOURCE_DIR}/third_party/dvdauthor/src/subgen.c" SUBGEN)
file(READ "${SOURCE_DIR}/third_party/dvdauthor/src/subreader.c" SUBREADER)
file(READ "${SOURCE_DIR}/third_party/dvdauthor/src/subfont.c" SUBFONT)
file(READ "${SOURCE_DIR}/third_party/dvdauthor/src/subrender.c" SUBRENDER)
file(READ "${SOURCE_DIR}/third_party/dvdauthor/src/dvdcli.c" DVDCLI)
file(READ "${SOURCE_DIR}/third_party/dvdauthor/src/subgen-image.c" SUBGEN_IMAGE)

foreach(needle
  "target_compile_options(dvdauthor PRIVATE -Wall -Wextra)"
  "target_compile_options(spumux PRIVATE -Wall -Wextra)"
  "if(BDMVAUTHOR_WARNINGS_AS_ERRORS)"
  "target_compile_options(dvdauthor PRIVATE -Werror)"
  "target_compile_options(spumux PRIVATE -Werror)")
  string(FIND "${DVDCMAKE}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "bundled dvdauthor warning policy missing: ${needle}")
  endif()
endforeach()

string(FIND "${DVDCMAKE}" "_GNU_SOURCE" cmake_gnu_source)
if(NOT cmake_gnu_source EQUAL -1)
  message(FATAL_ERROR "bundled dvdauthor CMake still duplicates _GNU_SOURCE")
endif()
foreach(needle
  "#ifndef _GNU_SOURCE"
  "#define _GNU_SOURCE"
  "#define PACKAGE_STRING PACKAGE_NAME \" \" PACKAGE_VERSION")
  if(needle MATCHES "PACKAGE_STRING")
    string(FIND "${CONFIG}" "${needle}" pos)
  else()
    string(FIND "${COMPAT_H}" "${needle}" pos)
  endif()
  if(pos EQUAL -1)
    message(FATAL_ERROR "bundled dvdauthor compatibility marker missing: ${needle}")
  endif()
endforeach()

foreach(needle
  "#if defined(_WIN32)"
  "_commit(fd)"
  "fdatasync(fd)"
  "fsync(fd)")
  string(FIND "${DVDVOB}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "VOB flush portability marker missing: ${needle}")
  endif()
endforeach()
string(FIND "${DVDVOB}" "#if defined(_WIN32)\n        _commit(fd)" win_commit)
if(win_commit EQUAL -1)
  message(FATAL_ERROR "Windows VOB flush does not select _commit before POSIX sync calls")
endif()

foreach(needle
  "static void read_ifo_sector"
  "const size_t got = fread(buf, 1, 2048, h);"
  "if (got != 2048)"
  "read_ifo_sector(h, buf, ifo, \"VTSI_MAT\");"
  "read_ifo_sector(h, buf, ifo, \"VTS_PTT_SRPT\");")
  string(FIND "${DVDA}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "fortified-I/O warning/correctness marker missing: ${needle}")
  endif()
endforeach()

foreach(needle
  "const int chroma_base = gotlightness"
  "const int chroma_scale = gotlightness ? 2 : 1;"
  "const int chroma = chroma_scale * chroma_base * component[1] / 255;")
  string(FIND "${COMPAT_C}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "GCC-11 conditional-expression warning fix missing: ${needle}")
  endif()
endforeach()

foreach(forbidden
  "fread(buf, 1, 2048, h); // VTS_PTT_SRPT"
  "2\n                                *\n                                    (component[2] < 128 ? component[2] : 255 - component[2])")
  string(FIND "${DVDA}${COMPAT_C}" "${forbidden}" pos)
  if(NOT pos EQUAL -1)
    message(FATAL_ERROR "portable GCC-11 warning-prone source form remains: ${forbidden}")
  endif()
endforeach()

foreach(needle
  "static const char * const vratedesc"
  "static const struct option longopts"
  "unsigned short sh[4] = {0, 0, 0, 0};"
  "base = FRIBIDI_PAR_ON;"
  "memset(current, 0, sizeof *current);"
  "memset(line1, 0, sizeof line1);"
  "/* fall through */"
  "lastgts != UINT64_MAX"
  "sscanf(line, \"%lu,%lu,\\\"%[^\\\"]\"")
  string(FIND "${DVDA}${SUBGEN}${SUBREADER}${DVDVOB}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "warning-clean source marker missing: ${needle}")
  endif()
endforeach()


foreach(bad_subreader
  "FRIBIDI_TYPE_ON"
  "bzero(")
  string(FIND "${SUBREADER}" "${bad_subreader}" pos)
  if(NOT pos EQUAL -1)
    message(FATAL_ERROR "non-portable/ill-typed subreader marker remains: ${bad_subreader}")
  endif()
endforeach()


foreach(needle
  "size_t i, imax = 0;"
  "const size_t exedir_len = GetModuleFileNameA"
  "const unsigned int glyph_width = glyph->bitmap.width;"
  "const unsigned int max_bitmap_width = maxw > 0 ? (unsigned int)maxw : 0U;"
  "const int bitmap_copy_width = glyph_width <= max_bitmap_width ? (int)glyph_width : maxw;")
  string(FIND "${SUBFONT}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "subfont warning-clean marker missing: ${needle}")
  endif()
endforeach()

foreach(bad_subfont
  "static unsigned int const nr_colors"
  "static unsigned int const maxcolor"
  "static unsigned const first_char"
  "glyph->bitmap.width > maxw"
  "glyph->bitmap.width <= maxw")
  string(FIND "${SUBFONT}" "${bad_subfont}" pos)
  if(NOT pos EQUAL -1)
    message(FATAL_ERROR "subfont warning-prone marker remains: ${bad_subfont}")
  endif()
endforeach()

foreach(bad
  "dvdvmlval.int_val<0"
  "dvdvmlval.int_val < 0")
  string(FIND "${DVDVML}" "${bad}" pos)
  if(NOT pos EQUAL -1)
    message(FATAL_ERROR "unsigned dvdvm lexer value still has impossible negative comparison")
  endif()
endforeach()


foreach(needle
  "int chindex, wordlen;"
  "const int kern_prevch ="
  "wordlen != 0 ? wordbuf[wordlen - 1] : (osl != NULL ? ' ' : -1);"
  "bool gotcolors = false, gothue = false, gotlightness = false, gotalpha = false"
  "if (fscanf(h.h, \"%x\", &pcolor) != 1)"
  "if (fread(pnghead, 1, sizeof pnghead, fp) != sizeof pnghead)"
  "s->xd <= (unsigned int)s->img.width"
  "s->xd <= (unsigned int)s->hlt.width"
  "s->xd <= (unsigned int)s->sel.width")
  string(FIND "${SUBRENDER}${DVDCLI}${SUBGEN_IMAGE}${COMPAT_C}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "portable GCC-11 warning/correctness marker missing: ${needle}")
  endif()
endforeach()

foreach(forbidden
  "int chindex, prevch, wordlen;"
  "int chindex, prevch = -1, wordlen;"
  "bool gotcolors, gothue, gotlightness, gotalpha"
  "fscanf(h.h, \"%x\", &pcolor);"
  "fread(pnghead,1,8,fp);")
  string(FIND "${SUBRENDER}${DVDCLI}${SUBGEN_IMAGE}${COMPAT_C}" "${forbidden}" pos)
  if(NOT pos EQUAL -1)
    message(FATAL_ERROR "portable GCC-11 warning-prone source form remains: ${forbidden}")
  endif()
endforeach()

message(STATUS "bundled dvdauthor UCRT64 warning-clean source checks ok")
