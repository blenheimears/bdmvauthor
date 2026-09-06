#!/usr/bin/env bash
set -euo pipefail

# Build and stage a native 64-bit Windows BDMV Author package from an MSYS2
# UCRT64 shell.  The staged directory is designed to run without MSYS2 in PATH.

usage() {
  cat <<'USAGE'
Usage: windows/build-msys2-ucrt64.sh [--ffmpeg-dir DIR]

  --ffmpeg-dir DIR  Strictly use ffmpeg.exe and ffprobe.exe from DIR instead
                    of the MSYS2 FFmpeg package. Both executables must exist
                    and pass a basic self-test; there is no fallback.
USAGE
}

ffmpeg_dir_arg=""
while (($#)); do
  case "$1" in
    --ffmpeg-dir)
      [[ $# -ge 2 ]] || { echo "error: --ffmpeg-dir requires a directory" >&2; exit 2; }
      ffmpeg_dir_arg="$2"
      shift 2
      ;;
    --ffmpeg-dir=*)
      ffmpeg_dir_arg="${1#*=}"
      [[ -n "$ffmpeg_dir_arg" ]] || { echo "error: --ffmpeg-dir requires a directory" >&2; exit 2; }
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "error: unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if [[ "${MSYSTEM:-}" != "UCRT64" ]]; then
  echo "error: run this script from the 'MSYS2 UCRT64' shell (MSYSTEM=UCRT64)" >&2
  exit 2
fi

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
version="$(tr -d '\r\n' < "$root/VERSION")"
[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "error: invalid VERSION file: $version" >&2; exit 2; }
build_dir="${BDMVAUTHOR_BUILD_DIR:-$root/build-msys2-ucrt64}"
dist_dir="${BDMVAUTHOR_DIST_DIR:-$root/dist/bdmvauthor-${version}-windows-ucrt64}"
jobs="${BDMVAUTHOR_JOBS:-${NUMBER_OF_PROCESSORS:-2}}"
installer_out="${BDMVAUTHOR_INSTALLER_OUT:-$root/dist/bdmvauthor-${version}-windows-ucrt64-setup.exe}"
uninstall_files_nsh="$build_dir/bdmvauthor-uninstall-files.nsh"
installer_license="$build_dir/bdmvauthor-installer-license.txt"

ucrt_prefix="${MINGW_PREFIX:-/ucrt64}"
if [[ "$ucrt_prefix" != "/ucrt64" ]]; then
  echo "error: expected the UCRT64 prefix /ucrt64, got: $ucrt_prefix" >&2
  exit 2
fi

# Resolve and validate a custom FFmpeg selection before installing anything.
# This is intentionally a strict override: a bad selection terminates the build
# immediately and can never fall through to the repository FFmpeg executables.
ffmpeg_source_dir="$ucrt_prefix/bin"
ffmpeg_source_description="MSYS2 UCRT64 FFmpeg"
if [[ -n "$ffmpeg_dir_arg" ]]; then
  ffmpeg_dir_posix="$(cygpath -u "$ffmpeg_dir_arg" 2>/dev/null || printf '%s' "$ffmpeg_dir_arg")"
  if [[ ! -d "$ffmpeg_dir_posix" ]]; then
    echo "error: --ffmpeg-dir does not exist or is not a directory: $ffmpeg_dir_arg" >&2
    echo "       The custom FFmpeg override is strict; refusing to fall back to MSYS2 FFmpeg." >&2
    exit 2
  fi
  ffmpeg_source_dir="$(cd "$ffmpeg_dir_posix" && pwd -P)"
  ffmpeg_source_description="custom --ffmpeg-dir '$ffmpeg_source_dir'"
  for exe in ffmpeg.exe ffprobe.exe; do
    if [[ ! -f "$ffmpeg_source_dir/$exe" ]]; then
      echo "error: --ffmpeg-dir is missing $exe: $ffmpeg_source_dir" >&2
      echo "       The custom FFmpeg override is strict; refusing to fall back to MSYS2 FFmpeg." >&2
      exit 2
    fi
  done
  echo "==> Validating custom FFmpeg override"
  if ! PATH="$ffmpeg_source_dir:$PATH" "$ffmpeg_source_dir/ffmpeg.exe" -hide_banner -version >/dev/null 2>&1; then
    echo "error: custom ffmpeg.exe failed its self-test: $ffmpeg_source_dir/ffmpeg.exe" >&2
    echo "       The custom FFmpeg override is strict; refusing to fall back to MSYS2 FFmpeg." >&2
    exit 2
  fi
  if ! PATH="$ffmpeg_source_dir:$PATH" "$ffmpeg_source_dir/ffprobe.exe" -hide_banner -version >/dev/null 2>&1; then
    echo "error: custom ffprobe.exe failed its self-test: $ffmpeg_source_dir/ffprobe.exe" >&2
    echo "       The custom FFmpeg override is strict; refusing to fall back to MSYS2 FFmpeg." >&2
    exit 2
  fi
fi

packages=(
  mingw-w64-ucrt-x86_64-gcc
  mingw-w64-ucrt-x86_64-binutils
  mingw-w64-ucrt-x86_64-cmake
  mingw-w64-ucrt-x86_64-ninja
  mingw-w64-ucrt-x86_64-pkgconf
  mingw-w64-ucrt-x86_64-qt6-base
  mingw-w64-ucrt-x86_64-openssl
  mingw-w64-ucrt-x86_64-zlib
  mingw-w64-ucrt-x86_64-freetype
  mingw-w64-ucrt-x86_64-fontconfig
  mingw-w64-ucrt-x86_64-libxml2
  mingw-w64-ucrt-x86_64-libpng
  mingw-w64-ucrt-x86_64-fribidi
  mingw-w64-ucrt-x86_64-libiconv
  mingw-w64-ucrt-x86_64-nsis
  flex
  bison
)

# The default build uses the repository FFmpeg package. A caller-supplied
# --ffmpeg-dir is a strict override: do not install or silently fall back to
# MSYS2 FFmpeg when it was requested.
if [[ -z "$ffmpeg_dir_arg" ]]; then
  packages+=(mingw-w64-ucrt-x86_64-ffmpeg)
fi

echo "==> Installing/updating required UCRT64 build dependencies"
pacman -S --needed --noconfirm "${packages[@]}"

# Use only UCRT64-native build tools.  This prevents accidental linkage against
# the POSIX/MSYS runtime and keeps the staged program native-Windows-only.
export PATH="$ucrt_prefix/bin:/usr/bin"
export PKG_CONFIG_PATH="$ucrt_prefix/lib/pkgconfig:$ucrt_prefix/share/pkgconfig"


rm -rf "$build_dir" "$dist_dir"
rm -f "$installer_out"
mkdir -p "$build_dir" "$dist_dir" "$(dirname "$installer_out")"

echo "==> Configuring Release build"
cmake -S "$root" -B "$build_dir" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBDMVAUTHOR_BUILD_GUI=ON \
  -DBDMVAUTHOR_BUILD_BUNDLED_TSMUXER=ON \
  -DBDMVAUTHOR_BUILD_BUNDLED_DVDAUTHOR=ON \
  -DBDMVAUTHOR_BUILD_BUNDLED_MKISOFS=ON \
  -DBDMVAUTHOR_BUILD_BUNDLED_MPLEX=ON \
  -DBDMVAUTHOR_WARNINGS_AS_ERRORS=ON \
  -DBUILD_TESTING=ON

echo "==> Building"
cmake --build "$build_dir" --parallel "$jobs"

if [[ ! -f "$build_dir/bdmvauthor.exe" ]]; then
  echo "error: GUI executable was not built; verify that Qt 6 was detected" >&2
  exit 1
fi
if [[ ! -f "$build_dir/bdmvauthor-cli.exe" || ! -f "$build_dir/tsmuxer.exe" ]]; then
  echo "error: expected bdmvauthor-cli.exe and tsmuxer.exe beside the GUI" >&2
  exit 1
fi
if [[ ! -f "$build_dir/dvdauthor.exe" || ! -f "$build_dir/spumux.exe" ]]; then
  echo "error: bundled dvdauthor.exe/spumux.exe were not built" >&2
  exit 1
fi
if [[ ! -f "$build_dir/mkisofs.exe" ]]; then
  echo "error: bundled cdrtools mkisofs.exe was not built by CMake" >&2
  exit 1
fi
if [[ ! -f "$build_dir/mplex.exe" ]]; then
  echo "error: bundled mplex.exe was not built by CMake" >&2
  exit 1
fi
if [[ ! -f "$ffmpeg_source_dir/ffmpeg.exe" || ! -f "$ffmpeg_source_dir/ffprobe.exe" ]]; then
  echo "error: selected FFmpeg source no longer contains ffmpeg.exe/ffprobe.exe: $ffmpeg_source_dir" >&2
  exit 1
fi

echo "==> Running test suite"
ctest --test-dir "$build_dir" --output-on-failure -C Release

cp -f "$build_dir/bdmvauthor.exe" "$dist_dir/"
cp -f "$build_dir/bdmvauthor-cli.exe" "$dist_dir/"
cp -f "$build_dir/tsmuxer.exe" "$dist_dir/"
cp -f "$build_dir/dvdauthor.exe" "$dist_dir/"
cp -f "$build_dir/spumux.exe" "$dist_dir/"
cp -f "$build_dir/mkisofs.exe" "$dist_dir/mkisofs.exe"
cp -f "$build_dir/mplex.exe" "$dist_dir/mplex.exe"
cp -f "$ffmpeg_source_dir/ffmpeg.exe" "$dist_dir/ffmpeg.exe"
cp -f "$ffmpeg_source_dir/ffprobe.exe" "$dist_dir/ffprobe.exe"

# Install licensing/notices beside the runtime.  The main program is Apache-2.0,
# while bundled third-party tools and libraries retain their respective licenses.
mkdir -p "$dist_dir/licenses"
cp -f "$root/LICENSE" "$dist_dir/LICENSE-BDMVAUTHOR.txt"
cp -f "$root/THIRD_PARTY_NOTICES.md" "$dist_dir/THIRD_PARTY_NOTICES.md"
cp -f "$root/third_party/tsmuxer/LICENSE" "$dist_dir/licenses/tsMuxer-LICENSE.txt"
cp -f "$root/third_party/dvdauthor/COPYING" "$dist_dir/licenses/dvdauthor-COPYING.txt"
cp -f "$root/third_party/mplex/COPYING" "$dist_dir/licenses/mplex-COPYING.txt"
cp -f "$root/third_party/mkisofs/COPYING.GPL2" "$dist_dir/licenses/mkisofs-COPYING.GPL2.txt"
cp -f "$root/third_party/mkisofs/CDDL.Schily.txt" "$dist_dir/licenses/mkisofs-CDDL.Schily.txt"
cp -f "$root/third_party/udf25mkiso/LICENSE" "$dist_dir/licenses/udf25mkiso-LICENSE.txt"
cp -f "$root/third_party/udf25mkiso/NOTICE" "$dist_dir/licenses/udf25mkiso-NOTICE.txt"
# FFmpeg's effective license depends on how the selected binary was configured,
# so capture the license statement from the exact ffmpeg.exe being redistributed.
PATH="$ffmpeg_source_dir:$PATH" "$ffmpeg_source_dir/ffmpeg.exe" -L > "$dist_dir/licenses/FFmpeg-LICENSE.txt" 2>&1

# Source for vendored helpers is carried in the BDMV Author source tree. FFmpeg
# source remains upstream/provider-provided and is intentionally not vendored here.
# Do not duplicate source trees inside the runnable Windows package.

# The core uses FreeType+Fontconfig for real menu/button font rendering.  Copy
# a relocatable private Fontconfig configuration and dereference MSYS2's conf.d
# symlinks so the staged tree does not depend on /ucrt64/etc/fonts at runtime.
mkdir -p "$dist_dir/fontconfig/conf.d"
cp -f "$ucrt_prefix/etc/fonts/fonts.conf" "$dist_dir/fontconfig/fonts.conf"
cp -Lf "$ucrt_prefix/etc/fonts/conf.d/"*.conf "$dist_dir/fontconfig/conf.d/"

# windeployqt supplies Qt's runtime DLLs and dynamically loaded plugins such as
# platforms/qwindows.dll.  The recursive import pass below then supplies every
# non-Windows DLL needed by BDMV Author, tsMuxer, Qt itself, and those plugins.
echo "==> Deploying Qt runtime/plugins"
"$ucrt_prefix/bin/windeployqt6.exe" \
  --release \
  --no-translations \
  --dir "$dist_dir" \
  "$dist_dir/bdmvauthor.exe"

# Return a DLL from the custom FFmpeg directory when one was requested. This
# lets alternate dynamic builds bring their own libav*/support DLLs.
find_custom_ffmpeg_dll() {
  [[ -n "$ffmpeg_dir_arg" ]] || return 1
  local wanted_lower="${1,,}" f base
  shopt -s nullglob
  for f in "$ffmpeg_source_dir/"*.dll; do
    base="${f##*/}"
    if [[ "${base,,}" == "$wanted_lower" ]]; then
      printf '%s\n' "$f"
      shopt -u nullglob
      return 0
    fi
  done
  shopt -u nullglob
  return 1
}

is_custom_ffmpeg_component() {
  [[ -n "$ffmpeg_dir_arg" ]] || return 1
  local name="${1##*/}" lower="${1##*/}"
  lower="${lower,,}"
  [[ "$lower" == "ffmpeg.exe" || "$lower" == "ffprobe.exe" ]] && return 0
  [[ "$lower" == *.dll ]] || return 1
  find_custom_ffmpeg_dll "$name" >/dev/null 2>&1
}

# Return the first UCRT64 DLL whose basename matches $1 case-insensitively.
find_ucrt_dll() {
  local wanted="$1" wanted_lower="${1,,}" f base
  shopt -s nullglob
  for f in "$ucrt_prefix/bin/"*.dll; do
    base="${f##*/}"
    if [[ "${base,,}" == "$wanted_lower" ]]; then
      printf '%s\n' "$f"
      shopt -u nullglob
      return 0
    fi
  done
  shopt -u nullglob
  return 1
}

# Windows API-set imports do not correspond to redistributable application DLLs.
is_windows_api_set() {
  local lower="${1,,}"
  [[ "$lower" == api-ms-win-*.dll || "$lower" == ext-ms-win-*.dll ]]
}

# Check Windows' native system directory rather than maintaining a fragile hard-
# coded list.  A short fallback list covers import-library names that may not be
# materialized as ordinary files on every Windows release.
is_windows_system_dll() {
  local dll="$1" lower="${1,,}"
  is_windows_api_set "$dll" && return 0
  # Normal Windows volumes are case-insensitive, so the direct System32 probe
  # is both fast and case-insensitive under MSYS2.
  if [[ -f "/c/Windows/System32/$dll" ]]; then
    return 0
  fi
  case "$lower" in
    kernel32.dll|kernelbase.dll|user32.dll|gdi32.dll|gdi32full.dll|advapi32.dll|shell32.dll|ole32.dll|oleaut32.dll|uuid.dll|comdlg32.dll|comctl32.dll|shlwapi.dll|ws2_32.dll|winmm.dll|version.dll|imm32.dll|setupapi.dll|cfgmgr32.dll|bcrypt.dll|crypt32.dll|secur32.dll|ntdll.dll|rpcrt4.dll|msvcrt.dll|ucrtbase.dll|dwmapi.dll|dwrite.dll|dxgi.dll|opengl32.dll|glu32.dll|winspool.drv|gdiplus.dll) return 0 ;;
  esac
  return 1
}

# Parse PE import tables with the UCRT64 objdump.  This works for EXEs and DLLs,
# including Qt plugins that are loaded dynamically and therefore are not imports
# of bdmvauthor.exe itself.
pe_imports() {
  "$ucrt_prefix/bin/objdump.exe" -p "$1" 2>/dev/null \
    | sed -n 's/^[[:space:]]*DLL Name:[[:space:]]*//p' \
    | tr -d '\r'
}

have_staged_dll() {
  local wanted_lower="${1,,}" f base
  # Runtime dependencies are deliberately staged in the application root.
  # Plugin DLLs themselves remain in Qt's subdirectories.
  while IFS= read -r -d '' f; do
    base="${f##*/}"
    if [[ "${base,,}" == "$wanted_lower" ]]; then
      return 0
    fi
  done < <(find "$dist_dir" -maxdepth 1 -type f -iname '*.dll' -print0)
  return 1
}

if [[ -n "$ffmpeg_dir_arg" ]]; then
  echo "==> Resolving custom FFmpeg DLL dependency closure"
  changed=1
  while (( changed )); do
    changed=0
    while IFS= read -r -d '' pe; do
      is_custom_ffmpeg_component "$pe" || continue
      while IFS= read -r dll; do
        [[ -n "$dll" ]] || continue
        have_staged_dll "$dll" && continue
        if src="$(find_custom_ffmpeg_dll "$dll")"; then
          cp -f "$src" "$dist_dir/${src##*/}"
          echo "    + ${src##*/} (custom FFmpeg)"
          changed=1
          continue
        fi
        if src="$(find_ucrt_dll "$dll")"; then
          cp -f "$src" "$dist_dir/${src##*/}"
          echo "    + ${src##*/} (UCRT runtime for custom FFmpeg)"
          changed=1
          continue
        fi
        if is_windows_system_dll "$dll"; then
          continue
        fi
        echo "error: unresolved DLL '$dll' required by custom FFmpeg component '${pe#$dist_dir/}'" >&2
        echo "       Checked --ffmpeg-dir, $ucrt_prefix/bin, and Windows System32; refusing fallback." >&2
        exit 1
      done < <(pe_imports "$pe")
    done < <(find "$dist_dir" -maxdepth 1 -type f \( -iname 'ffmpeg.exe' -o -iname 'ffprobe.exe' -o -iname '*.dll' \) -print0)
  done
fi

echo "==> Resolving non-Windows DLL dependency closure"
changed=1
while (( changed )); do
  changed=0
  while IFS= read -r -d '' pe; do
    while IFS= read -r dll; do
      [[ -n "$dll" ]] || continue
      have_staged_dll "$dll" && continue
      if src="$(find_ucrt_dll "$dll")"; then
        cp -f "$src" "$dist_dir/${src##*/}"
        echo "    + ${src##*/}"
        changed=1
        continue
      fi
      if is_windows_system_dll "$dll"; then
        continue
      fi
      echo "error: unresolved non-system DLL '$dll' imported by '${pe#$dist_dir/}'" >&2
      echo "       It was not found in $ucrt_prefix/bin or Windows System32." >&2
      exit 1
    done < <(pe_imports "$pe")
  done < <(find "$dist_dir" -type f \( -iname '*.exe' -o -iname '*.dll' \) -print0)
done

# windeployqt can copy a Windows-provided DLL (for example d3dcompiler_47.dll on
# current Windows).  Drop only files that are provably present in System32 and
# are not supplied by the UCRT64 package prefix.
while IFS= read -r -d '' dll_path; do
  name="${dll_path##*/}"
  if ! find_custom_ffmpeg_dll "$name" >/dev/null 2>&1 && ! find_ucrt_dll "$name" >/dev/null 2>&1 && is_windows_system_dll "$name"; then
    rm -f "$dll_path"
    echo "    - $name (provided by Windows)"
  fi
done < <(find "$dist_dir" -maxdepth 1 -type f -iname '*.dll' -print0)

# Strip every staged PE executable and DLL only after dependency discovery is
# complete.  --strip-all removes COFF symbols/debug sections that are not needed
# at runtime; import/export tables and resources remain intact.  This includes
# BDMV Author, bundled helpers, FFmpeg/ffprobe, Qt runtime DLLs/plugins, and the
# recursively staged UCRT64 dependency closure.
echo "==> Stripping staged executables and DLLs"
strip_tool="$ucrt_prefix/bin/strip.exe"
if [[ ! -x "$strip_tool" ]]; then
  echo "error: GNU strip was not found at $strip_tool (mingw-w64-ucrt-x86_64-binutils)" >&2
  exit 1
fi
while IFS= read -r -d '' pe; do
  echo "    strip ${pe#$dist_dir/}"
  "$strip_tool" --strip-all "$pe"
done < <(find "$dist_dir" -type f \( -iname '*.exe' -o -iname '*.dll' \) -print0)

# Final verification happens after stripping so packaging fails if strip ever
# damages a PE image or its import table. Every imported non-Windows DLL must
# still resolve either inside the staged directory or from Windows itself.
echo "==> Verifying stripped runtime dependency closure"
while IFS= read -r -d '' pe; do
  while IFS= read -r dll; do
    [[ -n "$dll" ]] || continue
    if have_staged_dll "$dll" || is_windows_system_dll "$dll"; then
      continue
    fi
    echo "error: packaged runtime is incomplete after stripping: '$dll' required by '${pe#$dist_dir/}'" >&2
    exit 1
  done < <(pe_imports "$pe")
done < <(find "$dist_dir" -type f \( -iname '*.exe' -o -iname '*.dll' \) -print0)

cat > "$dist_dir/README-WINDOWS.txt" <<TXT
BDMV Author $version - Windows UCRT64 build

This directory is self-contained for BDMV Author's own native runtime: the GUI,
CLI, bundled tsMuxer, bundled dvdauthor/spumux, bundled cdrtools mkisofs,
bundled mplex, FFmpeg/ffprobe, Qt plugins, private Fontconfig
configuration, and all required non-Windows DLLs are included. FFmpeg/ffprobe
were staged from: $ffmpeg_source_description. No MSYS2 shell
or MSYS2 PATH is required to launch bdmvauthor.exe.

Standalone x264/x265 remain optional external authoring tools.
Executables placed beside bdmvauthor.exe are detected automatically before the normal
Windows PATH is searched. Explicit paths configured in BDMV Author's External
Tools settings still take precedence.

Source for vendored helper tools and their licensing information is kept with
the BDMV Author source release under third_party/ rather than copied into this
runnable binary directory. FFmpeg source remains upstream/provider-provided and is
not vendored into BDMV Author. Runtime license notices are installed here.
TXT

echo "==> Preparing installer license page"
cat "$root/windows/installer-license-header.txt" "$root/LICENSE" > "$installer_license"

echo "==> Generating NSIS uninstall manifest"
: > "$uninstall_files_nsh"
printf '%s\n' '; Generated from the verified staged runtime. Do not edit.' >> "$uninstall_files_nsh"
while IFS= read -r -d '' installed; do
  rel="${installed#"$dist_dir"/}"
  if [[ "$rel" == *'$'* || "$rel" == *'"'* || "$rel" == *$'\r'* || "$rel" == *$'\n'* ]]; then
    echo "error: staged path cannot be represented safely in NSIS uninstall manifest: $rel" >&2
    exit 1
  fi
  rel_win="${rel//\//\\}"
  printf '  ClearErrors\n  Delete "$INSTDIR\\%s"\n  ${If} ${Errors}\n    StrCpy $0 1\n  ${EndIf}\n' "$rel_win" >> "$uninstall_files_nsh"
done < <(find "$dist_dir" -type f -print0 | sort -z)
# Remove only subdirectories that are empty after installed files are deleted.
# User-added files/directories are preserved; unlike RMDir /r, this cannot erase
# an unrelated custom destination. Uninstall.exe and the root are handled by the
# uninstaller only after every staged file deletion succeeds.
while IFS= read -r -d '' installed_dir; do
  rel="${installed_dir#"$dist_dir"/}"
  [[ "$rel" != "$installed_dir" ]] || continue
  rel_win="${rel//\//\\}"
  printf '  RMDir "$INSTDIR\\%s"\n' "$rel_win" >> "$uninstall_files_nsh"
done < <(find "$dist_dir" -depth -mindepth 1 -type d -print0)
echo "==> Building NSIS installer"
# makensis.exe is a native Windows program.  MSYS2 normally rewrites arguments
# beginning with / as POSIX paths, which turns /DVERSION=... into a bogus
# C:/msys64/DVERSION=... script path.  Every actual filesystem path below has
# already been converted explicitly with cygpath -w, so suppress MSYS argument
# conversion for this native invocation.
MSYS2_ARG_CONV_EXCL='*' "$ucrt_prefix/bin/makensis.exe" \
  "/DVERSION=$version" \
  "/DVERSION4=${version}.0" \
  "/DSTAGE_DIR=$(cygpath -w "$dist_dir")" \
  "/DSOURCE_DIR=$(cygpath -w "$root")" \
  "/DOUT_FILE=$(cygpath -w "$installer_out")" \
  "/DUNINSTALL_FILES_NSH=$(cygpath -w "$uninstall_files_nsh")" \
  "/DINSTALLER_LICENSE=$(cygpath -w "$installer_license")" \
  "$(cygpath -w "$root/windows/installer.nsi")"

if [[ ! -f "$installer_out" ]]; then
  echo "error: NSIS completed without creating the installer: $installer_out" >&2
  exit 1
fi

echo
echo "Windows package ready:"
echo "  $(cygpath -w "$dist_dir")"
echo "Windows installer ready:"
echo "  $(cygpath -w "$installer_out")"
echo "Run the installer outside the MSYS2 shell to test installation/uninstallation."
