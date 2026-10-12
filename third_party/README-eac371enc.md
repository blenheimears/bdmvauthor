# Optional eac371enc 7.1 AC-3/E-AC-3 helper

BDMV Author does **not** bundle eac371enc. A builder who wants 7.1 AC-3 output
places an eac371enc source tree at:

    third_party/eac371enc/

The source tree must provide the `eac371enc` CMake target and support
`EAC371_ENABLE_SNDFILE_INPUT=OFF`. BDMV Author forces that option OFF: FFmpeg
decodes the selected source track to canonical 48 kHz 7.1 `f32le` PCM and pipes
it directly to eac371enc, so libsndfile is not a BDMV Author dependency.

When the directory is absent, BDMV Author builds normally and preserves its
existing <=5.1 FFmpeg AC-3 behavior. When it is present, Blu-ray and UHD AC-3
may resolve to 7.1. Automatic channel selection keeps an 8-channel source at
7.1; explicit 7.1 also uses eac371enc. Explicit/automatic outputs of 5.1 or less
continue to use FFmpeg AC-3. DVD-Video and AVCHD never use eac371enc: their
Dolby Digital output is plain AC-3 only and is limited to at most 5.1 channels.

For 7.1, BDMV Author's AC-3 bitrate control is the **total** bitrate of the
5.1 compatibility core plus dependent E-AC-3 extension. eac371enc chooses the
internal split. For <=5.1 the same control remains the ordinary AC-3 bitrate.

The helper is currently local-only in the encoding scheduler. A worker pinned
to a remote server is therefore not compatible with a 7.1 AC-3 job; normal
worker fallback may use Local when configured.
