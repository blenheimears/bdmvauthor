#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 8 ]]; then
  echo "usage: $0 <ffmpeg-source> <build-dir> <install-prefix> <cc> <cxx> <ar> <ranlib> <strip>" >&2
  exit 2
fi

src="$1"
build="$2"
prefix="$3"
cc="$4"
cxx="$5"
ar="$6"
ranlib="$7"
strip="$8"
configure="$src/configure"

if [[ ! -f "$configure" ]]; then
  echo "error: FFmpeg configure script not found: $configure" >&2
  exit 1
fi

for tool_var in cc cxx ar ranlib strip; do
  tool="${!tool_var}"
  if [[ -z "$tool" || ! -x "$tool" ]]; then
    echo "error: FFmpeg build tool '$tool_var' is not executable: $tool" >&2
    exit 1
  fi
done

mkdir -p "$build" "$prefix"

# BDMV Author intentionally keeps FFmpeg's decoder side broad: do not pass
# --disable-decoders, --disable-demuxers, --disable-parsers, or
# --disable-protocols. Only the encoder side is pruned.
required_encoders=(
  rawvideo
  png
  ffvhuff
  mpeg2video
  ac3
  dca
  truehd
  pcm_s16le
  pcm_s24le
  pcm_s16be
  pcm_s24be
  pcm_s32le
  srt
  libx264
  libx265
)

# These are useful BDMV Author hardware providers on macOS but are not required
# for software-only operation. Enable them whenever the supplied FFmpeg tree
# exposes them.
optional_encoders=(
  h264_videotoolbox
  hevc_videotoolbox
)

# FFmpeg prints --list-encoders in terminal-width-dependent columns (FFmpeg
# 9.0.x commonly emits three names per line). Normalize that presentation to
# one encoder token per line before doing exact-name capability checks.
available_encoders="$(bash "$configure" --list-encoders | awk '{ for (i = 1; i <= NF; ++i) print $i }')"
encoder_args=()
for encoder in "${required_encoders[@]}"; do
  if ! grep -Fxq "$encoder" <<<"$available_encoders"; then
    echo "error: supplied FFmpeg source does not provide required encoder: $encoder" >&2
    exit 1
  fi
  encoder_args+=("--enable-encoder=$encoder")
done
for encoder in "${optional_encoders[@]}"; do
  if grep -Fxq "$encoder" <<<"$available_encoders"; then
    encoder_args+=("--enable-encoder=$encoder")
  fi
done

cd "$build"

configure_args=(
  "--prefix=$prefix"
  "--cc=$cc"
  "--cxx=$cxx"
  "--ar=$ar"
  "--ranlib=$ranlib"
  "--strip=$strip"
  --disable-shared
  --enable-static
  --disable-doc
  --disable-debug
  --disable-ffplay
  --disable-encoders
  --enable-gpl
  --enable-libx264
  --enable-libx265
)

if [[ "$(uname -s)" == "Darwin" ]]; then
  configure_args+=(--enable-videotoolbox)
fi

# The Nix derivation supplies x264/x265 and pkg-config. FFmpeg's normal
# autodetection remains enabled so every native decoder/demuxer/parser and any
# decoder dependency already available in the pinned build environment stays
# available. Explicit tool paths are inherited from the parent CMake/Nix build
# so FFmpeg never falls back to an unavailable host gcc on Darwin.
bash "$configure" "${configure_args[@]}" "${encoder_args[@]}"
