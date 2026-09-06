# BDMV Author native-Windows port

This directory is the user-supplied dvdauthor Git master snapshot vendored by
BDMV Author 0.1.95. The original source and GPL-2.0-or-later licensing are
preserved; see `COPYING` and the upstream source headers.

BDMV Author adds a small CMake build for the two programs it uses (`dvdauthor`
and `spumux`) and native-UCRT portability fixes:

- use Winsock byte-order declarations instead of `<netinet/in.h>` on Windows;
- use `_mkdir()` through `portable_mkdir()` because the Windows CRT mkdir API
  does not take a POSIX mode argument;
- use `_setmode()` for binary stdin/stdout on native Windows;
- use the normal Win32 `GetModuleFileNameA` declaration;
- use `GetACP()` instead of POSIX `nl_langinfo(CODESET)` for the active Windows code page; and
- compile with 64-bit file offsets so VOB output is not limited to 2 GiB.

The otherwise-empty upstream `NEWS` placeholder contains only a one-line pointer to this port note so ordinary unified patches can reproduce the vendored tree exactly.


## 0.1.96 follow-up

The first native UCRT64 compilation exposed an additional POSIX assumption in `dvdvob.c`: flushing a completed VOB called `fsync()`. Native Windows now uses `_commit()` for the same durability step. The 0.1.96 integration also cleans the GCC warning classes encountered during the Windows build and makes the helper targets honor BDMV Author's warnings-as-errors setting. These are portability/diagnostic fixes; the DVD authoring logic is otherwise unchanged.


## 0.1.97 follow-up

MSYS2 UCRT64 GCC 16 found a `-Wmaybe-uninitialized` issue in `subgen.c` while compiling `spumux` with warnings as errors. The four-element highlight/selection palette accumulator now starts at `{0, 0, 0, 0}` before nibble packing. That is the intended initial state of the accumulator and avoids reading/shifting indeterminate values.


## 0.1.98 follow-up

MSYS2 UCRT64 GCC 16 next exposed two `subreader.c` portability issues in the FriBidi/JacoSub feature path. `FriBidiParType` now receives `FRIBIDI_PAR_ON` rather than the character-type enum `FRIBIDI_TYPE_ON`, and the JacoSub reader uses standard `memset()` instead of BSD/POSIX `bzero()`.

## 0.1.99 follow-up

The next native UCRT64 pass exposed GCC 16 diagnostics in `subfont.c`. The Windows executable-directory scan now uses `size_t`, FreeType bitmap-width comparisons/clamping use compatible unsigned bounds and an explicit integer copy width, and obsolete unused declarations are removed.

The CMake port intentionally does not build `dvdunauthor`, `spuunmux`,
`mpeg2desc`, or `dvddirdel`, because BDMV Author does not invoke those tools.

## 0.1.109 portable-Linux follow-up

The retained CMake build is now also used explicitly by BDMV Author's portable-Linux target. Linux uses `src/config-linux.h.cmake`, omits the Windows `ws2_32` dependency, and links `spumux` with the ordinary math library. The normal default remains platform-specific at the BDMV Author top level: bundled dvdauthor/spumux are enabled automatically on Windows and only enabled explicitly where a Linux package intends to ship the private helpers.

## 0.1.111 portable-Linux GCC-11 follow-up

The Debian-12-era portable build uses GCC 11 plus fortified libc headers and `-Werror`. Two upstream-era source forms became fatal there: `ScanIfo()` ignored two `fread()` results, and the color parser used a deeply nested ternary/arithmetic chroma expression that GCC 11 warns about. BDMV Author now checks both IFO sector reads for the full 2048 bytes and fails on truncation, and expresses the chroma calculation through explicit base and scale intermediates. No warning class is disabled.

## 0.1.113 portable-Linux follow-up

The retained Debian-12/GCC-11 portable build exposed additional warnings after the first Linux cleanup. Subtitle word layout initializes `prevch` at declaration; palette and PNG header input now require successful complete reads; and image-dimension assertions validate signed dimensions before unsigned comparisons. These are source-level correctness/portability fixes and `-Werror` remains enabled.

## 0.1.114 portable-Linux follow-up

GCC 11's optimizer still reported two maybe-uninitialized diagnostics despite logical initialization. The subtitle word-width pass now derives the kerning predecessor from the word buffer/list state instead of a mutable local, and the color parser initializes every state flag unconditionally.
