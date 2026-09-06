#!/usr/bin/env bash
set -euo pipefail

if ! command -v nix >/dev/null 2>&1; then echo "error: nix was not found" >&2; exit 2; fi
if [[ "$(uname -s)" != Linux ]]; then echo "error: the generic Linux build is supported only on Linux" >&2; exit 2; fi
case "$(uname -m)" in
  x86_64) arch=x86_64 ;;
  aarch64|arm64) arch=aarch64 ;;
  *) echo "error: unsupported Linux architecture: $(uname -m)" >&2; exit 2 ;;
esac
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
version="$(tr -d '\r\n' < "$root/VERSION")"
[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "error: invalid VERSION file: $version" >&2; exit 2; }
cd "$root"
mkdir -p dist
out_path="$({
  nix build .#portable-linux --no-write-lock-file \
    --override-input nixpkgs github:NixOS/nixpkgs/nixos-26.05 \
    --override-input portable-nixpkgs github:NixOS/nixpkgs/nixos-22.11 \
    --print-out-paths --no-link "$@"
} | tail -n 1)"
test -x "$out_path/bin/bdmvauthor" || { echo "error: Nix did not produce bdmvauthor" >&2; exit 1; }
test -x "$out_path/bin/bdmvauthor-cli" || { echo "error: Nix did not produce bdmvauthor-cli" >&2; exit 1; }
test -x "$out_path/bin/tsmuxer" || { echo "error: Nix did not produce bundled tsMuxer" >&2; exit 1; }
test -x "$out_path/bin/mkisofs" || { echo "error: Nix did not produce bundled mkisofs" >&2; exit 1; }
test -x "$out_path/bin/dvdauthor" || { echo "error: Nix did not produce bundled dvdauthor" >&2; exit 1; }
test -x "$out_path/bin/spumux" || { echo "error: Nix did not produce bundled spumux" >&2; exit 1; }
test -x "$out_path/bin/mplex" || { echo "error: Nix did not produce bundled mplex" >&2; exit 1; }

# Resolve GNU strip from the same pinned current-nixpkgs input used by this
# build rather than assuming binutils is installed in the host environment.
# The portable derivation already link-strips its ELF files; this packaging pass
# is intentional defense-in-depth so copied release artifacts remain stripped
# if the derivation's linker flags change later.
echo "==> Resolving release-strip tool"
strip_out="$({
  nix build --no-write-lock-file --inputs-from . nixpkgs#binutils \
    --print-out-paths --no-link
} | tail -n 1)"
strip_tool="$strip_out/bin/strip"
test -x "$strip_tool" || { echo "error: Nix did not provide GNU strip: $strip_tool" >&2; exit 1; }

# Preserve the historical versioned standalone GUI/CLI copies, but also create
# a versioned directory with the exact sibling name `tsmuxer` that BDMV Author
# auto-discovers before PATH.  Copy the whole directory together when moving
# the portable build to another machine.
gui_name="bdmvauthor-${version}-linux-${arch}"
cli_name="bdmvauthor-cli-${version}-linux-${arch}"
bundle_name="bdmvauthor-${version}-linux-${arch}-portable"
bundle_dir="dist/$bundle_name"
cp -L "$out_path/bin/bdmvauthor" "dist/$gui_name"
cp -L "$out_path/bin/bdmvauthor-cli" "dist/$cli_name"
chmod 755 "dist/$gui_name" "dist/$cli_name"
rm -rf "$bundle_dir"
mkdir -p "$bundle_dir"
cp -L "$out_path/bin/bdmvauthor" "$bundle_dir/bdmvauthor"
cp -L "$out_path/bin/bdmvauthor-cli" "$bundle_dir/bdmvauthor-cli"
cp -L "$out_path/bin/tsmuxer" "$bundle_dir/tsmuxer"
cp -L "$out_path/bin/mkisofs" "$bundle_dir/mkisofs"
cp -L "$out_path/bin/dvdauthor" "$bundle_dir/dvdauthor"
cp -L "$out_path/bin/spumux" "$bundle_dir/spumux"
cp -L "$out_path/bin/mplex" "$bundle_dir/mplex"
chmod 755 "$bundle_dir/bdmvauthor" "$bundle_dir/bdmvauthor-cli" "$bundle_dir/tsmuxer" "$bundle_dir/mkisofs" "$bundle_dir/dvdauthor" "$bundle_dir/spumux" "$bundle_dir/mplex"

echo "==> Stripping generic Linux release binaries"
for binary in \
  "dist/$gui_name" \
  "dist/$cli_name" \
  "$bundle_dir/bdmvauthor" \
  "$bundle_dir/bdmvauthor-cli" \
  "$bundle_dir/tsmuxer" \
  "$bundle_dir/mkisofs" \
  "$bundle_dir/dvdauthor" \
  "$bundle_dir/spumux" \
  "$bundle_dir/mplex"; do
  echo "    strip $binary"
  "$strip_tool" --strip-all "$binary"
done

mkdir -p "$bundle_dir/licenses"
if [[ -f "$out_path/share/doc/bdmvauthor/third_party/tsmuxer/LICENSE" ]]; then
  cp -L "$out_path/share/doc/bdmvauthor/third_party/tsmuxer/LICENSE" "$bundle_dir/licenses/tsmuxer-LICENSE"
fi
if [[ -f "$out_path/share/doc/bdmvauthor/third_party/dvdauthor/COPYING" ]]; then
  cp -L "$out_path/share/doc/bdmvauthor/third_party/dvdauthor/COPYING" "$bundle_dir/licenses/dvdauthor-COPYING"
fi
if [[ -f "$out_path/share/doc/bdmvauthor/third_party/mplex/COPYING" ]]; then
  cp -L "$out_path/share/doc/bdmvauthor/third_party/mplex/COPYING" "$bundle_dir/licenses/mplex-COPYING"
fi
cp -L "$out_path/portable-abi.txt" "$bundle_dir/portable-abi.txt"
cp -L "$out_path/gui-needed-libraries.txt" "$bundle_dir/gui-needed-libraries.txt"
cp -L "$out_path/cli-needed-libraries.txt" "$bundle_dir/cli-needed-libraries.txt"
cp -L "$out_path/tsmuxer-needed-libraries.txt" "$bundle_dir/tsmuxer-needed-libraries.txt"
cp -L "$out_path/mkisofs-needed-libraries.txt" "$bundle_dir/mkisofs-needed-libraries.txt"
cp -L "$out_path/dvdauthor-needed-libraries.txt" "$bundle_dir/dvdauthor-needed-libraries.txt"
cp -L "$out_path/spumux-needed-libraries.txt" "$bundle_dir/spumux-needed-libraries.txt"
cp -L "$out_path/mplex-needed-libraries.txt" "$bundle_dir/mplex-needed-libraries.txt"

echo
echo "Generic Linux build complete:"
echo "  dist/$gui_name"
echo "  dist/$cli_name"
echo "  $bundle_dir/"
echo "    bdmvauthor"
echo "    bdmvauthor-cli"
echo "    tsmuxer  (bundled private sibling)"
echo "    mkisofs  (bundled private sibling)"
echo "    dvdauthor (bundled private sibling)"
echo "    spumux    (bundled private sibling)"
echo "    mplex     (bundled private sibling)"
echo "GUI host-provided shared libraries:"; sed 's/^/  /' "$out_path/gui-needed-libraries.txt"
echo "CLI host-provided shared libraries:"; sed 's/^/  /' "$out_path/cli-needed-libraries.txt"
echo "Bundled tsMuxer host-provided shared libraries:"; sed 's/^/  /' "$out_path/tsmuxer-needed-libraries.txt"
echo "Bundled mkisofs host-provided shared libraries:"; sed 's/^/  /' "$out_path/mkisofs-needed-libraries.txt"
echo "Bundled dvdauthor host-provided shared libraries:"; sed 's/^/  /' "$out_path/dvdauthor-needed-libraries.txt"
echo "Bundled spumux host-provided shared libraries:"; sed 's/^/  /' "$out_path/spumux-needed-libraries.txt"
echo "Bundled mplex host-provided shared libraries:"; sed 's/^/  /' "$out_path/mplex-needed-libraries.txt"
echo "ABI/toolchain audit:"; sed 's/^/  /' "$out_path/portable-abi.txt"
echo "FFmpeg/ffprobe and standalone x264/x265 remain runtime-discovered system tools; bundled DVD helpers may be overridden in Settings."
echo "A Settings/CLI tsMuxer override takes precedence over the bundled sibling."
