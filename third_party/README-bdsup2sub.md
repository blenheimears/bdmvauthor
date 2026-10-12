# Optional native subtitle converter

The standalone `bdsup2sub-native` C++ CLI project is **not vendored** in BDMV
Author. To enable automatic builds, put its source files (including
`CMakeLists.txt` and `src/main.cpp`) in `third_party/bdsup2sub/` before running
CMake. The default `BDMVAUTHOR_BUILD_BUNDLED_BDSUP2SUB=ON` detects the source,
compiles the `bdsup2sub` executable, and places it next to BDMV Author's CLI and
GUI for automatic runtime discovery. Installing BDMV Author also installs the
optional helper when built.

Without that folder, normal BDMV Author builds succeed and users can install
`bdsup2sub` separately on `PATH`, configure it in the GUI or use the
`--bdsup2sub` CLI option. No Java runtime or JAR is needed.

Use the separate `bdsup2sub-native-0.1.0-TEST` source archive for this optional
source folder; do not copy the uploaded original Java BDSup2Sub project here.
