# BDMV Author mplex source subset

BDMV Author 0.1.108 carries only the source required to build `mplex`, taken
from the user-supplied mjpegtools SVN r3517 snapshot
`mjpeg-Code-r3517-trunk-mjpeg_play.zip` (snapshot files dated 2025-01-29).
The supplied ZIP has SHA-256:

`18cf570556b34112dbc69a541dea24894c2e614897111ea7094ede1b4f9c0fc4`

The complete mjpegtools snapshot is intentionally **not** vendored.  This
directory retains the upstream `mplex` C++ source/header files plus only the
small GPL utility source/header subset that is directly required to compile
and link `mplex`.  Source for unrelated mjpegtools programs such as lavtools,
mpeg2enc, yuv tools, audio encoders, scripts, and their build files is omitted.

BDMV Author adds `CMakeLists.txt` and `config.h.cmake` as build glue for this
subset. The upstream mplex and retained utility files are otherwise preserved
from the supplied r3517 snapshot.

The retained mplex/support source is GPL version 2 or later / GPL version 2 as
stated by the individual source headers. `COPYING` is the upstream mplex GPLv2
license text.
