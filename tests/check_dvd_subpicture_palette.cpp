// SPDX-License-Identifier: Apache-2.0
#include "bdmvauthor/dvd_subpicture_palette.hpp"
#include <array>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <set>
#include <stdexcept>
#include <vector>

using bdmvauthor::detail::dvd_subpicture_rgba_palette;
using Color = std::array<std::uint8_t,4>;

void assert_image(const std::vector<std::uint8_t>& output) {
    std::set<Color> colors;
    for (std::size_t i=0; i < output.size(); i+=4U) {
        const Color pixel{output[i],output[i+1],output[i+2],output[i+3]};
        colors.insert(pixel);
        for (std::size_t c=0;c<3;++c)
            if (pixel[c] != 0U && pixel[c] != 255U)
                throw std::runtime_error("DVD subtitle uses non-shared RGB color");
    }
    if (colors.size() > 4U) throw std::runtime_error("DVD subtitle exceeds four RGBA entries");
}

Color pixel_at(const std::vector<std::uint8_t>& pixels, std::size_t i) {
    return {pixels[i*4],pixels[i*4+1],pixels[i*4+2],pixels[i*4+3]};
}

int main() {
    // Verify the explicit IFO palette agrees with the per-caption RGB cube.
    // Black and white must occupy invariant slots throughout the DVD title.
    {
        std::istringstream input(bdmvauthor::detail::dvd_subpicture_title_palette_rgb());
        std::vector<std::string> rows;
        std::string line;
        while (std::getline(input, line)) rows.push_back(line);
        if (rows.size() != 16U || rows[0] != "000000" || rows[7] != "ffffff" ||
            rows[8] != rows[0] || rows[15] != rows[7])
            throw std::runtime_error("DVD IFO CLUT differs from quantizer RGB cube");
        for (unsigned i = 0; i < 8U; ++i) {
            unsigned rgb = ((i & 1U) ? 0xff0000U : 0U) |
                           ((i & 2U) ? 0x00ff00U : 0U) |
                           ((i & 4U) ? 0x0000ffU : 0U);
            unsigned parsed = std::stoul(rows[i], nullptr, 16);
            if (rgb != parsed || rows[i] != rows[i + 8U])
                throw std::runtime_error("DVD authored palette index mismatch");
        }
    }
    const auto push = [](std::vector<std::uint8_t>& dest, Color color, std::size_t count) {
        for (std::size_t i=0;i<count;++i) dest.insert(dest.end(),color.begin(),color.end());
    };
    // Reproduction: many dark pixels and colorful anti-aliasing pixels, but
    // the small white text must not be dropped by automatic palette selection.
    std::vector<std::uint8_t> white_on_black;
    push(white_on_black, {0,0,0,0}, 9000);
    push(white_on_black, {0,0,0,255}, 1600);
    push(white_on_black, {0,0,0,125}, 700);
    push(white_on_black, {20,40,75,255}, 280);
    push(white_on_black, {255,255,255,255}, 20);
    auto output = dvd_subpicture_rgba_palette(white_on_black);
    assert_image(output);
    if (pixel_at(output, 0) != Color{0,0,0,0}) throw std::runtime_error("transparent background changed");
    if (pixel_at(output, 9000) != Color{0,0,0,255}) throw std::runtime_error("opaque black outline changed");
    if (pixel_at(output, 9000+1600+700+280) != Color{255,255,255,255})
        throw std::runtime_error("white subtitle foreground became black");

    // Real-world regression: antialiasing/soft edges dominate the histogram.
    // Earlier builds selected translucent white and/or translucent black and
    // the foreground appeared gray while the underline/outline appeared pale.
    std::vector<std::uint8_t> alpha_heavy;
    push(alpha_heavy, {0,0,0,0}, 1000);
    push(alpha_heavy, {255,255,255,128}, 500);
    push(alpha_heavy, {255,255,255,255}, 25);
    push(alpha_heavy, {0,0,0,128}, 600);
    push(alpha_heavy, {0,0,0,255}, 15);
    push(alpha_heavy, {160,160,160,170}, 40);
    output = dvd_subpicture_rgba_palette(alpha_heavy);
    assert_image(output);
    if (pixel_at(output, 1000) != Color{255,255,255,255} ||
        pixel_at(output, 1500) != Color{255,255,255,255})
        throw std::runtime_error("semi-transparent white replaced solid DVD foreground");
    if (pixel_at(output, 1525) != Color{0,0,0,255} ||
        pixel_at(output, 2125) != Color{0,0,0,255})
        throw std::runtime_error("DVD black outline/background lost opacity");
    if (pixel_at(output, 0) != Color{0,0,0,0})
        throw std::runtime_error("DVD background transparency lost");

    std::set<std::array<std::uint8_t,3>> across_title;
    for (std::size_t event=0;event<32U;++event) {
        std::vector<std::uint8_t> frame;
        const auto hue = static_cast<std::uint8_t>((event * 41U) & 255U);
        push(frame, {0,0,0,0}, 10);
        push(frame, {0,0,0,255}, 200);
        push(frame, {255,255,255,255}, 25);
        push(frame, {hue,static_cast<std::uint8_t>(255U-hue),hue,255}, 300);
        push(frame, {128,128,128,160}, 8);
        const auto quantized = dvd_subpicture_rgba_palette(frame);
        assert_image(quantized);
        if (pixel_at(quantized, 210) != Color{255,255,255,255})
            throw std::runtime_error("white foreground lost in colored event");
        for (std::size_t pos=0;pos<quantized.size();pos+=4U) {
            if (quantized[pos+3U] != 0)
                across_title.insert({quantized[pos],quantized[pos+1],quantized[pos+2]});
        }
    }
    if (across_title.size()>8U) throw std::runtime_error("global DVD CLUT exceeds color budget");

    std::vector<std::uint8_t> transparent(4U * 100U);
    assert_image(dvd_subpicture_rgba_palette(transparent));
    bool caught = false;
    try { (void)dvd_subpicture_rgba_palette({0,0,0}); }
    catch (const std::invalid_argument&) { caught=true; }
    if (!caught) throw std::runtime_error("malformed RGBA accepted");
    std::cout << "DVD subtitle foreground, outline, alpha, four-color event and title-wide CLUT checks passed\n";
}
