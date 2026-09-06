## BDMV Author 0.1.105 portability follow-up

- Native UCRT64 has no `S_IFLNK` symlink path in the minimized mkisofs build. `rock.c` now scopes its Rock Ridge symlink scratch buffer to the same `S_IFLNK` feature guard as the code that uses it, matching the earlier `tree.c` fix and keeping GCC 16 `-Werror` builds warning-clean.
- The bundled-mkisofs source regression now checks both `tree.c` and `rock.c` for correctly feature-scoped symlink state.

## BDMV Author 0.1.104 portability follow-up

- The reported UCRT64 compile command exposed that the CMake port enabled `USE_LARGEFILES` without passing `_FILE_OFFSET_BITS=64`. The port now applies `_FILE_OFFSET_BITS=64` to all configure probes and to the mkisofs target, and configuration fails unless `off_t` is at least 64 bits. The UDF maximum-size comparison additionally uses cdrtools' configured 64-bit `Llong` type, so the cutoff arithmetic is independent of Windows `long` width.
- The CMake configure layer now probes `strlcpy` and `strlcat`. If the host libc lacks either function, the target compiles the exact cdrtools 3.02a09 `libschily/strlcpy.c` / `libschily/strlcat.c` fallback source copied into `support/`. This keeps old-glibc portable builds and native UCRT64 self-contained without importing the rest of libschily.

## BDMV Author 0.1.103 portability follow-up

- Native MinGW does not compile the symlink table path, so `tree.c` now scopes the symlink scratch buffer and `nchar` to `S_IFLNK` builds instead of leaving unused objects under `-Werror`.
- The UDF large-file cutoff was changed to cast both multiplication operands to `off_t`; a later UCRT64 build showed that native Windows still uses a 32-bit `off_t` in this path, so 0.1.104 supersedes this with explicit `Llong` arithmetic.
- The minimal libschily diagnostic shim now follows cdrtools' `_comerr()` convention that negative `EX_BAD` values suppress errno text. It also supplies cdrtools-compatible `error()` and `js_error()` implementations so Linux cannot bind mkisofs calls to glibc's unrelated `error(status, errnum, ...)` ABI.
- The runtime regression now checks the no-pathspec diagnostic and rejects the former `Unknown error -1` output.

# BDMV Author mkisofs port

## Source base

This directory is derived from the user-supplied `cdrtools-3.02a09.tar.gz`
archive (SHA-256 `c7e4f732fb299e9b5d836629dadf5512aa5e6a5624ff438ceb1d056f4dcb07c2`).
The archive extracts as cdrtools 3.02 and identifies the mkisofs program as
3.02a09.

BDMV Author intentionally vendors only mkisofs and the small support subset
needed by this executable. It does not copy or build cdrecord, cdda2wav,
readcd, scgcheck, smake, or the other cdrtools programs.

## Feature boundary

The CMake target retains the image-authoring features used by BDMV Author:
ISO-9660, Rock Ridge, Joliet, UDF, El Torito, sort weights, duplicate-file
coalescing, and DVD-Audio/DVD-Video layout support (`DVD_AUD_VID`).

The private target does not enable HFS hybrid images, direct SCSI device
access/multisession drive probing, the Schily find-expression subsystem, or NLS
message catalogs. Those optional upstream features are the main reason the
original makefile links libhfs, libfile, libscg*, libcdrdeflt/libdeflt,
libfind, and most of libschily; they are not needed by BDMV Author's
`mkisofs -dvd-video` file-to-image invocation.

## Copied upstream support code

In addition to the mkisofs program files needed by the enabled feature set and
the headers those files include, the target
uses only these source files copied from other cdrtools libraries:

- `libschily/getargs.c`
- `libschily/astoll.c`
- `libschily/fnmatch.c`
- `libmdigest/sha3.c`
- `libschily/strlcpy.c` (fallback when libc lacks `strlcpy`)
- `libschily/strlcat.c` (fallback when libc lacks `strlcat`)

`support/compat.c` and `support/siconv_compat.c` are BDMV Author compatibility
implementations for the small libschily/libsiconv API surface that remains.
They replace those full libraries rather than importing unrelated source.

## Local source changes

Unused HFS/direct-SCSI implementation units (`apple.c`, `volume.c`, `desktop.c`,
`mac_label.c`, and `scsi.c`) are not copied into the vendored subset.

The retained mkisofs program sources remain close to cdrtools 3.02a09. Local changes
are limited to portability, warning correctness, and compiling out dormant HFS
resource-fork code when HFS is disabled:

- `mkisofs.c`: private version header and no Unix uid reset on native Windows.
- `tree.c`: Windows uid/gid defaults and ignored nanosecond-helper returns.
- `udf.c`, `write.c`, `mkisofs.h`: isolate dormant HFS metadata paths while
  retaining UDF/DVD operation without libhfs.
- `eltorito.c`, `joliet.c`, `multi.c`, `name.c`, `stream.c`, `write.c`: modern
  signedness/return-value/unused-code fixes needed for warning-clean builds.
- `getargs.c`, `astoll.c`, `fnmatch.c`: modern type and control-flow fixes.

The earlier SchilyTools UCRT64 v22 port was used as a portability reference.
In particular, this subset carries its LLP64-safe `raisecond(..., void *)`
interface (instead of passing pointers through 32-bit Windows `long`) and its
`NO_NLS` libintl guard. The generated CMake configuration also maps POSIX
`fseeko`/`ftello` to UCRT `_fseeki64`/`_ftelli64` when needed, and the compact
compatibility layer supplies `gettimeofday` on native Windows.

## Build system

`CMakeLists.txt` is BDMV Author-specific. It replaces the cdrtools RULES/smake
build completely and builds a single executable target whose output name is
`mkisofs`. No cdrtools bootstrap make program is built or run.

## BDMV Author 0.1.102 portability follow-up

The CMake configure layer now reproduces cdrtools' Autoconf fallback types for
platforms such as native MinGW-w64 that do not define `uid_t`, `gid_t`, or
`nlink_t` (`int`, `int`, and `unsigned long`, respectively). It also probes for
CRT-provided `errno` and `environ` definitions so the copied Schily headers do
not redeclare UCRT accessor-backed globals without their `dllimport` attributes.

`eltorito.c` now checks the boot-info-table seek/write/close operations and
handles short/interrupted writes. Besides satisfying fortified Linux builds with
`-Werror=unused-result`, this prevents silent creation of an image whose boot
information table was only partially updated.

## BDMV Author 0.1.106 follow-up

Native MSYS2 UCRT64 GCC uses separate Microsoft and GNU printf format
dialects for `__attribute__((format(...)))`.  cdrtools' retained multisession
validation diagnostics use standard C99 `%zu`/`%zd` modifiers, while the
upstream Schily compiler helper still selected generic `__printf__`.  The
minimal port now selects `__gnu_printf__`/`__gnu_scanf__` for native MinGW
GCC (and keeps `__printf__`/`__scanf__` for Clang and non-MinGW targets),
matching mingw-w64 UCRT's own stdio annotations.  No warning suppression or
format-string rewriting is used.
