// SPDX-License-Identifier: Apache-2.0
#include "bdmvauthor/progress.hpp"
#include "bdmvauthor/model.hpp"

#include <algorithm>
#include <fstream>
#include <string>
#include <string_view>

namespace bdmvauthor::detail {

std::optional<double> read_ffmpeg_progress_seconds(const std::filesystem::path& progress_file) {
    std::ifstream f(progress_file);
    if (!f) return std::nullopt;
    std::string line;
    std::optional<double> result;
    constexpr std::string_view Prefix = "out_time_us=";
    while (std::getline(f, line)) {
        if (!line.starts_with(Prefix)) continue;
        const auto value = line.substr(Prefix.size());
        if (value == "N/A") continue;
        long long us = 0;
        if (parse_integer_exact(value, us)) {
            if (us >= 0) result = static_cast<double>(us) / 1000000.0;
        } else {
            // The file can be observed while FFmpeg is appending a record.
            // Ignore a temporarily incomplete value and keep the previous one.
        }
    }
    return result;
}

double progress_fraction(double encoded_seconds, double duration_seconds) {
    if (duration_seconds <= 0.0) return 0.0;
    return std::clamp(encoded_seconds / duration_seconds, 0.0, 1.0);
}

} // namespace bdmvauthor::detail
