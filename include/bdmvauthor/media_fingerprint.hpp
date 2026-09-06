// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace bdmvauthor {

struct MediaFingerprint {
    static constexpr std::uint64_t SampleBytes = 1024ull * 1024ull;
    static constexpr std::uint64_t WholeFileThreshold = 3ull * SampleBytes;
    std::uint64_t file_size = 0;
    bool whole_file = true;
    std::uint64_t middle_offset = 0;
    std::string whole_sha256;
    std::string first_sha256;
    std::string middle_sha256;
    std::string last_sha256;

    std::string canonical() const;
    friend bool operator==(const MediaFingerprint&, const MediaFingerprint&) = default;
};

MediaFingerprint fingerprint_media_file(const std::filesystem::path& path);
std::string sha256_hex(std::string_view data);

} // namespace bdmvauthor
