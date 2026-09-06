#pragma once

#define PACKAGE_NAME "DVDAuthor"
#define PACKAGE_VERSION "0.7.2+-bdmvauthor"
#define PACKAGE_STRING PACKAGE_NAME " " PACKAGE_VERSION
#define PACKAGE_BUGREPORT "dvdauthor-users@lists.sourceforge.net"

#define HAVE_STDBOOL_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STDINT_H 1
#define HAVE_STRINGS_H 1
#define HAVE_STRING_H 1
#define HAVE_UNISTD_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_MEMORY_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_GETOPT_H 1
#define HAVE_IO_H 1
#define HAVE_GETOPT_LONG 1
#define HAVE_DECL_O_BINARY 1
#define HAVE_ICONV 1
#define HAVE_FREETYPE 1
#define HAVE_FT2BUILD_H 1
#define HAVE_FONTCONFIG 1
#define HAVE_FRIBIDI 1

/* Keep the fallback strndup implementation for consistent UCRT behavior. */
/* #undef HAVE_STRNDUP */
/* Nested GCC routines are unnecessary for dvdauthor/spumux and are disabled. */
/* #undef HAVE_NESTED_ROUTINES */
/* DVD unauthoring is not part of the bundled helper subset. */
/* #undef HAVE_DVDREAD */
/* Filenames remain UTF-8 as in the default upstream configuration. */
/* #undef LOCALIZE_FILENAMES */
