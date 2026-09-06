# BDMV Author

BDMV Author is a native, Java-free authoring application for creating **Blu-ray**, **Ultra HD Blu-ray**, and **DVD-Video** disc images. It provides both a Qt 6 graphical interface and a command-line interface, and can build discs with native interactive menus without requiring BD-J.

The program is designed to take ordinary media files, reuse already-compliant streams when possible, encode or convert streams when necessary, build the disc navigation structures, and produce a finished ISO/UDF disc image suitable for testing, archiving, or burning with separate disc-writing software.

## Features

- **Blu-ray, Ultra HD Blu-ray, and DVD-Video authoring** from a single application.
- **Native menus**:
  - Blu-ray/UHD HDMV menus without Java or BD-J.
  - DVD-Video menus using the standard DVD navigation model.
  - Multiple menus and submenus.
  - Still-image or motion-video backgrounds.
  - Menu audio, looping media, text labels, image labels, and text/image buttons.
  - Buttons can play titles or chapters, open menus, select audio/subtitle streams, turn subtitles off, and execute ordered/repeating action sequences.
- **Menu-less discs** with configurable startup and end-of-title behavior.
- **Multiple audio and subtitle streams** per title.
- **External text subtitles** and styling controls, including font, size, position, color, emphasis, spacing, and fades. Installed fonts are validated rather than silently replaced with generic family names.
- **Chapters** from source metadata, manual chapter points, fixed intervals, or no additional chapters.
- **Video encoding and remuxing** with target-aware resolution, frame-rate, GOP, bitrate, minrate/maxrate, color, VBV/HRD, and codec validation.
- **Audio encoding** including target-appropriate AC-3 and LPCM, with additional Blu-ray/UHD audio options where supported.
- **Compliant-stream passthrough** so compatible Blu-ray/UHD streams can be remuxed instead of unnecessarily re-encoded.
- **Automatic disc-size budgeting** with common DVD, Blu-ray, and UHD capacities, including 8 cm media and an unlimited-size mode.
- **Persistent encode and compliance caches** with a GUI cache manager and automatic cleanup.
- **Versioned `.bdmvproject` project files** that save the complete project and verify referenced media when reopened.
- **GUI and CLI** interfaces using the same authoring core.
- **Debug authoring controls** for deliberate out-of-spec testing. Normal builds enforce the selected format's established limits.

## Supported targets

### Blu-ray

BDMV Author supports standard Blu-ray authoring with target-valid AVC/H.264 and MPEG-2 video modes, supported Blu-ray resolutions and frame rates, native HDMV menus, multiple audio/subtitle streams, chapters, and UDF 2.50 image creation.

### Ultra HD Blu-ray

UHD authoring supports HEVC Main 10 and the project's supported UHD Blu-ray profiles, including 1080p AVC where permitted by the v3 format. UHD 50/59.94/60 fps modes are treated as experimental and are never selected automatically.

UHD subtitle graphics use the 1080-line subtitle plane, so subtitle sizing remains visually consistent with 1080p Blu-ray.

### DVD-Video

DVD-Video authoring supports NTSC, PAL, and Film-DVD timing profiles, MPEG-2 video, DVD-compatible audio, subtitles, chapters, menus, and UDF 1.02/ISO image creation.

## Basic GUI workflow

1. Start `bdmvauthor`.
2. Create a new project and select **Blu-ray**, **Ultra HD Blu-ray**, or **DVD-Video**.
3. Add one or more title files.
4. Configure each title as needed:
   - video codec, bitrate, minrate/maxrate, frame rate, resolution, aspect ratio, GOP, and encoder options;
   - audio streams, codecs, bitrates, channel handling, sample rate, and bit depth;
   - subtitle streams and external subtitles;
   - chapter policy and navigation behavior.
5. Design the menu tree, or remove the visible main menu for a menu-less disc.
6. Choose a disc-size target. Automatic bitrate calculation is enabled by default and attempts to use the available capacity without exceeding the selected target.
7. Choose the output image path and UDF disc label.
8. Click **Build image**.

BDMV Author displays live encoding/build progress and provides a Cancel button while rendering. The completed output is an image file; writing that image to optical media is intentionally left to dedicated disc-burning software.

### External tools

The GUI discovers authoring tools automatically. Paths and preferred encoder providers can be changed under **Settings → External tools and encoders…**.

Packaged builds provide most private helper programs beside BDMV Author. FFmpeg/ffprobe remain a required part of the media-processing toolchain; how they are supplied depends on the platform, as described below.

## Command-line interface

The CLI is named `bdmvauthor-cli`.

A minimal Blu-ray example is:

```sh
bdmvauthor-cli -o disc.iso movie.mkv
```

Select another target with `--target`:

```sh
bdmvauthor-cli -o movie-uhd.iso --target uhd-bluray-2160 movie.mkv
bdmvauthor-cli -o movie-dvd.iso --target dvd-video-480p movie.mkv
```

The CLI supports the same major authoring concepts as the GUI, including menus/submenus, title and chapter buttons, stream-selection buttons, startup sequences, video/audio settings, external subtitles, caching, tool overrides, and menu styling.

Run:

```sh
bdmvauthor-cli --help
```

for the complete option list.

## Third-party software

BDMV Author itself is licensed under the **Apache License 2.0**. Third-party components retain their own licenses. See [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) and the license files in `third_party/` for the complete notices.

### Bundled or vendored components

| Component | Purpose | How it is used |
| --- | --- | --- |
| **tsMuxer** | Blu-ray/UHD transport-stream and disc muxing | Vendored source; the command-line tool is built as a private companion executable. The tsMuxer GUI is not built. |
| **udf25mkiso** | Blu-ray/UHD UDF 2.50 image writing | Integrated into the BDMV Author source tree. |
| **cdrtools `mkisofs`** | DVD-Video ISO/UDF 1.02 image creation | A minimal vendored cdrtools 3.02a09 subset is built as a private helper. |
| **dvdauthor / spumux** | DVD-Video structure and DVD subtitle/menu subpicture authoring | Vendored source. Private helpers are bundled by the Windows and portable-Linux builds; the normal Nix package may use the Nixpkgs-provided tools. |
| **mplex** | DVD-Video MPEG program-stream multiplexing | Minimal vendored mjpegtools r3517 source. Bundled by Windows and portable-Linux builds; the normal Nix package may use the Nixpkgs mjpegtools package. |

### External/runtime components

| Component | Purpose | Requirement |
| --- | --- | --- |
| **FFmpeg / ffprobe** | Media probing, decoding, filtering, audio encoding, and the default video-encoding path | Required. Windows packages stage FFmpeg; the normal Nix package places it on `PATH`; portable generic-Linux builds expect it on the target system. |
| **x264** | Optional standalone H.264/AVC encoder provider | Optional. FFmpeg/libx264 is used by default when available. |
| **x265** | Optional standalone HEVC encoder provider | Optional. FFmpeg/libx265 is used by default when available. |
| **Qt 6.4+** | Graphical interface | Required to build/run the GUI. |
| **OpenSSL / libcrypto** | SHA-256 media fingerprints and cache keys | Required. |
| **FreeType + Fontconfig** | Font discovery and text rendering | Required by packaged Linux builds and used for concrete font rendering. |

The portable Linux build also relies on ordinary host libraries such as glibc, libstdc++, zlib, libxml2, libpng, and FriBidi. `libbluray` is used by the test suite as an interoperability reference; it is not bundled into BDMV Author.

## Building

The repository uses the version in the root `VERSION` file. CMake requires a C++20 compiler.

### NixOS / Nix

The repository contains a flake for the normal NixOS package.

Build it with:

```sh
nix build
```

Then run:

```sh
./result/bin/bdmvauthor
```

The normal Nix package builds the GUI and CLI, bundled tsMuxer and mkisofs, runs the test suite, and wraps the application with the expected FFmpeg/x264/x265 and DVD authoring tools on `PATH`.

For an interactive development environment:

```sh
nix develop
```

A standard CMake development build can then be made from that shell, for example:

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

### Windows: MSYS2 UCRT64

The supported native Windows build uses the **MSYS2 UCRT64** environment.

1. Install MSYS2.
2. Open the **MSYS2 UCRT64** shell, not the MSYS or MINGW64 shell.
3. From the BDMV Author source directory, run:

```sh
./windows/build-msys2-ucrt64.sh
```

The script installs or updates the required UCRT64 packages with `pacman`, including GCC/binutils, CMake, Ninja, Qt 6, OpenSSL, FreeType, Fontconfig, FFmpeg, NSIS, and the libraries needed by the bundled helpers. It then:

- configures a Release build;
- builds the GUI, CLI, tsMuxer, dvdauthor, spumux, mkisofs, and mplex;
- runs the test suite;
- stages Qt plugins and all required non-Windows DLLs;
- stages `ffmpeg.exe` and `ffprobe.exe`;
- strips staged executables and DLLs;
- verifies the final dependency closure; and
- creates an NSIS installer.

Outputs are written under `dist/`:

```text
dist/bdmvauthor-<version>-windows-ucrt64/
dist/bdmvauthor-<version>-windows-ucrt64-setup.exe
```

#### Using a custom FFmpeg build on Windows

To package a different FFmpeg build, provide a directory containing both `ffmpeg.exe` and `ffprobe.exe`:

```sh
./windows/build-msys2-ucrt64.sh --ffmpeg-dir 'C:\path\to\ffmpeg\bin'
```

The override is strict: if the supplied executables are missing or fail their self-test, the build stops rather than silently falling back to the MSYS2 FFmpeg package.

### Generic Linux portable build

The generic-Linux package is built with Nix but is designed to **run on a conventional non-Nix Linux installation**. The build machine therefore needs Nix; the target machine does not.

Supported architectures are **x86_64** and **AArch64**.

Build the portable package with:

```sh
./nix/build-portable-linux.sh
```

or build the underlying flake target directly:

```sh
nix build .#portable-linux
```

The helper script is recommended for release packaging. It creates a versioned directory under `dist/` similar to:

```text
dist/bdmvauthor-<version>-linux-x86_64-portable/
```

containing:

```text
bdmvauthor
bdmvauthor-cli
tsmuxer
mkisofs
dvdauthor
spumux
mplex
licenses/
```

Copy the **entire directory** to the target machine so BDMV Author can continue to find its private sibling helpers automatically.

The portable executables are built against a Debian-12-era ABI floor and are audited to avoid Nix-store paths and RPATH/RUNPATH dependencies. They intentionally use the target distribution's normal shared libraries for desktop integration and security updates.

The target Linux system should provide compatible versions of the normal runtime libraries, including:

- Qt 6.4+ (`Core`, `Gui`, `Widgets`, and `Concurrent`);
- OpenSSL 3;
- Fontconfig and FreeType;
- zlib;
- libxml2;
- libpng;
- FriBidi;
- glibc and libstdc++.

**FFmpeg and ffprobe are not bundled in the generic Linux directory** and must be installed on the target system or configured explicitly in BDMV Author. Standalone x264/x265 are optional.

### Native CMake build on Linux or other supported environments

A normal host-native build can also be made directly with CMake and Ninja after installing the required development libraries for your distribution:

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBDMVAUTHOR_BUILD_GUI=ON \
  -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Qt 6.4+ is required for the GUI. To build only the core and CLI:

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBDMVAUTHOR_BUILD_GUI=OFF
cmake --build build
```

Useful CMake switches include:

```text
BDMVAUTHOR_BUILD_GUI
BDMVAUTHOR_BUILD_BUNDLED_TSMUXER
BDMVAUTHOR_BUILD_BUNDLED_MKISOFS
BDMVAUTHOR_BUILD_BUNDLED_DVDAUTHOR
BDMVAUTHOR_BUILD_BUNDLED_MPLEX
BDMVAUTHOR_WARNINGS_AS_ERRORS
BUILD_TESTING
```

On a plain native Linux build, make sure any DVD helpers that you do not build from the vendored sources are available on `PATH` or configured in the application's external-tool settings.

## Project files and cache

GUI projects are saved as versioned `.bdmvproject` JSON files. A project contains the title list, encoding profiles, stream settings, menu tree, navigation, styles, and paths to referenced media.

When saving a project, BDMV Author stores SHA-256-based fingerprints for referenced media. When reopening it, missing or changed files are detected and the GUI can ask the user to locate replacements rather than silently using the wrong source.

Encoded-media and compliance-analysis caches can be enabled independently. The GUI includes a cache manager for inspecting and deleting entries and configuring automatic cleanup.

## Notes and limitations

- BDMV Author creates disc images; it does not burn optical media itself.
- Blu-ray/UHD menus are native HDMV menus. **BD-J is not implemented.**
- **AACS/encryption is not implemented.**
- Experimental TrueHD + AC-3 authoring is available but should be validated carefully on real players.
- UHD 50/59.94/60 fps modes are experimental and require explicit selection.
- The Debug menu can deliberately disable normal format bitrate/transport ceilings for testing. Output created with those overrides may be noncompliant and is not expected to work on all players.
- Optical-disc player behavior varies. Testing the resulting image with multiple software and hardware players is recommended before committing important material to write-once media.

## License

BDMV Author is licensed under the **Apache License 2.0**. See [`LICENSE`](LICENSE).

Bundled and linked third-party software has its own licensing terms. See [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) and the license files included with the corresponding source trees and binary packages.
