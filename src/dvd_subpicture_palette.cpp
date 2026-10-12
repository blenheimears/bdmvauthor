// SPDX-License-Identifier: Apache-2.0
#include "bdmvauthor/dvd_subpicture_palette.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace bdmvauthor::detail {
namespace {

struct PaletteColor {
    std::uint8_t red = 0, green = 0, blue = 0, alpha = 0;
    bool operator==(const PaletteColor&) const = default;
};

// Encoded index: 3 binary RGB components and one of two alpha levels.
// The RGB cube is *title-wide*; the limited local palette is *event-wide*.
PaletteColor candidate(std::size_t i) {
    return {
        static_cast<std::uint8_t>((i & 1U) != 0U ? 255 : 0),
        static_cast<std::uint8_t>((i & 2U) != 0U ? 255 : 0),
        static_cast<std::uint8_t>((i & 4U) != 0U ? 255 : 0),
        static_cast<std::uint8_t>((i & 8U) != 0U ? 255 : 128)
    };
}

std::size_t candidate_index(const std::uint8_t* p) {
    const auto red = static_cast<std::size_t>(p[0] >= 128U);
    const auto green = static_cast<std::size_t>(p[1] >= 128U) << 1U;
    const auto blue = static_cast<std::size_t>(p[2] >= 128U) << 2U;
    const auto alpha = static_cast<std::size_t>(p[3] >= 192U) << 3U;
    return red | green | blue | alpha;
}

unsigned distance(const PaletteColor a, const PaletteColor b) {
    const int dr = static_cast<int>(a.red) - static_cast<int>(b.red);
    const int dg = static_cast<int>(a.green) - static_cast<int>(b.green);
    const int db = static_cast<int>(a.blue) - static_cast<int>(b.blue);
    const int da = static_cast<int>(a.alpha) - static_cast<int>(b.alpha);
    return static_cast<unsigned>(dr*dr + dg*dg + db*db + 2*da*da);
}

} // namespace

std::vector<std::uint8_t> dvd_subpicture_rgba_palette(const std::vector<std::uint8_t>& rgba) {
    if (rgba.size() % 4U != 0U)
        throw std::invalid_argument("DVD subtitle RGBA buffer length is not a multiple of four");

    std::array<std::uint64_t, 16> counts{};
    std::uint64_t colored_pixels = 0;
    std::uint64_t visible_pixels = 0;
    bool has_bright = false, has_dark = false;
    for (std::size_t pos = 0; pos < rgba.size(); pos += 4U) {
        if (rgba[pos + 3U] < 32U) continue;
        ++counts[candidate_index(&rgba[pos])];
        ++visible_pixels;
        const int r = rgba[pos], g = rgba[pos + 1U], b = rgba[pos + 2U];
        const int lo = std::min({r, g, b}), hi = std::max({r, g, b});
        if (hi - lo > 32) ++colored_pixels;
        const int lightness = (r + g + b) / 3;
        if (lightness >= 176) has_bright = true;
        if (lightness <= 80) has_dark = true;
    }

    // Most PGS film captions use white glyphs over a black outline/backdrop.
    // Those captions require a stable DVD SPU palette, *including* the alpha
    // values. The 0.1.200 histogram could choose translucent white instead of
    // opaque white when more anti-aliased pixels than solid pixels were present,
    // making the actual DVD foreground gray. The same happened to dark mats.
    // Normalize neutral white-on-black captions independently of the relative
    // areas of text and outline. Never invert the light and dark roles.
    if (has_bright && has_dark && visible_pixels != 0U &&
        colored_pixels <= visible_pixels / 20U) {
        std::vector<std::uint8_t> output(rgba.size(), 0);
        for (std::size_t pos = 0; pos < rgba.size(); pos += 4U) {
            const auto alpha = rgba[pos + 3U];
            if (alpha < 32U) continue;
            const int brightness = (static_cast<int>(rgba[pos]) +
                static_cast<int>(rgba[pos + 1U]) + static_cast<int>(rgba[pos + 2U])) / 3;
            if (brightness >= 128) {
                // DVD foreground: white at full opacity; very faint antialias
                // coverage is transparent instead of becoming a gray fill.
                if (alpha >= 96U) {
                    output[pos] = output[pos + 1U] = output[pos + 2U] = 255U;
                    output[pos + 3U] = 255U;
                }
            } else {
                // DVD outline/mat: black at full opacity where substantial;
                // half alpha is reserved for the soft outside edge.
                output[pos + 3U] = alpha >= 96U ? 255U : 128U;
            }
        }
        return output;
    }

    // Prefer the foreground and outline over a large background.  FFmpeg's
    // per-image palettegen selected the most common colors instead, so a tiny
    // white foreground could be discarded and rendered as black on DVD.
    std::vector<std::size_t> chosen;
    const auto retain_color = [&](const std::size_t rgb) {
        const auto opaque = rgb | 8U;
        const auto semitransparent = rgb;
        if (counts[opaque] != 0U || counts[semitransparent] != 0U) {
            // Keep the solid foreground and outline even if translucent edge
            // pixels are more numerous. Per-color majority voting was wrong.
            chosen.push_back(opaque);
        }
    };
    retain_color(7U); // white glyphs: preserve even when small
    retain_color(0U); // dark outline/backdrop: preserve even when small

    while (chosen.size() < 3U) {
        std::size_t best = 16U;
        for (std::size_t i = 0; i < counts.size(); ++i) {
            if (counts[i] == 0U || std::find(chosen.begin(), chosen.end(), i) != chosen.end()) continue;
            if (best == 16U || counts[i] > counts[best]) best = i;
        }
        if (best == 16U) break;
        chosen.push_back(best);
    }

    std::vector<std::uint8_t> output(rgba.size(), 0);
    if (chosen.empty()) return output;
    const auto& palettes = chosen;
    for (std::size_t pos = 0; pos < rgba.size(); pos += 4U) {
        if (rgba[pos + 3U] < 32U) continue; // single fully transparent entry
        const auto source = candidate(candidate_index(&rgba[pos]));
        std::size_t best = palettes.front();
        unsigned best_distance = std::numeric_limits<unsigned>::max();
        for (const auto index : palettes) {
            const unsigned d = distance(source, candidate(index));
            if (d < best_distance) {
                best_distance = d;
                best = index;
            }
        }
        const auto color = candidate(best);
        output[pos] = color.red;
        output[pos + 1U] = color.green;
        output[pos + 2U] = color.blue;
        output[pos + 3U] = color.alpha;
    }
    return output;
}

std::string dvd_subpicture_title_palette_rgb() {
    // dvdauthor's RGB palette file needs 16 RGB triplets (one per line).
    // Using the same exact 0/255 RGB cube as the quantizer ensures spumux's
    // Y/Cr/Cb remap finds a matching PGC entry for each bitmap subtitle.
    std::string result;
    result.reserve(16U * 7U);
    static constexpr char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < 16U; ++i) {
        const auto value = (i & 7U);
        const unsigned rgb = ((value & 1U) ? 0xff0000U : 0U) |
                             ((value & 2U) ? 0x00ff00U : 0U) |
                             ((value & 4U) ? 0x0000ffU : 0U);
        for (int shift = 20; shift >= 0; shift -= 4)
            result.push_back(hex[(rgb >> shift) & 15U]);
        result.push_back('\n');
    }
    return result;
}

} // namespace bdmvauthor::detail
