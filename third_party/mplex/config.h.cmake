#ifndef BDMVAUTHOR_MJPEGTOOLS_CONFIG_H
#define BDMVAUTHOR_MJPEGTOOLS_CONFIG_H

/* Minimal configuration used by BDMV Author's mplex-only CMake build. */
#define HAVE_GETOPT_H 1
#define HAVE_GETOPT_LONG 1
#define HAVE_LROUND 1
#define HAVE_FMAX 1
#define HAVE_FMIN 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_MALLOC_H 1
#define HAVE_MEMALIGN 1
#define HAVE_POSIX_MEMALIGN 1
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_UNISTD_H 1
#define STDC_HEADERS 1
#define MJPEGTOOLS 1
#define PACKAGE "mjpegtools"
#define PACKAGE_NAME "mjpegtools"
#define PACKAGE_STRING "mjpegtools 2.2.3"
#define PACKAGE_TARNAME "mjpegtools"
#define PACKAGE_VERSION "2.2.3"
#define VERSION "2.2.3"

#if defined(_WIN32) && !defined(__CYGWIN__)
# define strcasecmp _stricmp
# define strncasecmp _strnicmp
#endif

#endif
