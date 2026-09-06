/*
 * Minimal cdrtools compatibility layer used by BDMV Author's private mkisofs.
 * This intentionally implements only APIs referenced by the selected mkisofs
 * sources; it does not attempt to recreate libschily as a general library.
 */
#include <schily/mconfig.h>
#include <schily/standard.h>
#include <schily/schily.h>
#include <schily/checkerr.h>
#include <schily/errno.h>
#include <schily/stdio.h>
#include <schily/string.h>
#include <schily/unistd.h>

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) && !defined(HAVE_GETTIMEOFDAY)
#include <windows.h>
#include <schily/time.h>

/* Convert the Windows 100 ns FILETIME epoch (1601) to Unix timeval (1970). */
int
gettimeofday(struct timeval *tv, void *tz)
{
    FILETIME ft;
    ULARGE_INTEGER ticks;
    const unsigned long long epoch_100ns = 116444736000000000ULL;
    (void)tz;
    if (tv == NULL)
        return -1;
    GetSystemTimeAsFileTime(&ft);
    ticks.LowPart = ft.dwLowDateTime;
    ticks.HighPart = ft.dwHighDateTime;
    if (ticks.QuadPart < epoch_100ns)
        return -1;
    ticks.QuadPart -= epoch_100ns;
    tv->tv_sec = (long)(ticks.QuadPart / 10000000ULL);
    tv->tv_usec = (long)((ticks.QuadPart % 10000000ULL) / 10ULL);
    return 0;
}
#endif

static int compat_argc;
static char **compat_argv;

void
save_args(int argc, char **argv)
{
    compat_argc = argc;
    compat_argv = argv;
}

int
saved_ac(void)
{
    return compat_argc;
}

char **
saved_av(void)
{
    return compat_argv;
}

char *
saved_av0(void)
{
    return compat_argv != NULL && compat_argv[0] != NULL ? compat_argv[0] : (char *)"mkisofs";
}

int
geterrno(void)
{
    return errno;
}

int
seterrno(int err)
{
    int old = errno;
    errno = err;
    return old;
}

char *
fillbytes(void *dst, ssize_t count, char value)
{
    if (count > 0)
        memset(dst, (unsigned char)value, (size_t)count);
    return (char *)dst;
}

char *
movebytes(const void *src, void *dst, ssize_t count)
{
    if (count > 0)
        memmove(dst, src, (size_t)count);
    return (char *)dst;
}

ssize_t
ffileread(FILE *stream, void *buf, size_t len)
{
    int fd = fileno(stream);
    ssize_t ret;
    int old_errno = errno;
    do {
        ret = read(fd, buf, len);
    } while (ret < 0 && errno == EINTR);
    if (ret >= 0)
        errno = old_errno;
    return ret;
}

int
streql(const char *a, const char *b)
{
    return a != NULL && b != NULL && strcmp(a, b) == 0;
}

static const char *
compat_progname(void)
{
    const char *name = saved_av0();
    const char *slash;
    const char *backslash;
    if (name == NULL || *name == '\0')
        return "mkisofs";
    slash = strrchr(name, '/');
    backslash = strrchr(name, '\\');
    if (slash != NULL && (backslash == NULL || slash > backslash))
        name = slash + 1;
    else if (backslash != NULL)
        name = backslash + 1;
    return name;
}

static int
compat_vmessage(int err, const char *fmt, va_list ap)
{
    int saved = errno;
    int ret;
    fprintf(stderr, "%s: ", compat_progname());
    if (err >= 0)
        fprintf(stderr, "%s. ", strerror(err));
    ret = vfprintf(stderr, fmt, ap);
    if (fmt == NULL || *fmt == '\0' || fmt[strlen(fmt) - 1] != '\n')
        fputc('\n', stderr);
    errno = saved;
    return ret;
}

void
comerr(const char *fmt, ...)
{
    va_list ap;
    int err = errno;
    va_start(ap, fmt);
    (void)compat_vmessage(err, fmt, ap);
    va_end(ap);
    exit(err > 0 && err < 256 ? err : 1);
}

void
comerrno(int err, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    (void)compat_vmessage(err, fmt, ap);
    va_end(ap);
    exit(err > 0 && err < 256 ? err : 1);
}

int
errmsg(const char *fmt, ...)
{
    va_list ap;
    int err = errno;
    int ret;
    va_start(ap, fmt);
    ret = compat_vmessage(err, fmt, ap);
    va_end(ap);
    return ret;
}

int
errmsgno(int err, const char *fmt, ...)
{
    va_list ap;
    int ret;
    va_start(ap, fmt);
    ret = compat_vmessage(err, fmt, ap);
    va_end(ap);
    return ret;
}

int
error(const char *fmt, ...)
{
    va_list ap;
    int ret;
    va_start(ap, fmt);
    ret = vfprintf(stderr, fmt, ap);
    va_end(ap);
    return ret;
}

int
js_error(const char *fmt, ...)
{
    va_list ap;
    int ret;
    va_start(ap, fmt);
    ret = vfprintf(stderr, fmt, ap);
    va_end(ap);
    return ret;
}

int
js_printf(const char *fmt, ...)
{
    va_list ap;
    int ret;
    va_start(ap, fmt);
    ret = vprintf(fmt, ap);
    va_end(ap);
    return ret;
}

int
js_fprintf(FILE *stream, const char *fmt, ...)
{
    va_list ap;
    int ret;
    va_start(ap, fmt);
    ret = vfprintf(stream, fmt, ap);
    va_end(ap);
    return ret;
}

int
js_sprintf(char *dst, const char *fmt, ...)
{
    va_list ap;
    int ret;
    va_start(ap, fmt);
    ret = vsprintf(dst, fmt, ap);
    va_end(ap);
    return ret;
}

int
js_snprintf(char *dst, size_t size, const char *fmt, ...)
{
    va_list ap;
    int ret;
    va_start(ap, fmt);
    ret = vsnprintf(dst, size, fmt, ap);
    va_end(ap);
    return ret;
}

/* getargs only raises this condition for an invalid internal format string. */
void
raisecond(const char *name, void *arg)
{
    (void)arg;
    fprintf(stderr, "mkisofs: internal condition raised: %s\n", name != NULL ? name : "unknown");
    abort();
}

/*
 * BDMV Author does not expose mkisofs' generic error-control configuration.
 * Retain the default mkisofs behavior: errors are visible and fatal when the
 * caller asks errabort() to terminate.  Accepting errconfig() preserves the
 * startup default and option parser without importing libschily's pattern
 * compiler solely for this private helper.
 */
int
errconfig(char *name)
{
    (void)name;
    return 1;
}

BOOL
errhidden(int etype, const char *fname)
{
    (void)etype;
    (void)fname;
    return FALSE;
}

BOOL
errwarnonly(int etype, const char *fname)
{
    (void)etype;
    (void)fname;
    return FALSE;
}

BOOL
errabort(int etype, const char *fname, BOOL doexit)
{
    (void)etype;
    (void)fname;
    if (doexit)
        exit(1);
    return TRUE;
}
