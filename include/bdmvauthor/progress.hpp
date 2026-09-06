// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <filesystem>
#include <optional>

namespace bdmvauthor::detail {

// Read the newest timestamp emitted by FFmpeg's machine-readable -progress
// output. FFmpeg writes out_time_us in microseconds for each update block.
std::optional<double> read_ffmpeg_progress_seconds(const std::filesystem::path& progress_file);

double progress_fraction(double encoded_seconds, double duration_seconds);

} // namespace bdmvauthor::detail
