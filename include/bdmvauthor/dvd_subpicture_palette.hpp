// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bdmvauthor::detail {

// Quantize decoded RGBA subtitle pixels to a common DVD-safe RGB color cube.
// Each frame uses at most three visible RGBA combinations plus transparency;
// all frames share at most eight nontransparent RGB values in the DVD CLUT.
// White-on-black captions have a deterministic opaque white foreground and
// black underlay; edge transparency never reduces the core glyph opacity.
// Other captions retain the shared eight-RGB-color quantizer.
std::vector<std::uint8_t> dvd_subpicture_rgba_palette(const std::vector<std::uint8_t>& rgba);

// A deterministic title-wide DVD CLUT for bitmap subtitles. The 8 RGB-cube
// colors match dvd_subpicture_rgba_palette() exactly; 16 rows are required by
// dvdauthor's PGC palette= attribute. The remaining 8 entries duplicate the
// first 8, so they cannot introduce unintended colors. Only use for titles
// containing exclusively bitmap subtitles.
std::string dvd_subpicture_title_palette_rgb();

} // namespace bdmvauthor::detail
