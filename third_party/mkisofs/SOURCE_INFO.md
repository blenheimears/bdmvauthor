# Bundled mkisofs source

BDMV Author vendors a deliberately minimal subset of **cdrtools 3.02a09**,
provided by the user from `cdrtools-3.02a09.tar.gz` (SHA-256
`c7e4f732fb299e9b5d836629dadf5512aa5e6a5624ff438ceb1d056f4dcb07c2`).

Only the `mkisofs` program sources and the small support subset required to
build BDMV Author's private `mkisofs` executable are included. The project does
**not** vendor or build cdrecord, cdda2wav, readcd, scgcheck, smake, or the other
cdrtools programs.

The private build enables the features used by BDMV Author (ISO-9660, Rock
Ridge, Joliet, UDF, El Torito, sorting, duplicate-file coalescing, and
DVD-Audio/DVD-Video layout support). HFS hybrid creation, direct SCSI device
access/multisession drive probing, the Schily find-expression subsystem and NLS
message catalogs are intentionally not linked into this private helper.

The CMake port supplies a compact compatibility layer for the handful of
libschily/libsiconv APIs that mkisofs actually references. It also carries the
upstream cdrtools 3.02a09 `libschily/strlcpy.c` and `libschily/strlcat.c`
fallbacks, compiled only when the host libc does not provide those functions. Selected source and
header files retain their upstream copyright/license headers. `COPYING.GPL2`
and `CDDL.Schily.txt` contain the relevant upstream licenses.

See `BDMVAUTHOR_PORT.md` for the exact feature boundary, copied support files,
and local portability changes.
