// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "bdmvauthor/model.hpp"
#include <functional>
#include <stdexcept>
#include <string>

namespace bdmvauthor {

using ProgressCallback = std::function<void(double, const std::string&)>;
using CancellationCallback = std::function<bool()>;

class AuthorCancelled : public std::runtime_error {
public:
    AuthorCancelled() : std::runtime_error("authoring cancelled") {}
};

enum class VideoEncoderProvider { Ffmpeg, Standalone };

struct AuthoringLimits {
    int bluray_transport_bitrate_kbps = 48000;
    int uhd_transport_bitrate_kbps = 70000;
    int dvd_transport_bitrate_kbps = 10080;
    // Session-only debug gate. This is deliberately not part of Project and is
    // false unless the user explicitly enables Debug -> Allow exceeding format
    // limits. It permits bitrate/transport experiments outside disc limits.
    bool allow_exceeding_format_limits = false;

    int configured_transport_bitrate_kbps(DiscTarget target) const {
        switch (target) {
            case DiscTarget::BluRay1080: return bluray_transport_bitrate_kbps;
            case DiscTarget::UltraHdBluRay2160: return uhd_transport_bitrate_kbps;
            case DiscTarget::DvdVideo480p: return dvd_transport_bitrate_kbps;
        }
        return target_max_combined_av_bitrate_kbps(target);
    }
    int combined_transport_bitrate_kbps(DiscTarget target) const {
        const int configured = configured_transport_bitrate_kbps(target);
        return allow_exceeding_format_limits ? configured
            : std::min(configured, target_max_combined_av_bitrate_kbps(target));
    }
};

struct ToolPaths {
    std::string ffmpeg = "ffmpeg";
    std::string ffprobe = "ffprobe";
    std::string x264 = "x264";
    std::string x265 = "x265";
    std::string dvdauthor = "dvdauthor";
    std::string spumux = "spumux";
    std::string mplex = "mplex";
    std::string mkisofs = "mkisofs";
    // Empty selects the private tsMuxer executable built beside BDMV Author.
    // Set explicitly only to override it for debugging/development.
    std::string tsmuxer;
    // FFmpeg is the default provider when it has the requested encoder.  GUI
    // runtime probing may resolve either field to Standalone when a distro
    // FFmpeg omits libx264/libx265 but the corresponding command-line encoder
    // is installed.
    VideoEncoderProvider h264_provider = VideoEncoderProvider::Ffmpeg;
    VideoEncoderProvider hevc_provider = VideoEncoderProvider::Ffmpeg;
};

class AuthorEngine {
public:
    explicit AuthorEngine(ToolPaths tools = {}, AuthoringLimits limits = {});
    void author(const Project& project, ProgressCallback progress = {}, CancellationCallback cancelled = {});
    static void validate_project(const Project& project, bool allow_exceeding_format_limits = false);
    static void validate_encoding_profile_for_target(const EncodingProfile& encoding, DiscTarget target,
                                                     const std::string& where = "encoding",
                                                     bool allow_exceeding_format_limits = false);
    static bool validate_java_free_bdmv(const std::filesystem::path& disc_root,
                                        std::string* error = nullptr);
private:
    ToolPaths tools_;
    AuthoringLimits limits_;
};

} // namespace bdmvauthor
