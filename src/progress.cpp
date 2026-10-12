// SPDX-License-Identifier: Apache-2.0
#include "bdmvauthor/progress.hpp"
#include "bdmvauthor/model.hpp"

#include <algorithm>
#include <fstream>
#include <string>
#include <string_view>

namespace bdmvauthor::detail {

std::optional<FfmpegProgressSnapshot> read_ffmpeg_progress_snapshot(const std::filesystem::path& progress_file) {
    std::ifstream f(progress_file);
    if (!f) return std::nullopt;
    std::string line;
    std::optional<double> seconds;
    double fps = 0.0;
    double speed = 0.0;
    constexpr std::string_view TimePrefix = "out_time_us=";
    constexpr std::string_view FpsPrefix = "fps=";
    constexpr std::string_view SpeedPrefix = "speed=";
    while (std::getline(f, line)) {
        if (line.starts_with(TimePrefix)) {
            const auto value = line.substr(TimePrefix.size());
            if (value == "N/A") continue;
            long long us = 0;
            if (parse_integer_exact(value, us) && us >= 0)
                seconds = static_cast<double>(us) / 1000000.0;
            continue;
        }
        if (line.starts_with(FpsPrefix)) {
            try {
                const auto value = line.substr(FpsPrefix.size());
                if (value != "N/A") fps = std::max(0.0, std::stod(value));
            } catch (...) {}
            continue;
        }
        if (line.starts_with(SpeedPrefix)) {
            try {
                auto value = line.substr(SpeedPrefix.size());
                if (!value.empty() && value.back() == 'x') value.pop_back();
                if (value != "N/A") speed = std::max(0.0, std::stod(value));
            } catch (...) {}
        }
    }
    if (!seconds) return std::nullopt;
    return FfmpegProgressSnapshot{*seconds, fps, speed};
}

std::optional<double> read_ffmpeg_progress_seconds(const std::filesystem::path& progress_file) {
    const auto snapshot = read_ffmpeg_progress_snapshot(progress_file);
    return snapshot ? std::optional<double>{snapshot->encoded_seconds} : std::nullopt;
}

double progress_fraction(double encoded_seconds, double duration_seconds) {
    if (duration_seconds <= 0.0) return 0.0;
    return std::clamp(encoded_seconds / duration_seconds, 0.0, 1.0);
}

} // namespace bdmvauthor::detail
