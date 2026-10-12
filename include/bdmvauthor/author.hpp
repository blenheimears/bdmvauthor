// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "bdmvauthor/model.hpp"
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace bdmvauthor {

using ProgressCallback = std::function<void(double, const std::string&)>;
using CancellationCallback = std::function<bool()>;

class AuthorCancelled : public std::runtime_error {
public:
    AuthorCancelled() : std::runtime_error("authoring cancelled") {}
};

enum class VideoEncoderProvider { Ffmpeg, Standalone };
enum class RemoteOperatingSystem { Auto, Posix, Windows };

struct RemoteEncodingServer {
    bool enabled = true;
    std::string name;
    std::string ssh = "ssh";
    std::string host;
    std::string user;
    int port = 22;
    RemoteOperatingSystem operating_system = RemoteOperatingSystem::Auto;
    std::string identity_file;
    std::string ffmpeg = "ffmpeg";
    std::string x264 = "x264";
    std::string x265 = "x265";
    std::string vc1enc = "vc1enc";
    std::string hdmaenc = "hdmaenc";
    VideoEncoderProvider h264_provider = VideoEncoderProvider::Ffmpeg;
    VideoEncoderProvider hevc_provider = VideoEncoderProvider::Ffmpeg;
    std::string h264_hardware_encoder;
    std::string hevc_hardware_encoder;
    std::string hardware_decoder;
    // Optional FFmpeg hardware device/render node used by this remote worker.
    // On Linux this is typically /dev/dri/renderD128. QSV receives it as
    // child_device=..., while VAAPI receives it as the device specifier.
    std::string hardware_device;
    int parallel_encodes = 1;
    // Runtime capability flags populated by GUI/CLI probing. Defaults keep
    // programmatic callers backward compatible.
    bool supports_h264 = true;
    bool supports_hevc = true;
    bool supports_mpeg2 = true;
    bool supports_vc1 = true;
    bool supports_hdma = true;
};

struct EncodingSchedulerSettings {
    bool local_enabled = true;
    // When true, Local remains outside normal scheduling while local_enabled
    // is false, but may be used if no enabled remote worker can provide the
    // requested encoder/tool.
    bool local_fallback_when_remote_unavailable = false;
    int local_parallel_encodes = 1;
    int retry_delay_seconds = 60;
    int maximum_retries = 15;
    bool retry_forever = false;
    int failover_after_consecutive_failures = 3;
};

struct EncodeJobProgress {
    std::string worker;
    std::string job;
    std::string codec;
    std::string status;
    int pass = 0;
    int passes = 1;
    double fraction = 0.0;
    double fps = 0.0;
    double speed = 0.0;
    int attempt = 1;
    int maximum_attempts = 1;
    // Populated only for a failed attempt. failure_type is "transient" or
    // "permanent"; diagnostic preserves the complete underlying tool output.
    std::string failure_type;
    std::string diagnostic;
};
using EncodeJobProgressCallback = std::function<void(const EncodeJobProgress&)>;

struct AuthoringLimits {
    int bluray_transport_bitrate_kbps = 48000;
    int uhd_transport_bitrate_kbps = 70000;
    int avchd_transport_bitrate_kbps = 18000;
    int dvd_transport_bitrate_kbps = 10080;
    // Session-only debug gate. This is deliberately not part of Project and is
    // false unless the user explicitly enables Debug -> Allow exceeding format
    // limits. It permits bitrate/transport experiments outside disc limits.
    bool allow_exceeding_format_limits = false;

    int configured_transport_bitrate_kbps(DiscTarget target) const {
        switch (target) {
            case DiscTarget::BluRay1080: return bluray_transport_bitrate_kbps;
            case DiscTarget::UltraHdBluRay2160: return uhd_transport_bitrate_kbps;
            case DiscTarget::Avchd: return avchd_transport_bitrate_kbps;
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

enum class EncodeCachePolicy {
    Enabled,
    DisableForHardwareEncodes,
    Disabled
};

struct StoragePaths {
    // Persistent encoded-media/compliance cache root. Empty keeps the
    // platform/environment default. This is runtime application state, not
    // project state.
    std::filesystem::path cache_root;
    // Parent directory for per-authoring-run scratch/work trees. Empty keeps
    // the platform temporary-directory default.
    std::filesystem::path temporary_root;
    // Application-level encoded-video cache policy. The Project's existing
    // use_encode_cache flag can still disable encoded-media caching for an
    // individual project; this policy can additionally bypass only hardware
    // video encodes or all encoded-media cache use.
    EncodeCachePolicy encode_cache_policy = EncodeCachePolicy::Enabled;
};

struct ToolPaths {
    std::string ffmpeg = "ffmpeg";
    std::string ffprobe = "ffprobe";
    std::string x264 = "x264";
    std::string x265 = "x265";
    // Experimental VC-1 support always uses the vc1enc process frontend.
    std::string vc1enc = "vc1enc";
    // Experimental DTS-HD MA encoding runs the external frontend only.
    std::string hdmaenc = "hdmaenc";
    std::string eac371enc = "eac371enc";
    std::string dvdauthor = "dvdauthor";
    std::string spumux = "spumux";
    // Optional builder-supplied standalone native bitmap subtitle converter.
    // Auto-resolves the sibling executable or PATH; no Java runtime required.
    std::string bdsup2sub = "bdsup2sub";
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

    // Exact FFmpeg encoder names selected by application settings after
    // runtime capability discovery (for example h264_nvenc, hevc_vaapi,
    // h264_vulkan, h264_videotoolbox). Empty means that hardware encoding for
    // that codec is unavailable/not configured.
    std::string h264_hardware_encoder;
    std::string hevc_hardware_encoder;
    // FFmpeg -hwaccel value selected independently for decoding. Empty means
    // software decoding; "auto" delegates backend choice to FFmpeg. Hardware
    // decoding remains local even when encoding is delegated to an SSH server,
    // because the local FFmpeg process prepares the filtered frame stream.
    std::string hardware_decoder;
    // Video processing uses a separate backend, independent of decoding and
    // encoding. Defaults preserve the original zscale/tonemap pipeline.
    // Scaling: software (Lanczos), native (FFmpeg scale+pad), vaapi, qsv, cuda, vulkan. HDR10 tone mapping: software (quality-first), software-fast (pre-scale HDR), vaapi.
    std::string scaling_filter_backend = "software";
    std::string tonemap_filter_backend = "software";
    std::string filter_hardware_device; // optional render node, e.g. /dev/dri/renderD128
    std::function<void(const std::string&)> filter_fallback_warning;

    // Optional SSH encoding server. These values are application/runtime state
    // and are never serialized in a Project. The local FFmpeg process decodes,
    // filters and pipes frames/PCM to the remote encoder; the elementary stream
    // is returned on SSH stdout, so no media temporary files are created remotely.
    bool remote_encoding = false;
    std::string ssh = "ssh";
    std::string remote_host;
    std::string remote_user;
    int remote_ssh_port = 22;
    RemoteOperatingSystem remote_operating_system = RemoteOperatingSystem::Auto;
    std::string remote_identity_file;
    std::string remote_ffmpeg = "ffmpeg";
    std::string remote_x264 = "x264";
    std::string remote_x265 = "x265";
    std::string remote_vc1enc = "vc1enc";
    std::string remote_hdmaenc = "hdmaenc";
    // Optional device/render node for the legacy single remote worker.
    std::string remote_hardware_device;

    // Multi-worker scheduler. The legacy single remote fields above remain for
    // CLI/backward compatibility and are converted into a one-server pool when
    // remote_servers is empty. These values are application state, never Project data.
    std::vector<RemoteEncodingServer> remote_servers;
    EncodingSchedulerSettings scheduler;
};

class AuthorEngine {
public:
    explicit AuthorEngine(ToolPaths tools = {}, AuthoringLimits limits = {}, StoragePaths storage = {});
    void author(const Project& project, ProgressCallback progress = {}, CancellationCallback cancelled = {},
                EncodeJobProgressCallback encode_progress = {});
    static void validate_project(const Project& project, bool allow_exceeding_format_limits = false);
    static void validate_encoding_profile_for_target(const EncodingProfile& encoding, DiscTarget target,
                                                     const std::string& where = "encoding",
                                                     bool allow_exceeding_format_limits = false);
    static bool validate_java_free_bdmv(const std::filesystem::path& disc_root,
                                        std::string* error = nullptr);
private:
    ToolPaths tools_;
    AuthoringLimits limits_;
    StoragePaths storage_;
};

} // namespace bdmvauthor
