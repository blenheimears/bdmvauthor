// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <filesystem>
#include <optional>


namespace bdmvauthor::detail {

struct FfmpegProgressSnapshot {
    double encoded_seconds = 0.0;
    double fps = 0.0;
    double speed = 0.0;
};

// Read the newest complete values emitted by FFmpeg's machine-readable
// -progress stream. fps/speed remain zero when a backend does not report them.
std::optional<FfmpegProgressSnapshot> read_ffmpeg_progress_snapshot(const std::filesystem::path& progress_file);

// Compatibility helper used by existing progress tests/callers.
std::optional<double> read_ffmpeg_progress_seconds(const std::filesystem::path& progress_file);

double progress_fraction(double encoded_seconds, double duration_seconds);

} // namespace bdmvauthor::detail
