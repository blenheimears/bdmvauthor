// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

namespace udf25 {

constexpr std::uint32_t kSectorSize = 2048;

struct GraftPoint {
    std::filesystem::path source;
    std::string destination;
};

struct WriterOptions {
    std::filesystem::path output;
    bool output_to_stdout = false;
    std::string volume_label = "UDF_VOLUME";
    std::string application_id = "*udf25mkiso";
    std::string implementation_id = "*udf25mkiso";
    bool follow_symlinks = false;
    bool reproducible = false;
    bool verbose = false;
    bool dry_run = false;
    bool verify_after_write = true;
    bool automatic_bdmv_correction = true;
    bool automatic_hddvd_correction = true;
    // Optional cooperative cancellation hook. The writer checks it between
    // layout/write phases and while streaming large file payloads.
    std::function<bool()> cancel_requested;
    std::optional<std::int64_t> fixed_mtime;
    std::vector<GraftPoint> grafts;
    std::vector<std::string> virtual_directories;
};

struct ImageStats {
    std::uint64_t image_bytes = 0;
    std::uint64_t payload_bytes = 0;
    std::uint64_t files = 0;
    std::uint64_t directories = 0;
    std::uint64_t sectors = 0;
};

class ImageWriter {
public:
    explicit ImageWriter(WriterOptions options);
    ~ImageWriter();
    ImageWriter(ImageWriter&&) noexcept;
    ImageWriter& operator=(ImageWriter&&) noexcept;
    ImageWriter(const ImageWriter&) = delete;
    ImageWriter& operator=(const ImageWriter&) = delete;

    ImageStats build();

    static bool verify(const std::filesystem::path& image, std::ostream& report);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace udf25
