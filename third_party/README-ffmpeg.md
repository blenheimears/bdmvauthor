# Optional builder-supplied FFmpeg source

BDMV Author source releases do not redistribute FFmpeg. For a self-contained
macOS `.app`/`.dmg`, place an FFmpeg source tree here so this path exists:

    third_party/ffmpeg/configure

Then run the normal Darwin build, for example:

    nix build .#dmg

This private source tree is **only used by the macOS app/DMG build**. Linux
builds ignore third_party/ffmpeg even when the directory is present; normal
Linux/NixOS builds continue to use `pkgs.ffmpeg` from the runtime tool path.
The CMake option is forced off on non-macOS platforms so the private copy can
never become a Linux build requirement.

On macOS, CMake automatically builds that source tree out of tree and installs
its `ffmpeg` and `ffprobe` executables beside BDMV Author. The `.app` packager
prefers those private siblings over the Nix-provided FFmpeg fallback.
The FFmpeg configure step inherits the exact compiler and binary-tool paths selected by the parent CMake/Nix build, so Darwin builds use the Nix Clang wrapper rather than FFmpeg's default `gcc`.

The bundled configuration deliberately keeps all decoders enabled and leaves FFmpeg's demuxer,
parser, protocol, filter, and bitstream-filter sets at their normal defaults.
Only encoders are restricted. The required encoder set is:

- `rawvideo`, `png`, `ffvhuff`
- `libx264`, `libx265`, `mpeg2video`
- `ac3`, `dca`, `truehd`
- `pcm_s16le`, `pcm_s24le`, `pcm_s16be`, `pcm_s24be`, `pcm_s32le`
- `srt`

When exposed by the supplied FFmpeg source on macOS,
`h264_videotoolbox` and `hevc_videotoolbox` are enabled as well.

The Nix build supplies x264/x265 development files and links the corresponding
FFmpeg encoders. Because enabling libx264/libx265 requires FFmpeg's GPL build
mode, builders/distributors must comply with the licenses of the FFmpeg source
and all enabled dependencies they choose to distribute.
