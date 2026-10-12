# Optional libvc1 source

BDMV Author source releases do not include libvc1.

To build experimental **libvc1 VC-1** support, place a separately obtained
**libvc1 0.2.35 or newer** source tree here as:

```text
third_party/libvc1/
  CMakeLists.txt
  VERSION
  LICENSE
  ...
```

Configure/build BDMV Author normally after placing the source. CMake builds the
`vc1enc` command-line frontend as a statically linked sibling executable and
BDMV Author communicates with it as a separate process; BDMV Author does not
link directly to libvc1.

If `third_party/libvc1` is absent, or its version is older than 0.2.35, BDMV Author builds without VC-1 support. Version 0.2.35 is the first frontend with raw VC-1 as the primary output (`-o output.vc1 --format raw`), which BDMV Author requires so no intermediate M2TS is written.
Windows NSIS, normal Nix, and portable-Linux package builds include the compiled
`vc1enc` helper whenever this source directory was supplied and successfully
built.
