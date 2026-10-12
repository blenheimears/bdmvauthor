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
      systems = [ "x86_64-linux" "aarch64-linux" "x86_64-darwin" "aarch64-darwin" ];

      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f system);

      mkBdmvAuthor = system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
          lib = pkgs.lib;
          qt = pkgs.qt6;
          isDarwin = pkgs.stdenv.isDarwin;
          runtimeTools = [ pkgs.ffmpeg pkgs.x264 pkgs.x265 pkgs.openssh ]
            ++ lib.optionals pkgs.stdenv.isLinux [ pkgs.dvdauthor pkgs.mjpegtools ];
        in pkgs.stdenv.mkDerivation {
          pname = "bdmvauthor";
          inherit version;
          src = self;

          nativeBuildInputs = [ pkgs.cmake pkgs.ninja pkgs.pkg-config pkgs.makeWrapper qt.wrapQtAppsHook ]
            ++ lib.optionals isDarwin [ pkgs.bison pkgs.flex pkgs.gnumake pkgs.nasm ];
          buildInputs = [ qt.qtbase pkgs.freetype pkgs.fontconfig pkgs.openssl ]
            ++ lib.optionals (builtins.pathExists ./third_party/bdsup2sub/CMakeLists.txt) [ pkgs.libpng pkgs.zlib ]
            ++ lib.optionals isDarwin [ pkgs.zlib pkgs.libxml2 pkgs.libpng pkgs.fribidi pkgs.libiconv pkgs.x264 pkgs.x265 ];
          cmakeFlags = [
            "-DBDMVAUTHOR_BUILD_GUI=ON"
            "-DBDMVAUTHOR_BUILD_BUNDLED_TSMUXER=ON"
            "-DBDMVAUTHOR_BUILD_BUNDLED_MKISOFS=ON"
            "-DBDMVAUTHOR_WARNINGS_AS_ERRORS=ON"
            "-DBUILD_TESTING=ON"
          ] ++ lib.optionals (!isDarwin) [
            # The private third_party/ffmpeg build is only for self-contained
            # macOS app packaging. Linux uses pkgs.ffmpeg from runtimeTools.
            "-DBDMVAUTHOR_BUILD_BUNDLED_FFMPEG=OFF"
          ] ++ lib.optionals isDarwin [
            "-DBDMVAUTHOR_BUILD_BUNDLED_DVDAUTHOR=ON"
            "-DBDMVAUTHOR_BUILD_BUNDLED_MPLEX=ON"
            "-DBDMVAUTHOR_BUILD_BUNDLED_FFMPEG=ON"
          ];
          nativeCheckInputs = [ pkgs.libbluray ];
          doCheck = true;
          checkPhase = ''
            runHook preCheck
            ${lib.optionalString isDarwin ''
              export DYLD_LIBRARY_PATH=${lib.makeLibraryPath [ pkgs.libbluray ]}:"''${DYLD_LIBRARY_PATH:-}"
            ''}
            ${lib.optionalString (!isDarwin) ''
              export LD_LIBRARY_PATH=${lib.makeLibraryPath [ pkgs.libbluray ]}:"''${LD_LIBRARY_PATH:-}"
            ''}
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
            ${lib.optionalString isDarwin ''
              test -x "$out/bin/dvdauthor" || { echo "error: bundled dvdauthor was not installed on macOS" >&2; exit 1; }
              test -x "$out/bin/spumux" || { echo "error: bundled spumux was not installed on macOS" >&2; exit 1; }
              test -x "$out/bin/mplex" || { echo "error: bundled mplex was not installed on macOS" >&2; exit 1; }
              if [[ -f third_party/ffmpeg/configure ]]; then
                test -x "$out/bin/ffmpeg" || { echo "error: builder-supplied FFmpeg was present but ffmpeg was not installed" >&2; exit 1; }
                test -x "$out/bin/ffprobe" || { echo "error: builder-supplied FFmpeg was present but ffprobe was not installed" >&2; exit 1; }
              fi
            ''}
            if [[ -f third_party/libhdma/CMakeLists.txt ]]; then
              test -x "$out/bin/hdmaenc" || { echo "error: builder-supplied libhdma was present but hdmaenc was not installed" >&2; exit 1; }
            fi
            if [[ -f third_party/libvc1/CMakeLists.txt ]]; then
              test -x "$out/bin/vc1enc" || { echo "error: builder-supplied libvc1 was present but vc1enc was not installed" >&2; exit 1; }
            fi
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
            hdma_define=""
            if [[ -f third_party/libhdma/CMakeLists.txt ]]; then
              hdma_define="-DBDMVAUTHOR_HAVE_HDMAENC=1"
            fi
            vc1_define=""
            if [[ -f third_party/libvc1/CMakeLists.txt ]]; then
              vc1_define="-DBDMVAUTHOR_HAVE_VC1ENC=1"
            fi
            common="-Iinclude -Ithird_party/udf25mkiso -nostdinc++ -isystem $old_cxx_root -isystem $old_cxx_root/${oldTarget} -isystem $old_cxx_root/backward $prefix_flags -O2 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wabi=16 -fabi-version=16 -fabi-compat-version=16 -std=c++20 -fPIC -DBDMVAUTHOR_VERSION=\"$version\" $vc1_define $hdma_define"
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

            core_sources="src/author.cpp src/dvd_subpicture_palette.cpp src/compliance.cpp src/audio_tools.cpp src/progress.cpp src/media_fingerprint.cpp src/hdmv.cpp src/font_renderer.cpp third_party/udf25mkiso/udf_writer.cpp"
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

            # Experimental VC-1 is process-isolated from BDMV Author. When the
            # builder-supplied source exists, build a statically-linked vc1enc frontend
            # as a private sibling; otherwise continue without VC-1 support.
            if [[ -f third_party/libvc1/CMakeLists.txt ]]; then
              cmake -S third_party/libvc1 -B libvc1-portable-build -G Ninja \
                -DCMAKE_BUILD_TYPE=Release \
                -DCMAKE_C_COMPILER=${oldCxx}/bin/cc \
                -DCMAKE_CXX_COMPILER=${oldCxx}/bin/c++ \
                -DCMAKE_SKIP_RPATH=ON \
                -DCMAKE_BUILD_WITH_INSTALL_RPATH=OFF \
                -DLIBVC1_BUILD_SHARED=OFF \
                -DLIBVC1_BUILD_STATIC=ON \
                -DLIBVC1_BUILD_FRONTEND=ON \
                -DLIBVC1_FRONTEND_LINK_STATIC=ON \
                -DLIBVC1_BUILD_TESTS=OFF \
                -DLIBVC1_ENABLE_MATROSKA=OFF \
                -DLIBVC1_ENABLE_VULKAN=OFF \
                -DCMAKE_CXX_FLAGS="$prefix_flags -g0 -fno-record-gcc-switches" \
                -DCMAKE_EXE_LINKER_FLAGS="-Wl,--as-needed -Wl,--dynamic-linker=${genericInterpreter} -Wl,--strip-all"
              cmake --build libvc1-portable-build --target vc1enc
              cp libvc1-portable-build/vc1enc vc1enc-portable
              patchelf --remove-rpath vc1enc-portable
              patchelf --set-interpreter ${genericInterpreter} vc1enc-portable
              strip --strip-all vc1enc-portable
              test -x vc1enc-portable || { echo "error: bundled experimental vc1enc was not prepared" >&2; exit 1; }
            fi

            # Optional standalone hdmaenc is linked to its own static C library, not to BDMV Author.
            # The raw PCM frontend removes any libsndfile runtime dependency.
            if [[ -f third_party/libhdma/CMakeLists.txt ]]; then
              if [[ ! -f third_party/libhdma/VERSION ]]; then
                echo "error: portable Linux libhdma source must declare VERSION >= 0.0.41" >&2
                exit 1
              fi
              hdma_version="$(tr -d '\r\n' < third_party/libhdma/VERSION)"
              if [[ ! "$hdma_version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
                echo "error: invalid third_party/libhdma/VERSION: $hdma_version" >&2
                exit 1
              fi
              IFS=. read -r hdma_major hdma_minor hdma_patch <<< "$hdma_version"
              if (( hdma_major == 0 && hdma_minor == 0 && hdma_patch < 41 )); then
                echo "error: portable Linux uses the NixOS 22.11 GCC 11 baseline and requires libhdma >= 0.0.41 (GCC11 XLL bounds fix); found $hdma_version" >&2
                exit 1
              fi
              cmake -S third_party/libhdma -B libhdma-portable-build -G Ninja \
                -DCMAKE_BUILD_TYPE=Release \
                -DCMAKE_C_COMPILER=${oldCxx}/bin/cc \
                -DCMAKE_SKIP_RPATH=ON \
                -DCMAKE_BUILD_WITH_INSTALL_RPATH=OFF \
                -DLIBHDMA_TESTS=OFF -DLIBHDMA_ENABLE_SNDFILE_INPUT=OFF \
                -DCMAKE_C_FLAGS="$prefix_flags -g0 -fno-record-gcc-switches" \
                -DCMAKE_EXE_LINKER_FLAGS="-Wl,--as-needed -Wl,--dynamic-linker=${genericInterpreter} -Wl,--strip-all"
              cmake --build libhdma-portable-build --target hdmaenc
              cp libhdma-portable-build/hdmaenc hdmaenc-portable
              patchelf --remove-rpath hdmaenc-portable
              patchelf --set-interpreter ${genericInterpreter} hdmaenc-portable
              strip --strip-all hdmaenc-portable
              test -x hdmaenc-portable || { echo "error: experimental hdmaenc frontend was not prepared" >&2; exit 1; }
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
            if [[ -x hdmaenc-portable ]]; then
              install -Dm755 hdmaenc-portable "$out/bin/hdmaenc"
              install -Dm644 third_party/libhdma/COPYING.LGPL-2.1 "$out/share/doc/bdmvauthor/third_party/libhdma/COPYING.LGPL-2.1"
              install -Dm644 third_party/libhdma/LICENSE-ORIGINS.md "$out/share/doc/bdmvauthor/third_party/libhdma/LICENSE-ORIGINS.md"
            fi
            if [[ -x vc1enc-portable ]]; then
              install -Dm755 vc1enc-portable "$out/bin/vc1enc"
              install -Dm644 third_party/libvc1/LICENSE "$out/share/doc/bdmvauthor/third_party/libvc1/LICENSE"
            fi
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
            if [[ -x "$out/bin/vc1enc" ]]; then audit_elf "$out/bin/vc1enc"; fi
            if [[ -x "$out/bin/hdmaenc" ]]; then audit_elf "$out/bin/hdmaenc"; fi
            audit_elf "$out/bin/tsmuxer"
            audit_elf "$out/bin/mkisofs"
            audit_elf "$out/bin/dvdauthor"
            audit_elf "$out/bin/spumux"
            audit_elf "$out/bin/mplex"

            readelf -d "$out/bin/bdmvauthor" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p' > "$out/gui-needed-libraries.txt"
            readelf -d "$out/bin/bdmvauthor-cli" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p' > "$out/cli-needed-libraries.txt"
            if [[ -x "$out/bin/vc1enc" ]]; then
              readelf -d "$out/bin/vc1enc" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p' > "$out/vc1enc-needed-libraries.txt"
            else
              : > "$out/vc1enc-needed-libraries.txt"
            fi
            if [[ -x "$out/bin/hdmaenc" ]]; then
              readelf -d "$out/bin/hdmaenc" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p' > "$out/hdmaenc-needed-libraries.txt"
            else
              : > "$out/hdmaenc-needed-libraries.txt"
            fi
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
            for forbidden in libavcodec.so libavformat.so libx264.so libx265.so libvc1.so libhdma.so; do
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
              if [[ -x "$out/bin/vc1enc" ]]; then vc1_version=unknown; [[ -f third_party/libvc1/VERSION ]] && vc1_version="$(tr -d '\r\n' < third_party/libvc1/VERSION)"; printf 'bundled-vc1enc=libvc1-%s; experimental private sibling executable\n' "$vc1_version"; else printf 'bundled-vc1enc=not-built; third_party/libvc1 absent\n'; fi
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

      mkMacApp = system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
          lib = pkgs.lib;
          qt = pkgs.qt6;
          base = mkBdmvAuthor system;
          ffmpegBin = lib.getBin pkgs.ffmpeg;
          x264Bin = lib.getBin pkgs.x264;
          x265Bin = lib.getBin pkgs.x265;
          qtBaseLib = lib.getLib qt.qtbase;
          # Nixpkgs exposes Qt's plugin root as a passthru attribute.  Package
          # the runtime plugins directly instead of depending on an external Qt deployment tool,
          # which is absent from the pinned Darwin qttools derivation.
          qtPluginRoot = "${qt.qtbase}/${qt.qtbase.qtPluginPrefix}";
          fontconfigOut = pkgs.fontconfig;
          rsvgBin = lib.getBin pkgs.librsvg;
          arch = if system == "aarch64-darwin" then "arm64" else "x86_64";
        in
          if !pkgs.stdenv.isDarwin then throw "macOS application bundles are only available on Darwin"
          else pkgs.runCommand "bdmvauthor-${version}-macos-app-${arch}" {
            nativeBuildInputs = [
              pkgs.macdylibbundler
              pkgs.darwin.cctools
              rsvgBin
              pkgs.file
              pkgs.coreutils
            ];
          } ''
            set -euo pipefail

            # Keep the working bundle path free of spaces. macdylibbundler
            # invokes otool/install_name_tool internally and does not reliably
            # preserve quoting for input paths containing spaces. Rename only
            # after all Mach-O rewriting is complete.
            app="$out/BDMVAuthor.app"
            final_app="$out/BDMV Author.app"
            macos="$app/Contents/MacOS"
            frameworks="$app/Contents/Frameworks"
            resources="$app/Contents/Resources"
            mkdir -p "$macos" "$frameworks" "$resources"

            # The ordinary Nix package is wrapped for execution from the Nix
            # store. A relocatable .app must contain the underlying Mach-O
            # executable instead of a wrapper that embeds /nix/store paths.
            copy_unwrapped() {
              local source="$1" destination="$2" base_name source_dir wrapped
              base_name="$(basename "$source")"
              source_dir="$(dirname "$source")"
              wrapped="$source_dir/.$base_name-wrapped"
              if [[ -x "$wrapped" ]]; then source="$wrapped"; fi
              cp "$source" "$destination"
              chmod u+w,go+rX "$destination"
            }

            copy_unwrapped "${base}/bin/bdmvauthor" "$macos/bdmvauthor"
            copy_unwrapped "${base}/bin/bdmvauthor-cli" "$macos/bdmvauthor-cli"
            for helper in tsmuxer mkisofs dvdauthor spumux mplex; do
              copy_unwrapped "${base}/bin/$helper" "$macos/$helper"
            done
            for helper in vc1enc hdmaenc; do
              if [[ -x "${base}/bin/$helper" ]]; then
                copy_unwrapped "${base}/bin/$helper" "$macos/$helper"
              fi
            done

            # Prefer a builder-supplied FFmpeg compiled from third_party/ffmpeg.
            # This is the relocatable distribution path on macOS; when no source
            # tree was supplied, retain the Nix FFmpeg fallback for development
            # and compatibility with older build trees. macdylibbundler below
            # still captures any non-system dependencies (notably x264/x265).
            if [[ -x "${base}/bin/ffmpeg" && -x "${base}/bin/ffprobe" ]]; then
              copy_unwrapped "${base}/bin/ffmpeg" "$macos/ffmpeg"
              copy_unwrapped "${base}/bin/ffprobe" "$macos/ffprobe"
            else
              copy_unwrapped "${ffmpegBin}/bin/ffmpeg" "$macos/ffmpeg"
              copy_unwrapped "${ffmpegBin}/bin/ffprobe" "$macos/ffprobe"
            fi
            copy_unwrapped "${x264Bin}/bin/x264" "$macos/x264"
            copy_unwrapped "${x265Bin}/bin/x265" "$macos/x265"

            cat > "$app/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleDevelopmentRegion</key><string>en</string>
  <key>CFBundleDisplayName</key><string>BDMV Author</string>
  <key>CFBundleExecutable</key><string>bdmvauthor</string>
  <key>CFBundleIconFile</key><string>BDMVAuthor.icns</string>
  <key>CFBundleIdentifier</key><string>org.bdmvauthor.app</string>
  <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
  <key>CFBundleName</key><string>BDMV Author</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>${version}</string>
  <key>CFBundleVersion</key><string>${version}</string>
  <key>LSApplicationCategoryType</key><string>public.app-category.video</string>
  <key>LSMinimumSystemVersion</key><string>14.0</string>
  <key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
PLIST

            cat > "$resources/qt.conf" <<'QTCONF'
[Paths]
Plugins = PlugIns
QTCONF

            cp "${self}/VERSION" "$resources/VERSION"
            cp "${self}/LICENSE" "$resources/LICENSE"
            cp "${self}/THIRD_PARTY_NOTICES.md" "$resources/THIRD_PARTY_NOTICES.md"
            if [[ -d "${base}/share/doc/bdmvauthor" ]]; then
              mkdir -p "$resources/licenses"
              cp -R "${base}/share/doc/bdmvauthor/." "$resources/licenses/"
            fi

            # Fontconfig is linked into BDMV Author and spumux.  Its Nix build
            # normally points at store-hosted configuration, which is not
            # suitable for a relocatable Finder application.  Ship the generic
            # matching rules and an app-local top-level configuration that scans
            # the standard macOS system and per-user font directories instead.
            mkdir -p "$resources/fontconfig/conf.d"
            if [[ -d "${fontconfigOut}/etc/fonts/conf.d" ]]; then
              while IFS= read -r -d "" fontconf; do
                cp -L "$fontconf" "$resources/fontconfig/conf.d/"
              done < <(find -L "${fontconfigOut}/etc/fonts/conf.d" -maxdepth 1 -type f -name '*.conf' -print0)
            fi
            cat > "$resources/fontconfig/fonts.conf" <<'FONTCONF'
<?xml version="1.0"?>
<fontconfig>
  <dir>/System/Library/Fonts</dir>
  <dir>/System/Library/Fonts/Supplemental</dir>
  <dir>/Library/Fonts</dir>
  <dir>~/Library/Fonts</dir>
  <include ignore_missing="yes">conf.d</include>
  <cachedir prefix="xdg">fontconfig</cachedir>
  <config><rescan><int>30</int></rescan></config>
</fontconfig>
FONTCONF

            # Build a native Finder icon from the existing approved SVG.
            iconset="$TMPDIR/BDMVAuthor.iconset"
            mkdir -p "$iconset"
            for size in 16 32 128 256 512; do
              doubled=$((size * 2))
              "${rsvgBin}/bin/rsvg-convert" -w "$size" -h "$size" "${self}/resources/bdmvauthor-icon.svg" > "$iconset/icon_''${size}x''${size}.png"
              "${rsvgBin}/bin/rsvg-convert" -w "$doubled" -h "$doubled" "${self}/resources/bdmvauthor-icon.svg" > "$iconset/icon_''${size}x''${size}@2x.png"
            done
            /usr/bin/iconutil -c icns "$iconset" -o "$resources/BDMVAuthor.icns"

            # Qt's macOS deployment documentation requires the Cocoa platform
            # plugin and recommends shipping the image-format, icon-engine,
            # print-support, and style plugin families used by GUI applications.
            # The pinned Nixpkgs qttools build does not contain the Qt deployment utility, so
            # copy those runtime plugins directly from qtbase while preserving
            # their category subdirectories. qt.conf above points Qt at
            # Contents/PlugIns.
            plugins="$app/Contents/PlugIns"
            mkdir -p "$plugins"
            for category in platforms styles imageformats iconengines printsupport; do
              if [[ -d "${qtPluginRoot}/$category" ]]; then
                cp -RL "${qtPluginRoot}/$category" "$plugins/$category"
                chmod -R u+w "$plugins/$category"
              fi
            done
            if [[ ! -f "$plugins/platforms/libqcocoa.dylib" ]]; then
              echo "error: required Qt Cocoa platform plugin was not found at ${qtPluginRoot}/platforms/libqcocoa.dylib" >&2
              exit 1
            fi

            # macdylibbundler 1.0.x (the version in the pinned Nixpkgs) can
            # relocate ordinary dylibs but has no framework-bundling flag.
            # Copy Qt's framework bundles explicitly, normalize every Qt
            # framework load command to the app-local Frameworks directory,
            # then let dylibbundler handle the remaining ordinary dylib closure.
            command -v otool >/dev/null
            command -v install_name_tool >/dev/null

            while IFS= read -r -d "" qtframework; do
              framework_name="$(basename "$qtframework")"
              resolved_framework="$(realpath "$qtframework")"
              mkdir -p "$frameworks/$framework_name"
              # Dereference only the outer Nix path. Preserve the framework's
              # internal Versions/Current, executable, and Resources symlinks;
              # codesign requires the canonical framework bundle structure.
              cp -a "$resolved_framework/." "$frameworks/$framework_name/"
              chmod -R u+w "$frameworks/$framework_name"

            done < <(find -L "${qtBaseLib}/lib" -maxdepth 1 -type d -name 'Qt*.framework' -print0)

            # Normalize any Nix-store framework aliases only after every Qt
            # framework is present, then audit the complete symlink graph.
            "${self}/scripts/macos_normalize_framework_symlinks.sh" "$frameworks"

            rewrite_qt_framework_refs() {
              local candidate="$1" dep framework_prefix framework_name suffix replacement dylib_id
              while IFS= read -r dep; do
                case "$dep" in
                  /nix/store/*.framework/*)
                    framework_prefix="''${dep%%.framework/*}.framework"
                    framework_name="''${framework_prefix##*/}"
                    suffix="''${dep#"$framework_prefix"/}"
                    replacement="@executable_path/../Frameworks/$framework_name/$suffix"
                    install_name_tool -change "$dep" "$replacement" "$candidate"
                    ;;
                  @rpath/Qt*.framework/*)
                    replacement="@executable_path/../Frameworks/''${dep#@rpath/}"
                    install_name_tool -change "$dep" "$replacement" "$candidate"
                    ;;
                esac
              done < <(otool -L "$candidate" | tail -n +2 | awk '{print $1}')

              dylib_id="$(otool -D "$candidate" 2>/dev/null | sed -n '2p' || true)"
              case "$dylib_id" in
                /nix/store/*.framework/*)
                  framework_prefix="''${dylib_id%%.framework/*}.framework"
                  framework_name="''${framework_prefix##*/}"
                  suffix="''${dylib_id#"$framework_prefix"/}"
                  install_name_tool -id "@executable_path/../Frameworks/$framework_name/$suffix" "$candidate"
                  ;;
                @rpath/Qt*.framework/*)
                  install_name_tool -id "@executable_path/../Frameworks/''${dylib_id#@rpath/}" "$candidate"
                  ;;
              esac
            }

            while IFS= read -r -d "" candidate; do
              if file "$candidate" | grep -q 'Mach-O'; then
                rewrite_qt_framework_refs "$candidate"
              fi
            done < <(find "$macos" "$plugins" "$frameworks" -type f -print0)

            bundle_args=(-cd -of -b -ns)
            while IFS= read -r -d "" executable; do
              if file "$executable" | grep -q 'Mach-O'; then
                bundle_args+=(-x "$executable")
              fi
            done < <(find "$macos" "$plugins" "$frameworks" -type f -print0)
            dylibbundler "''${bundle_args[@]}" \
              -d "$frameworks" \
              -p '@executable_path/../Frameworks/' \
              -i /usr/lib -i /System/Library

            # Remove Nix-store rpaths that are no longer needed after bundling.
            # Keep @rpath/@loader_path entries created by Qt and the bundler.
            while IFS= read -r -d "" candidate; do
              if ! file "$candidate" | grep -q 'Mach-O'; then continue; fi
              while IFS= read -r rpath; do
                case "$rpath" in
                  /nix/store/*) install_name_tool -delete_rpath "$rpath" "$candidate" || true ;;
                esac
              done < <(otool -l "$candidate" | awk '/cmd LC_RPATH/{getline; getline; if ($1=="path") print $2}')
            done < <(find "$app/Contents" -type f -print0)

            # A packaged app must not retain dynamic-loader references to the
            # Nix store. Text/debug strings are harmless; LC_LOAD_DYLIB and
            # LC_RPATH entries are not.
            bad=0
            while IFS= read -r -d "" candidate; do
              if ! file "$candidate" | grep -q 'Mach-O'; then continue; fi
              if otool -L "$candidate" | tail -n +2 | grep -q '/nix/store/'; then
                echo "error: unbundled /nix/store dylib dependency in $candidate" >&2
                otool -L "$candidate" >&2
                bad=1
              fi
              if otool -l "$candidate" | awk '/cmd LC_RPATH/{getline; getline; if ($1=="path") print $2}' | grep -q '^/nix/store/'; then
                echo "error: unbundled /nix/store rpath in $candidate" >&2
                bad=1
              fi
            done < <(find "$app/Contents" -type f -print0)
            [[ "$bad" -eq 0 ]]

            # Apple Silicon requires every modified Mach-O object to carry a
            # valid signature. Sign nested code from the inside out, then sign
            # each framework bundle, and finally the outer application bundle.
            # Avoid --deep for signing: it is fragile with frameworks and can
            # obscure malformed nested-code layouts.
            while IFS= read -r -d "" candidate; do
              if file "$candidate" | grep -q 'Mach-O'; then
                /usr/bin/codesign --force --sign - "$candidate"
              fi
            done < <(find "$macos" "$plugins" "$frameworks" -type f -print0)

            while IFS= read -r -d "" framework_bundle; do
              /usr/bin/codesign --force --sign - "$framework_bundle"
            done < <(find "$frameworks" -maxdepth 1 -type d -name 'Qt*.framework' -print0)

            /usr/bin/codesign --force --sign - "$app"
            /usr/bin/codesign --verify --deep --strict "$app"

            test -x "$macos/bdmvauthor"
            test -x "$macos/ffmpeg"
            test -f "$resources/fontconfig/fonts.conf"
            test -d "$resources/fontconfig/conf.d"
            test -d "$app/Contents/PlugIns/platforms"
            test -d "$app/Contents/Frameworks"

            # Present the normal Finder-visible name only after tools that are
            # sensitive to spaces have finished processing the bundle.
            mv "$app" "$final_app"
            /usr/bin/codesign --verify --deep --strict "$final_app"
          '';

      mkMacDmg = system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
          appPackage = mkMacApp system;
          arch = if system == "aarch64-darwin" then "arm64" else "x86_64";
        in
          if !pkgs.stdenv.isDarwin then throw "macOS disk images are only available on Darwin"
          else pkgs.runCommand "bdmvauthor-${version}-macos-dmg-${arch}" {
            nativeBuildInputs = [ pkgs.coreutils ];
          } ''
            set -euo pipefail
            mkdir -p "$out" "$TMPDIR/dmg-root"
            cp -R "${appPackage}/BDMV Author.app" "$TMPDIR/dmg-root/BDMV Author.app"
            ln -s /Applications "$TMPDIR/dmg-root/Applications"
            dmg="$out/BDMV-Author-${version}-${arch}.dmg"
            /usr/bin/hdiutil create \
              -volname "BDMV Author ${version}" \
              -srcfolder "$TMPDIR/dmg-root" \
              -format UDZO -fs HFS+ -ov "$dmg"
            sha256sum "$dmg" > "$out/BDMV-Author-${version}-${arch}.dmg.sha256"
          '';
    in {
      packages = forAllSystems (system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
          bdmvauthor = mkBdmvAuthor system;
          portableLinux = if pkgs.stdenv.isLinux then mkPortableLinux system else null;
        in { default = bdmvauthor; inherit bdmvauthor; }
          // pkgs.lib.optionalAttrs pkgs.stdenv.isLinux { "portable-linux" = portableLinux; }
          // pkgs.lib.optionalAttrs pkgs.stdenv.isDarwin {
            app = mkMacApp system;
            dmg = mkMacDmg system;
            "macos-app" = mkMacApp system;
            "macos-dmg" = mkMacDmg system;
          });

      devShells = forAllSystems (system:
        let pkgs=nixpkgs.legacyPackages.${system}; qt=pkgs.qt6;
        in { default = pkgs.mkShell { packages = [ pkgs.cmake pkgs.ninja pkgs.pkg-config qt.qtbase qt.wrapQtAppsHook pkgs.ffmpeg pkgs.x264 pkgs.x265 pkgs.openssh pkgs.openssl pkgs.fontconfig pkgs.freetype ]
          ++ pkgs.lib.optionals pkgs.stdenv.isLinux [ pkgs.dvdauthor pkgs.mjpegtools pkgs.libpng pkgs.zlib ]
          ++ pkgs.lib.optionals pkgs.stdenv.isDarwin [ pkgs.bison pkgs.flex pkgs.zlib pkgs.libxml2 pkgs.libpng pkgs.fribidi pkgs.libiconv ]; }; });

      overlays.default = final: prev: { bdmvauthor = self.packages.${final.system}.bdmvauthor; };
    };
}
