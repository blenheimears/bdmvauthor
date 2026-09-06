// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "bdmvauthor/model.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace bdmvauthor::detail {
struct TextMask {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels; // 0 or 1
};

TextMask render_button_text_mask(const std::string& text, const ButtonStyle& style,
                                 int width, int height, int padding);
bool is_generic_font_family(const std::string& family);
std::string resolve_font_family(const std::string& family, bool bold = false, bool italic = false);
std::string resolve_font_file(const std::string& family, bool bold = false, bool italic = false);
void configure_fontconfig_runtime_environment();
const char* font_renderer_backend();
}
