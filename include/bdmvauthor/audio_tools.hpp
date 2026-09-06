// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <filesystem>

namespace bdmvauthor::detail {

// Build the Blu-ray-style elementary stream expected by the vendored tsMuxer:
// 48 kHz TrueHD frames interleaved with a 640 kb/s, 48 kHz AC-3 compatibility
// stream. This is experimental and intentionally narrow to the format emitted
// by BDMV Author's FFmpeg commands. The return value is the conservative
// one-second peak bitrate of the merged TrueHD+AC-3 elementary stream in kb/s.
int merge_truehd_ac3_core(const std::filesystem::path& truehd_path,
                           const std::filesystem::path& ac3_path,
                           const std::filesystem::path& output_path);

} // namespace bdmvauthor::detail
