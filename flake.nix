{
  description = "BDMV Author - Java-free Blu-ray/UHD/DVD-Video authoring";

  # Match the mature Discus portable-Linux build policy: current stable tools
  # for normal NixOS builds and compilation, with a NixOS 22.11 Qt/glibc/GCC
  # ABI baseline for the host-integrated portable ELF.
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";
  inputs.portable-nixpkgs.url = "github:NixOS/nixpkgs/nixos-22.11";

  outputs = { self, nixpkgs, portable-nixpkgs }:
    let
      version = builtins.replaceStrings [ "\n" "\r" ] [ "" "" ] (builtins.readFile ./VERSION);
      systems = [ "x86_64-linux" "aarch64-linux" ];

      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f system);

      mkBdmvAuthor = system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
          lib = pkgs.lib;
          qt = pkgs.qt6;
          runtimeTools = [ pkgs.ffmpeg pkgs.x264 pkgs.x265 pkgs.dvdauthor pkgs.mjpegtools ];
        in pkgs.stdenv.mkDerivation {
          pname = "bdmvauthor";
          inherit version;
          src = self;

          nativeBuildInputs = [ pkgs.cmake pkgs.ninja pkgs.pkg-config pkgs.makeWrapper qt.wrapQtAppsHook ];
          buildInputs = [ qt.qtbase pkgs.freetype pkgs.fontconfig pkgs.openssl ];
          cmakeFlags = [
            "-DBDMVAUTHOR_BUILD_GUI=ON"
            "-DBDMVAUTHOR_BUILD_BUNDLED_TSMUXER=ON"
            "-DBDMVAUTHOR_BUILD_BUNDLED_MKISOFS=ON"
            "-DBDMVAUTHOR_WARNINGS_AS_ERRORS=ON"
            "-DBUILD_TESTING=ON"
          ];
          nativeCheckInputs = [ pkgs.libbluray ];
          doCheck = true;
          checkPhase = ''
            runHook preCheck
            export LD_LIBRARY_PATH=${lib.makeLibraryPath [ pkgs.libbluray ]}:"''${LD_LIBRARY_PATH:-}"
            ctest --output-on-failure
            runHook postCheck
          '';

          # Normal NixOS installs still make a complete toolchain immediately
          # available on PATH.  The GUI's runtime resolver sees these like any
          # other system-installed tools, and users may override them in
          # Settings.  x265 is included alongside x264 so either provider can
          # be selected when FFmpeg also has libx264/libx265.
          qtWrapperArgs = [ "--prefix PATH : ${lib.makeBinPath runtimeTools}" ];
          postInstall = ''
            test -x "$out/bin/bdmvauthor" || { echo "error: bdmvauthor GUI was not installed" >&2; exit 1; }
            test -x "$out/bin/tsmuxer" || { echo "error: bundled tsMuxer was not installed" >&2; exit 1; }
            test -x "$out/bin/mkisofs" || { echo "error: bundled cdrtools mkisofs was not installed" >&2; exit 1; }
            wrapProgram "$out/bin/bdmvauthor-cli" --prefix PATH : ${lib.makeBinPath runtimeTools}
          '';
          passthru = {
            tsmuxerBundled = true;
            tsmuxerSourceCommit = "c6b1186209e42c877052e762c9185f3226ef8ea2";
            mkisofsBundled = true;
            mkisofsSourceVersion = "cdrtools-3.02a09";
          };
          meta = {
            description = "Qt Blu-ray/UHD/DVD-Video authoring application with Java-free native menus";
            license = [ lib.licenses.asl20 lib.licenses.cddl lib.licenses.gpl2Only ];
            platforms = systems;
            mainProgram = "bdmvauthor";
          };
        };

      mkPortableLinux = system:
        let
          buildPkgs = nixpkgs.legacyPackages.${system};
          abiPkgs = portable-nixpkgs.legacyPackages.${system};
          # As in Discus, current GCC/binutils compile the source, but the final
          # link uses the 22.11 GCC 11/glibc/libstdc++ toolchain. This preserves
          # a Debian-12-era runtime ABI while still compiling modern C++20 code.
          portableCompileCC = buildPkgs.gcc16Stdenv.cc;
          oldCxx = abiPkgs.stdenv.cc;
          oldGcc = abiPkgs.stdenv.cc.cc;
          oldTarget = abiPkgs.stdenv.hostPlatform.config;
          genericInterpreter =
            if system == "x86_64-linux" then "/lib64/ld-linux-x86-64.so.2"
            else if system == "aarch64-linux" then "/lib/ld-linux-aarch64.so.1"
            else throw "unsupported portable Linux architecture: ${system}";
        in abiPkgs.stdenv.mkDerivation {
          pname = "bdmvauthor-portable-linux";
          inherit version;
          src = self;

          # Do not use wrapQtAppsHook: this target must be a conventional FHS
          # ELF using the host Qt/font/OpenSSL stack and normal loader cache.
          nativeBuildInputs = [ buildPkgs.pkg-config buildPkgs.python3 buildPkgs.binutils buildPkgs.patchelf buildPkgs.cmake buildPkgs.ninja buildPkgs.flex buildPkgs.bison ];
          buildInputs = [ abiPkgs.qt6.qtbase abiPkgs.openssl abiPkgs.fontconfig abiPkgs.freetype abiPkgs.zlib abiPkgs.libxml2 abiPkgs.libpng abiPkgs.fribidi ];
          dontConfigure = true;
          dontFixup = true;
          dontWrapQtApps = true;
          NIX_DONT_SET_RPATH = "1";
          NIX_NO_SELF_RPATH = "1";

          buildPhase = ''
            runHook preBuild
            qt_version="$(pkg-config --modversion Qt6Core)"
            case "$qt_version" in
              6.4.*) ;;
              *) echo "error: portable Qt ABI baseline changed: expected Qt 6.4.x, got $qt_version" >&2; exit 1 ;;
            esac

            version="$(tr -d '\r\n' < VERSION)"
            case "$version" in
              [0-9]*.[0-9]*.[0-9]*) ;;
              *) echo "error: invalid VERSION file: $version" >&2; exit 1 ;;
            esac
            old_cxx_root="${oldGcc}/include/c++/${oldGcc.version}"
            prefix_flags="-ffile-prefix-map=/nix/store=/usr/src/nix-store -fmacro-prefix-map=/nix/store=/usr/src/nix-store -fdebug-prefix-map=/nix/store=/usr/src/nix-store -fno-record-gcc-switches -g0"
            common="-Iinclude -Ithird_party/udf25mkiso -nostdinc++ -isystem $old_cxx_root -isystem $old_cxx_root/${oldTarget} -isystem $old_cxx_root/backward $prefix_flags -O2 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wabi=16 -fabi-version=16 -fabi-compat-version=16 -std=c++20 -fPIC -DBDMVAUTHOR_VERSION=\"$version\""
            # pkg-config emits dependency include directories with -I.  GCC
            # then diagnoses warnings inside the old Qt 6.4 headers under our
            # -Wconversion policy.  Treat third-party headers as system headers
            # while retaining all non-include pkg-config flags.
            system_pkg_cflags() {
              local result="" flag
              for flag in $(pkg-config --cflags-only-I "$@"); do
                result="$result -isystem ''${flag#-I}"
              done
              printf '%s %s' "$result" "$(pkg-config --cflags-only-other "$@")"
            }
            dep_cflags="$(system_pkg_cflags openssl fontconfig freetype2)"
            qt_cflags="$(system_pkg_cflags Qt6Widgets Qt6Gui Qt6Core Qt6Concurrent)"

            core_sources="src/author.cpp src/compliance.cpp src/audio_tools.cpp src/progress.cpp src/media_fingerprint.cpp src/hdmv.cpp src/font_renderer.cpp third_party/udf25mkiso/udf_writer.cpp"
            core_objects=""
            n=0
            for src in $core_sources; do
              obj="core-$n.o"; n=$((n+1))
              ${portableCompileCC}/bin/c++ $common $dep_cflags -DBDMVAUTHOR_HAVE_FONTCONFIG=1 -DBDMVAUTHOR_HAVE_FREETYPE_FONTCONFIG=1 -c "$src" -o "$obj"
              core_objects="$core_objects $obj"
            done
            ${portableCompileCC}/bin/c++ $common $dep_cflags $qt_cflags -c src/project_file.cpp -o project_file.o
            ${portableCompileCC}/bin/c++ $common $dep_cflags $qt_cflags -c src/gui.cpp -o gui.o
            ${portableCompileCC}/bin/c++ $common $dep_cflags -c src/cli.cpp -o cli.o

            # GCC's C++ driver defines _GNU_SOURCE, and current glibc maps
            # std::stoi/stol/stoul-family implementations onto GLIBC_2.38's
            # __isoc23_* entry points.  Application integer parsing therefore
            # uses std::from_chars. Keep this object-boundary audit as a hard
            # backstop against accidental reintroduction or header drift.
            if nm -u $core_objects gui.o project_file.o cli.o | grep -q '__isoc23_'; then
              echo "error: portable object code references post-Debian-12 __isoc23_* symbols" >&2
              nm -u $core_objects gui.o project_file.o cli.o | grep '__isoc23_' >&2 || true
              exit 1
            fi

            # Build the vendored tsMuxer CLI with the same 22.11 ABI/link
            # baseline used for the final BDMV Author link.  Unlike FFmpeg and
            # x264/x265, tsMuxer is part of this source tree and is shipped as a
            # private sibling executable in the portable
            # package.  Keeping it dynamic against the baseline zlib,
            # Fontconfig, FreeType, libstdc++, and glibc lets patched host
            # libraries provide security fixes while retaining the Debian 12
            # ABI ceiling.
            cmake -S third_party/tsmuxer -B tsmuxer-portable-build -G Ninja \
              -DCMAKE_BUILD_TYPE=Release \
              -DCMAKE_CXX_COMPILER=${oldCxx}/bin/c++ \
              -DCMAKE_SKIP_RPATH=ON \
              -DCMAKE_BUILD_WITH_INSTALL_RPATH=OFF \
              -DTSMUXER_GUI=OFF \
              -DTSMUXER_STATIC_BUILD=OFF \
              -DTSMUXER_VERSION_OVERRIDE=git-c6b1186-bdmvauthor \
              -DCMAKE_CXX_FLAGS="$prefix_flags -g0 -fno-record-gcc-switches" \
              -DCMAKE_EXE_LINKER_FLAGS="-Wl,--as-needed -Wl,--dynamic-linker=${genericInterpreter} -Wl,--strip-all"
            cmake --build tsmuxer-portable-build --target tsmuxer
            cp tsmuxer-portable-build/tsMuxer/tsmuxer tsmuxer-portable
            test -x tsmuxer-portable || { echo "error: vendored portable tsMuxer was not built" >&2; exit 1; }
            if nm -u tsmuxer-portable | grep -q '__isoc23_'; then
              echo "error: portable tsMuxer references post-Debian-12 __isoc23_* symbols" >&2
              nm -u tsmuxer-portable | grep '__isoc23_' >&2 || true
              exit 1
            fi

            # Build the vendored cdrtools 3.02a09 mkisofs subset directly with
            # CMake.  No Schily makefiles/smake and no host mkisofs/genisoimage
            # package participate in the portable build.
            cmake -S third_party/mkisofs -B mkisofs-portable-build -G Ninja \
              -DCMAKE_BUILD_TYPE=Release \
              -DCMAKE_C_COMPILER=${oldCxx}/bin/cc \
              -DCMAKE_SKIP_RPATH=ON \
              -DCMAKE_BUILD_WITH_INSTALL_RPATH=OFF \
              -DBDMVAUTHOR_WARNINGS_AS_ERRORS=ON \
              -DCMAKE_C_FLAGS="$prefix_flags -g0 -fno-record-gcc-switches" \
              -DCMAKE_EXE_LINKER_FLAGS="-Wl,--as-needed -Wl,--dynamic-linker=${genericInterpreter} -Wl,--strip-all"
            cmake --build mkisofs-portable-build --target bdmvauthor_mkisofs
            cp mkisofs-portable-build/mkisofs mkisofs-portable
            patchelf --remove-rpath mkisofs-portable
            patchelf --set-interpreter ${genericInterpreter} mkisofs-portable
            strip --strip-all mkisofs-portable
            test -x mkisofs-portable || { echo "error: vendored portable mkisofs was not prepared" >&2; exit 1; }

            # Build the vendored dvdauthor/spumux subset against the same old
            # glibc and library baseline.  These are private DVD-Video helper
            # executables, not libraries linked into BDMV Author itself.
            cmake -S third_party/dvdauthor -B dvdauthor-portable-build -G Ninja \
              -DCMAKE_BUILD_TYPE=Release \
              -DCMAKE_C_COMPILER=${oldCxx}/bin/cc \
              -DCMAKE_SKIP_RPATH=ON \
              -DCMAKE_BUILD_WITH_INSTALL_RPATH=OFF \
              -DBDMVAUTHOR_WARNINGS_AS_ERRORS=ON \
              -DCMAKE_C_FLAGS="$prefix_flags -g0 -fno-record-gcc-switches" \
              -DCMAKE_EXE_LINKER_FLAGS="-Wl,--as-needed -Wl,--dynamic-linker=${genericInterpreter} -Wl,--strip-all"
            cmake --build dvdauthor-portable-build --target dvdauthor spumux
            cp dvdauthor-portable-build/dvdauthor dvdauthor-portable
            cp dvdauthor-portable-build/spumux spumux-portable
            for helper in dvdauthor-portable spumux-portable; do
              patchelf --remove-rpath "$helper"
              patchelf --set-interpreter ${genericInterpreter} "$helper"
              strip --strip-all "$helper"
              test -x "$helper" || { echo "error: vendored portable $helper was not prepared" >&2; exit 1; }
            done

            # Build only the vendored mplex subset retained by BDMV Author.
            cmake -S third_party/mplex -B mplex-portable-build -G Ninja \
              -DCMAKE_BUILD_TYPE=Release \
              -DCMAKE_C_COMPILER=${oldCxx}/bin/cc \
              -DCMAKE_CXX_COMPILER=${oldCxx}/bin/c++ \
              -DCMAKE_SKIP_RPATH=ON \
              -DCMAKE_BUILD_WITH_INSTALL_RPATH=OFF \
              -DCMAKE_C_FLAGS="$prefix_flags -g0 -fno-record-gcc-switches" \
              -DCMAKE_CXX_FLAGS="$prefix_flags -g0 -fno-record-gcc-switches" \
              -DCMAKE_EXE_LINKER_FLAGS="-Wl,--as-needed -Wl,--dynamic-linker=${genericInterpreter} -Wl,--strip-all"
            cmake --build mplex-portable-build --target mplex
            cp mplex-portable-build/mplex mplex-portable
            patchelf --remove-rpath mplex-portable
            patchelf --set-interpreter ${genericInterpreter} mplex-portable
            strip --strip-all mplex-portable
            test -x mplex-portable || { echo "error: vendored portable mplex was not prepared" >&2; exit 1; }

            ${oldCxx}/bin/c++ -o bdmvauthor-portable gui.o project_file.o $core_objects \
              $(pkg-config --libs Qt6Widgets Qt6Gui Qt6Core Qt6Concurrent openssl fontconfig freetype2) \
              -ldl -pthread -Wl,--as-needed -Wl,--dynamic-linker=${genericInterpreter} -Wl,--strip-all
            ${oldCxx}/bin/c++ -o bdmvauthor-cli-portable cli.o $core_objects \
              $(pkg-config --libs openssl fontconfig freetype2) \
              -ldl -pthread -Wl,--as-needed -Wl,--dynamic-linker=${genericInterpreter} -Wl,--strip-all

            cmake -DSOURCE_DIR="$PWD" -P tests/check_portable_linux.cmake
            runHook postBuild
          '';

          installPhase = ''
            runHook preInstall
            install -Dm755 bdmvauthor-portable "$out/bin/bdmvauthor"
            install -Dm755 bdmvauthor-cli-portable "$out/bin/bdmvauthor-cli"
            install -Dm755 tsmuxer-portable "$out/bin/tsmuxer"
            install -Dm755 mkisofs-portable "$out/bin/mkisofs"
            install -Dm755 dvdauthor-portable "$out/bin/dvdauthor"
            install -Dm755 spumux-portable "$out/bin/spumux"
            install -Dm755 mplex-portable "$out/bin/mplex"
            install -Dm644 third_party/tsmuxer/LICENSE "$out/share/doc/bdmvauthor/third_party/tsmuxer/LICENSE"
            install -Dm644 third_party/dvdauthor/COPYING "$out/share/doc/bdmvauthor/third_party/dvdauthor/COPYING"
            install -Dm644 third_party/mplex/COPYING "$out/share/doc/bdmvauthor/third_party/mplex/COPYING"
            install -Dm644 third_party/tsmuxer/BDMVAUTHOR-VENDORING.md "$out/share/doc/bdmvauthor/third_party/tsmuxer/BDMVAUTHOR-VENDORING.md"
            mkdir -p "$out/share/doc/bdmvauthor/third_party"
            cp -a third_party/mkisofs "$out/share/doc/bdmvauthor/third_party/mkisofs"
            install -Dm644 resources/bdmvauthor-icon.svg "$out/share/icons/hicolor/scalable/apps/bdmvauthor.svg"
            install -Dm644 data/bdmvauthor.desktop "$out/share/applications/bdmvauthor.desktop"

            audit_elf() {
              local binary="$1"
              local interp
              interp="$(readelf -l "$binary" | sed -n 's/.*Requesting program interpreter: \(.*\)]/\1/p')"
              test "$interp" = "${genericInterpreter}" || { echo "error: unexpected portable ELF interpreter: $interp" >&2; exit 1; }
              if readelf -d "$binary" | grep -Eq '\((RPATH|RUNPATH)\)'; then
                echo "error: portable ELF unexpectedly contains RPATH/RUNPATH: $binary" >&2; readelf -d "$binary" >&2; exit 1
              fi
              if grep -a -q '/nix/store/' "$binary"; then
                echo "error: portable binary still contains /nix/store references: $binary" >&2
                strings -a "$binary" | grep '/nix/store/' | sort -u | head -n 50 >&2 || true
                exit 1
              fi
              python3 - "$binary" <<'PY_ABI'
import re, subprocess, sys
binary=sys.argv[1]
text=subprocess.check_output(["readelf","--version-info",binary],text=True)
limits={"GLIBC":(2,36),"GLIBCXX":(3,4,30),"CXXABI":(1,3,13)}
viol=[]
for family,limit in limits.items():
    pat=re.compile(r"\b"+re.escape(family)+r"_([0-9]+(?:\.[0-9]+)+)\b")
    for vtext in pat.findall(text):
        v=tuple(map(int,vtext.split('.'))); n=max(len(v),len(limit)); a=v+(0,)*(n-len(v)); b=limit+(0,)*(n-len(limit))
        if a>b: viol.append(f"{family}_{vtext} > {family}_{'.'.join(map(str,limit))}")
if viol:
    print("error: portable ELF exceeds Debian 12 runtime ABI baseline:",file=sys.stderr)
    for x in sorted(set(viol)): print("  "+x,file=sys.stderr)
    sys.exit(1)
PY_ABI
            }
            audit_elf "$out/bin/bdmvauthor"
            audit_elf "$out/bin/bdmvauthor-cli"
            audit_elf "$out/bin/tsmuxer"
            audit_elf "$out/bin/mkisofs"
            audit_elf "$out/bin/dvdauthor"
            audit_elf "$out/bin/spumux"
            audit_elf "$out/bin/mplex"

            readelf -d "$out/bin/bdmvauthor" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p' > "$out/gui-needed-libraries.txt"
            readelf -d "$out/bin/bdmvauthor-cli" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p' > "$out/cli-needed-libraries.txt"
            readelf -d "$out/bin/tsmuxer" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p' > "$out/tsmuxer-needed-libraries.txt"
            readelf -d "$out/bin/mkisofs" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p' > "$out/mkisofs-needed-libraries.txt"
            readelf -d "$out/bin/dvdauthor" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p' > "$out/dvdauthor-needed-libraries.txt"
            readelf -d "$out/bin/spumux" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p' > "$out/spumux-needed-libraries.txt"
            readelf -d "$out/bin/mplex" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p' > "$out/mplex-needed-libraries.txt"
            gui_needed="$(cat "$out/gui-needed-libraries.txt")"
            tsmuxer_needed="$(cat "$out/tsmuxer-needed-libraries.txt")"
            dvdauthor_needed="$(cat "$out/dvdauthor-needed-libraries.txt")"
            spumux_needed="$(cat "$out/spumux-needed-libraries.txt")"
            mplex_needed="$(cat "$out/mplex-needed-libraries.txt")"
            for required in libQt6Widgets.so libQt6Gui.so libQt6Core.so libQt6Concurrent.so libcrypto.so libfontconfig.so libfreetype.so libstdc++.so; do
              printf '%s\n' "$gui_needed" | grep -q "^$required" || { echo "error: expected portable GUI dependency $required is missing" >&2; exit 1; }
            done
            # External encoder/authoring libraries must never become DT_NEEDED
            # entries of the GUI. tsMuxer is shipped as a separate executable,
            # not linked into BDMV Author itself.
            for forbidden in libavcodec.so libavformat.so libx264.so libx265.so; do
              if printf '%s\n' "$gui_needed" | grep -q "^$forbidden"; then echo "error: external encoder unexpectedly linked into portable GUI: $forbidden" >&2; exit 1; fi
            done
            for required in libstdc++.so libz.so libfontconfig.so libfreetype.so; do
              printf '%s\n' "$tsmuxer_needed" | grep -q "^$required" || { echo "error: expected portable tsMuxer dependency $required is missing" >&2; exit 1; }
            done
            for forbidden in libavcodec.so libavformat.so libx264.so libx265.so libQt6; do
              if printf '%s\n' "$tsmuxer_needed" | grep -q "^$forbidden"; then echo "error: unexpected portable tsMuxer dependency: $forbidden" >&2; exit 1; fi
            done
            printf '%s\n' "$dvdauthor_needed" | grep -q '^libxml2.so' || { echo "error: expected portable dvdauthor libxml2 dependency is missing" >&2; exit 1; }
            for required in libxml2.so libpng libfreetype.so libfribidi.so libfontconfig.so; do
              printf '%s\n' "$spumux_needed" | grep -q "^$required" || { echo "error: expected portable spumux dependency $required is missing" >&2; exit 1; }
            done
            printf '%s\n' "$mplex_needed" | grep -q '^libstdc++.so' || { echo "error: expected portable mplex libstdc++ dependency is missing" >&2; exit 1; }
            for helper_needed in "$dvdauthor_needed" "$spumux_needed" "$mplex_needed"; do
              for forbidden in libavcodec.so libavformat.so libx264.so libx265.so libQt6; do
                if printf '%s\n' "$helper_needed" | grep -q "^$forbidden"; then echo "error: external encoder/Qt dependency leaked into portable DVD helper: $forbidden" >&2; exit 1; fi
              done
            done
            {
              printf 'compiler='; ${portableCompileCC}/bin/c++ --version | head -n 1
              printf 'link-toolchain='; ${oldCxx}/bin/c++ --version | head -n 1
              printf 'cxx-abi=fabi-version-16 (GCC 11 baseline)\n'
              printf 'glibc-link-baseline=${abiPkgs.glibc.version}\n'
              printf 'glibc-max=GLIBC_2.36\nlibstdcxx-max=GLIBCXX_3.4.30\ncxxabi-max=CXXABI_1.3.13\n'
              printf 'qt-build-baseline='; pkg-config --modversion Qt6Core
              printf 'openssl-build-baseline='; pkg-config --modversion openssl
              printf 'fontconfig-build-baseline='; pkg-config --modversion fontconfig
              printf 'freetype-build-baseline='; pkg-config --modversion freetype2
              printf 'runtime-search=system-loader-cache (no RPATH/RUNPATH)\n'
              printf 'bundled-tsmuxer=git-c6b1186-bdmvauthor; private sibling executable\n'
              printf 'bundled-mkisofs=cdrtools-3.02a09-minimal; CMake-built private sibling executable\n'
              printf 'bundled-dvdauthor=0.7.2+-bdmvauthor; private sibling executable\n'
              printf 'bundled-spumux=0.7.2+-bdmvauthor; private sibling executable\n'
              printf 'bundled-mplex=mjpegtools-r3517-minimal; private sibling executable\n'
              printf 'external-tools=runtime-discovered; FFmpeg/ffprobe and standalone x264/x265 remain host-provided on portable Linux\n'
            } > "$out/portable-abi.txt"
            runHook postInstall
          '';
        };
    in {
      packages = forAllSystems (system:
        let bdmvauthor = mkBdmvAuthor system; portableLinux = mkPortableLinux system;
        in { default = bdmvauthor; inherit bdmvauthor; "portable-linux" = portableLinux; });

      devShells = forAllSystems (system:
        let pkgs=nixpkgs.legacyPackages.${system}; qt=pkgs.qt6;
        in { default = pkgs.mkShell { packages = [ pkgs.cmake pkgs.ninja pkgs.pkg-config qt.qtbase qt.wrapQtAppsHook pkgs.ffmpeg pkgs.x264 pkgs.x265 pkgs.dvdauthor pkgs.mjpegtools pkgs.openssl pkgs.fontconfig pkgs.freetype ]; }; });

      overlays.default = final: prev: { bdmvauthor = self.packages.${final.system}.bdmvauthor; };
    };
}
