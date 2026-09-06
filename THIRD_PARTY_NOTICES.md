# Third-party notices

## udf25mkiso 0.1.9

`third_party/udf25mkiso/udf_writer.cpp` and `udf_writer.hpp` are from the earlier standalone UDF 2.50 image creator made for this project lineage. They are Apache License 2.0; the original LICENSE and NOTICE are retained in that directory.

## tsMuxer — vendored GitHub snapshot

tsMuxer is Apache-2.0 licensed.

BDMV Author builds the tsMuxer command-line target and its `libmediation` support library as a private companion executable. The separate tsMuxer GUI is not built. Local changes made for embedding/build portability are documented in `third_party/tsmuxer/BDMVAUTHOR-VENDORING.md`.

## dvdauthor / spumux — vendored source

BDMV Author vendors the GPL-2.0-or-later dvdauthor source under `third_party/dvdauthor` and builds only the `dvdauthor` and `spumux` helpers needed for DVD-Video authoring. The project-local CMake port supports native MSYS2 UCRT64 Windows and the portable-Linux target; unrelated dvdauthor utilities are not built by that CMake path. The upstream `COPYING` file is retained in the vendored source tree.

## cdrtools 3.02a09 / mkisofs

BDMV Author vendors a deliberately minimal source subset of `mkisofs` from cdrtools 3.02a09 distribution under `third_party/mkisofs`. It does not vendor or build cdrecord, cdda2wav, readcd, scgcheck, smake, or the rest of the cdrtools programs. A project-local CMake port builds the private `mkisofs` executable directly on Linux and native MSYS2 UCRT64 Windows. The retained source enables the ISO9660/Rock Ridge/Joliet/UDF/DVD-Video functionality used by BDMV Author and carries the applicable upstream GPL-2.0/CDDL notices in that directory.

## mplex — from mjpegtools SVN r3517

BDMV Author vendors only the source required to build `mplex` under `third_party/mplex`, taken from the mjpegtools SVN r3517 snapshot. The retained tree contains the upstream mplex source/header files and only the small GPL utility subset directly required to compile/link mplex; source for unrelated mjpegtools programs and unused build files is omitted. DVD-Video authoring uses the DVD (`-f 8`) profile so `dvdauthor` receives program streams with recurring NAV/VOBU sectors. The upstream GPLv2 license text is retained, and `BDMVAUTHOR-VENDORING.md` records the supplied snapshot hash and subset policy. Windows and portable Linux build this vendored subset as a private sibling helper; the normal Nix package may continue to use its packaged mjpegtools implementation through its package environment.

## FFmpeg / ffprobe — externally supplied runtime

Native Windows packages stage `ffmpeg.exe` and `ffprobe.exe` from the MSYS2 UCRT64 `mingw-w64-ucrt-x86_64-ffmpeg` package by default. The Windows build helper can instead be given a strict `--ffmpeg-dir` containing alternate executables, in which case those exact binaries are redistributed and their dependency closure is staged from the custom directory and normal UCRT64/runtime sources as required. BDMV Author does not rebuild or vendor FFmpeg source in-tree; source remains available from the provider of the selected FFmpeg build and the upstream FFmpeg project.

## NSIS — Windows installer build system

Native MSYS2 UCRT64 builds use the distribution-provided NSIS toolchain to create the Windows setup executable after runtime staging succeeds. NSIS source is not vendored into BDMV Author. The setup license page distinguishes BDMV Author's Apache-2.0 license from third-party component licenses, and the installed runtime carries the main license, these notices, and component license texts.

## libbluray

libbluray is used at test time as an interoperability reference for HDMV control-file decoding and VLC compatibility behavior. No libbluray source is copied into this project.

## OpenSSL

BDMV Author links against the system OpenSSL `libcrypto` implementation for SHA-256 media fingerprints used by persistent encode-cache keys. OpenSSL source is not vendored in this project.
