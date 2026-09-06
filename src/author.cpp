// SPDX-License-Identifier: Apache-2.0
#include "bdmvauthor/author.hpp"
#include "bdmvauthor/audio_tools.hpp"
#include "bdmvauthor/compliance.hpp"
#include "bdmvauthor/hdmv.hpp"
#include "bdmvauthor/font_renderer.hpp"
#include "bdmvauthor/progress.hpp"
#include "bdmvauthor/media_fingerprint.hpp"
#include "udf_writer.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstdio>
#include <fcntl.h>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <future>
#include <iomanip>
#include <initializer_list>
#include <iterator>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#else
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
extern char **environ;
#endif

namespace bdmvauthor {
namespace {
namespace fs = std::filesystem;

std::atomic<const CancellationCallback*> active_cancellation_callback{nullptr};

bool authoring_cancel_requested() {
    const auto* callback = active_cancellation_callback.load(std::memory_order_acquire);
    return callback && static_cast<bool>(*callback) && (*callback)();
}

void authoring_cancellation_point() {
    if (authoring_cancel_requested()) throw AuthorCancelled();
}

struct CancellationRegistration {
    const CancellationCallback* previous = nullptr;
    explicit CancellationRegistration(const CancellationCallback& callback) {
        previous = active_cancellation_callback.exchange(&callback, std::memory_order_acq_rel);
    }
    ~CancellationRegistration() { active_cancellation_callback.store(previous, std::memory_order_release); }
};

std::string shq(const fs::path& p) {
    std::string s = p.string(), r = "\"";
    for (char c : s) {
        if (c == '\"') r += "\\\"";
        else r += c;
    }
    return r += "\"";
}
std::string shq_s(const std::string& s) { return shq(fs::path(s)); }

std::string video_codec_name(VideoCodec c) {
    switch (c) {
        case VideoCodec::X264: return "x264";
        case VideoCodec::Mpeg2: return "mpeg2";
        case VideoCodec::Hevc: return "hevc";
    }
    return "unknown";
}
std::string audio_codec_name(AudioCodec c) {
    switch(c){case AudioCodec::Ac3:return "ac3";case AudioCodec::Lpcm:return "lpcm";case AudioCodec::Dca:return "dca";case AudioCodec::TrueHdAc3:return "truehd-ac3";}
    return "unknown";
}
fs::path default_encode_cache_root() {
    if (const char* override_dir = std::getenv("BDMVAUTHOR_CACHE_DIR"); override_dir && *override_dir)
        return fs::path(override_dir);
    const char* home = std::getenv("HOME");
#if defined(_WIN32)
    if (!home || !*home) home = std::getenv("USERPROFILE");
#endif
    if (!home || !*home) return {};
    return fs::path(home) / ".cache" / "bdmvauthor" / "encoded-clips";
}

struct CacheMetadata {
    std::string category;
    std::string source_name;
    std::string target;
    std::string codec;
    std::string resolution;
    std::string frame_rate;
    int bitrate_kbps = 0;
    int peak_bitrate_kbps = 0;
    std::string settings;
};

std::string cache_target_name(DiscTarget target) {
    switch (target) {
        case DiscTarget::BluRay1080: return "Blu-ray";
        case DiscTarget::UltraHdBluRay2160: return "Ultra HD Blu-ray";
        case DiscTarget::DvdVideo480p: return "DVD-Video";
    }
    return "Unknown";
}

std::string cache_source_name(const fs::path& source) {
    return source.empty() ? std::string{} : source.filename().string();
}

fs::path cache_metadata_path(const fs::path& cached) { return fs::path(cached.string() + ".meta"); }

long long cache_existing_created_unix(const fs::path& cached) {
    std::ifstream f(cache_metadata_path(cached));
    if (!f) return 0;
    std::string key;
    while (f >> key) {
        if (key == "createdUnix") { long long value = 0; if (f >> value) return value; return 0; }
        std::string rest; std::getline(f, rest);
    }
    return 0;
}

int cache_peak_bitrate_kbps(const fs::path& cached) {
    std::ifstream f(cache_metadata_path(cached));
    if (!f) return 0;
    std::string key;
    while (f >> key) {
        if (key == "peakBitrateKbps") { int value = 0; if (f >> value) return value; return 0; }
        std::string rest; std::getline(f, rest);
    }
    return 0;
}

long long cache_file_mtime_unix(const fs::path& cached) {
    std::error_code ec;
    const auto file_time = fs::last_write_time(cached, ec);
    if (ec) return 0;
    const auto system_time = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        file_time - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    return static_cast<long long>(std::chrono::system_clock::to_time_t(system_time));
}

void touch_cache_metadata(const fs::path& cached, const CacheMetadata& metadata) {
    if (cached.empty()) return;
    std::error_code ec;
    fs::create_directories(cached.parent_path(), ec);
    if (ec) return;
    const long long now = static_cast<long long>(std::time(nullptr));
    long long created = cache_existing_created_unix(cached);
    if (created <= 0) created = cache_file_mtime_unix(cached);
    if (created <= 0) created = now;
    const fs::path meta = cache_metadata_path(cached);
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path tmp = fs::path(meta.string() + ".tmp-" + std::to_string(nonce));
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f) return;
        f << "BDMVAUTHOR_CACHE_META 1\n"
          << "category " << std::quoted(metadata.category) << '\n'
          << "sourceName " << std::quoted(metadata.source_name) << '\n'
          << "target " << std::quoted(metadata.target) << '\n'
          << "codec " << std::quoted(metadata.codec) << '\n'
          << "resolution " << std::quoted(metadata.resolution) << '\n'
          << "frameRate " << std::quoted(metadata.frame_rate) << '\n'
          << "bitrateKbps " << metadata.bitrate_kbps << '\n'
          << "peakBitrateKbps " << metadata.peak_bitrate_kbps << '\n'
          << "settings " << std::quoted(metadata.settings) << '\n'
          << "createdUnix " << created << '\n'
          << "lastUsedUnix " << now << '\n';
        f.flush();
        if (!f) { f.close(); fs::remove(tmp, ec); return; }
    }
    if (fs::exists(meta, ec)) { ec.clear(); fs::remove(meta, ec); }
    ec.clear(); fs::rename(tmp, meta, ec);
    if (ec) fs::remove(tmp, ec);
}

struct VideoTimingMode {
    std::string frame_rate;          // encoded frames/second, exact FFmpeg rational
    double frame_rate_value = 0.0;
    bool interlaced = false;
    bool fake_interlaced = false; // progressive pictures signaled as PAFF for Blu-ray 25p/29.97p AVC
    std::string temporal_rate;       // progressive samples/second before tinterlace
    double temporal_rate_value = 0.0;
    std::string tsmuxer_fps;         // decimal spelling accepted by tsMuxer
    std::string description;
    int video_width = 0;
    int video_height = 0;
    int graphics_width = 0;
    int graphics_height = 0;
    std::string aspect_ratio = "16:9";
    std::string sample_aspect_ratio = "1/1";
};

bool dvd_timing_is_pal(const VideoTimingMode& timing) {
    return timing.frame_rate == "25";
}

TargetGeometry target_geometry_for_timing(DiscTarget target, const VideoTimingMode& timing) {
    if (timing.video_width > 0 && timing.video_height > 0) {
        const int gw = timing.graphics_width > 0 ? timing.graphics_width : timing.video_width;
        const int gh = timing.graphics_height > 0 ? timing.graphics_height : timing.video_height;
        return {timing.video_width, timing.video_height, gw, gh};
    }
    if (target == DiscTarget::DvdVideo480p && dvd_timing_is_pal(timing)) return {720,576,720,576};
    return target_geometry(target);
}

CacheMetadata video_cache_metadata(const std::string& category, const fs::path& source, DiscTarget target,
                                   const EncodingProfile& e, const VideoTimingMode& timing, const ToolPaths& tools, int peak_bitrate_limit_kbps = 0) {
    CacheMetadata m;
    m.category = category;
    m.source_name = cache_source_name(source);
    m.target = cache_target_name(target);
    m.codec = video_codec_name(e.video_codec);
    const auto geometry = target_geometry_for_timing(target, timing);
    m.resolution = std::to_string(geometry.video_width) + "x" + std::to_string(geometry.video_height);
    m.frame_rate = timing.description.empty() ? timing.frame_rate : timing.description;
    m.bitrate_kbps = e.video_bitrate_kbps;
    std::ostringstream settings;
    const std::string provider = e.video_codec == VideoCodec::X264
        ? (tools.h264_provider == VideoEncoderProvider::Ffmpeg ? "ffmpeg/libx264" : "x264")
        : e.video_codec == VideoCodec::Hevc
            ? (tools.hevc_provider == VideoEncoderProvider::Ffmpeg ? "ffmpeg/libx265" : "x265")
            : "ffmpeg";
    settings << "provider=" << provider
             << "; preset=" << (e.video_codec == VideoCodec::Hevc ? e.x265_preset : e.x264_preset)
             << "; two-pass=" << (e.two_pass ? "yes" : "no")
             << "; keyframe=" << e.keyframe_interval_frames
             << "; minrate-kbps=" << e.video_min_bitrate_kbps
             << "; maxrate-kbps=" << e.video_max_bitrate_kbps
             << "; peak-limit-kbps=" << peak_bitrate_limit_kbps;
    const auto advanced = advanced_video_options_for_codec(e);
    if (!advanced.empty()) settings << "; advanced=" << advanced;
    m.settings = settings.str();
    return m;
}

CacheMetadata audio_cache_metadata(const std::string& category, const fs::path& source, DiscTarget target,
                                   const EncodingProfile& e, int channels, int source_ordinal, int peak_bitrate_kbps = 0) {
    CacheMetadata m;
    m.category = category;
    m.source_name = cache_source_name(source);
    m.target = cache_target_name(target);
    m.codec = audio_codec_name(e.audio_codec);
    if (e.audio_codec == AudioCodec::Ac3) m.bitrate_kbps = e.ac3_bitrate_kbps;
    else if (e.audio_codec == AudioCodec::Dca) m.bitrate_kbps = e.dca_bitrate_kbps;
    else if (e.audio_codec == AudioCodec::Lpcm && channels > 0)
        m.bitrate_kbps = (e.lpcm_sample_rate_hz * e.lpcm_bit_depth * channels) / 1000;
    m.peak_bitrate_kbps = peak_bitrate_kbps;
    std::ostringstream settings;
    settings << "source-track=" << (source_ordinal + 1) << "; channels=" << channels;
    if (e.audio_codec == AudioCodec::Lpcm)
        settings << "; sample-rate=" << e.lpcm_sample_rate_hz << "; bit-depth=" << e.lpcm_bit_depth;
    const auto advanced = advanced_audio_options_for_codec(e);
    if (!advanced.empty()) settings << "; advanced=" << advanced;
    m.settings = settings.str();
    return m;
}

std::string dvd_video_format(const VideoTimingMode& timing) {
    return dvd_timing_is_pal(timing) ? "pal" : "ntsc";
}

std::string dvd_spumux_format(const VideoTimingMode& timing) {
    return dvd_timing_is_pal(timing) ? "PAL" : "NTSC";
}

std::uint8_t hdmv_frame_rate_code(const VideoTimingMode& timing) {
    const double fps = timing.frame_rate_value;
    if (std::abs(fps - 24000.0/1001.0) < 0.02) return 1;
    if (std::abs(fps - 24.0) < 0.02) return 2;
    if (std::abs(fps - 25.0) < 0.02) return 3;
    if (std::abs(fps - 30000.0/1001.0) < 0.02) return 4;
    if (std::abs(fps - 50.0) < 0.02) return 6;
    if (std::abs(fps - 60000.0/1001.0) < 0.02 || std::abs(fps - 60.0) < 0.02) return 7;
    throw std::runtime_error("cannot map menu timing to an HDMV IG frame-rate code: " + timing.description);
}

std::string timing_cache_text(const VideoTimingMode& t) {
    return t.frame_rate + (t.interlaced ? "i" : "p") + (t.fake_interlaced ? "/fake-interlaced" : "") + "/temporal=" + t.temporal_rate;
}

std::string video_cache_key(const MediaFingerprint& fp,const EncodingProfile& e,const Project& p,
                            const VideoTimingMode& timing, const ToolPaths& tools, int peak_bitrate_limit_kbps) {
    std::ostringstream k;k<<"bdmvauthor-video-cache-v13\n"<<fp.canonical()<<"\ncodec="<<video_codec_name(e.video_codec)
      <<"\nbitrate="<<e.video_bitrate_kbps<<"\nminrate="<<e.video_min_bitrate_kbps<<"\nmaxrate="<<e.video_max_bitrate_kbps<<"\nx264Preset="<<e.x264_preset<<"\nx265Preset="<<e.x265_preset<<"\ntwoPass="<<(e.two_pass?1:0)
      <<"\nframeTiming="<<timing_cache_text(timing)<<"\ntarget="<<static_cast<int>(p.target)
      <<"\nkeyframeInterval="<<e.keyframe_interval_frames
      <<"\nprovider=" << (e.video_codec==VideoCodec::X264
            ? (tools.h264_provider==VideoEncoderProvider::Ffmpeg?"ffmpeg":"standalone")
            : e.video_codec==VideoCodec::Hevc
                ? (tools.hevc_provider==VideoEncoderProvider::Ffmpeg?"ffmpeg":"standalone")
                : "ffmpeg")
      <<"\nadvancedVideo="<<advanced_video_options_for_codec(e)
      <<"\npeakBitrateLimitKbps="<<peak_bitrate_limit_kbps;
    const auto cache_geometry = target_geometry_for_timing(p.target, timing);
    k << "\nsize=" << cache_geometry.video_width << "x" << cache_geometry.video_height
      << "\naspect=" << timing.aspect_ratio << "\nsar=" << timing.sample_aspect_ratio;
    if (target_is_dvd(p.target)) k << "\ndvdRaster=multi-mode-v3";
    k << "\n";
    return sha256_hex(k.str());
}
std::string menu_video_cache_key(const fs::path& input, const fs::path& overlay, const EncodingProfile& e,
                                 const Project& p, const VideoTimingMode& timing, const ToolPaths& tools,
                                 bool source_interlaced, bool still, double seconds, const Rgba& background_color,
                                 bool loop_media, int peak_bitrate_limit_kbps) {
    std::ostringstream k;
    k << "bdmvauthor-menu-video-cache-v4\n";
    if (input.empty()) k << "input=generated-color\n";
    else k << "input=" << fingerprint_media_file(input).canonical() << "\n";
    if (overlay.empty()) k << "overlay=none\n";
    else k << "overlay=" << fingerprint_media_file(overlay).canonical() << "\n";
    k << "codec=" << video_codec_name(e.video_codec)
      << "\nbitrate=" << e.video_bitrate_kbps
      << "\nminrate=" << e.video_min_bitrate_kbps << "\nmaxrate=" << e.video_max_bitrate_kbps
      << "\nx264Preset=" << e.x264_preset << "\nx265Preset=" << e.x265_preset
      << "\ntwoPass=" << (e.two_pass ? 1 : 0)
      << "\nframeTiming=" << timing_cache_text(timing)
      << "\ntarget=" << static_cast<int>(p.target)
      << "\nkeyframeInterval=" << e.keyframe_interval_frames
      << "\nprovider=" << (e.video_codec == VideoCodec::X264
            ? (tools.h264_provider == VideoEncoderProvider::Ffmpeg ? "ffmpeg" : "standalone")
            : e.video_codec == VideoCodec::Hevc
                ? (tools.hevc_provider == VideoEncoderProvider::Ffmpeg ? "ffmpeg" : "standalone")
                : "ffmpeg")
      << "\nadvancedVideo=" << advanced_video_options_for_codec(e)
      << "\nsourceInterlaced=" << (source_interlaced ? 1 : 0)
      << "\nstill=" << (still ? 1 : 0)
      << "\nduration=" << std::fixed << std::setprecision(6) << seconds
      << "\nmediaLoop=" << (loop_media ? 1 : 0)
      << "\npeakBitrateLimitKbps=" << peak_bitrate_limit_kbps
      << "\nbackground=" << static_cast<unsigned>(background_color.r) << ','
      << static_cast<unsigned>(background_color.g) << ','
      << static_cast<unsigned>(background_color.b) << ','
      << static_cast<unsigned>(background_color.a);
    const auto geometry = target_geometry_for_timing(p.target, timing);
    k << "\nsize=" << geometry.video_width << 'x' << geometry.video_height
      << "\naspect=" << timing.aspect_ratio << "\nsar=" << timing.sample_aspect_ratio;
    if (target_is_dvd(p.target)) k << "\ndvdRaster=multi-mode-v3";
    k << "\n";
    return sha256_hex(k.str());
}


std::string audio_cache_key(const MediaFingerprint& fp,const EncodingProfile& e,int source_ordinal,int output_channels) {
    std::ostringstream k;k<<"bdmvauthor-audio-cache-v7\n"<<fp.canonical()<<"\ncodec="<<audio_codec_name(e.audio_codec)
      <<"\nsourceAudioOrdinal="<<source_ordinal<<"\noutputChannels="<<output_channels
      <<"\nac3Bitrate="<<e.ac3_bitrate_kbps<<"\ndcaBitrate="<<e.dca_bitrate_kbps
      <<"\nadvancedAudio="<<advanced_audio_options_for_codec(e)
      <<"\nlpcmSampleRate="<<e.lpcm_sample_rate_hz<<"\nlpcmBitDepth="<<e.lpcm_bit_depth;
    if(e.audio_codec==AudioCodec::TrueHdAc3)k<<"\nac3CoreAdvanced="<<e.ac3_advanced_options;
    k<<"\nfixedCodecSampleRate="<<(e.audio_codec==AudioCodec::Lpcm?e.lpcm_sample_rate_hz:48000)<<"\n";
    return sha256_hex(k.str());
}
std::string menu_audio_cache_key(const MediaFingerprint& fp, const EncodingProfile& e, DiscTarget target,
                                 double seconds, bool dvd_elementary, bool loop_media) {
    std::ostringstream k;
    k << (dvd_elementary ? "bdmvauthor-dvd-menu-audio-cache-v2\n" : "bdmvauthor-menu-audio-cache-v2\n")
      << fp.canonical() << "\ntarget=" << static_cast<int>(target)
      << "\nduration=" << std::fixed << std::setprecision(6) << seconds
      << "\nmediaLoop=" << (loop_media ? 1 : 0) << '\n';
    if (dvd_elementary) {
        k << "codec=" << audio_codec_name(e.audio_codec)
          << "\nac3Bitrate=" << e.ac3_bitrate_kbps << "\nac3Advanced=" << e.ac3_advanced_options
          << "\nlpcmAdvanced=" << e.lpcm_advanced_options << "\nlpcmSampleRate=" << e.lpcm_sample_rate_hz
          << "\nlpcmBitDepth=" << e.lpcm_bit_depth << "\ndvdLpcm=raw-big-endian-for-mplex-v2\n";
    } else {
        k << "base=" << audio_cache_key(fp, e, 0, 0) << "\n";
    }
    return sha256_hex(k.str());
}

std::string video_extension(const EncodingProfile& e){if(e.video_codec==VideoCodec::X264)return ".264";if(e.video_codec==VideoCodec::Hevc)return ".265";return ".m2v";}
std::string audio_extension(const EncodingProfile& e){
    switch(e.audio_codec){case AudioCodec::Ac3:return ".ac3";case AudioCodec::Lpcm:return ".wav";case AudioCodec::Dca:return ".dts";case AudioCodec::TrueHdAc3:return ".thd+ac3";}
    return ".audio";
}
std::string video_meta_for(const fs::path& path,const EncodingProfile& e,const VideoTimingMode& timing){
    const std::string ar = ", ar=" + timing.aspect_ratio;
    if(e.video_codec==VideoCodec::X264) return "V_MPEG4/ISO/AVC, "+shq(path)+", fps="+timing.tsmuxer_fps+ar+", insertSEI, contSPS\n";
    if(e.video_codec==VideoCodec::Hevc) return "V_MPEGH/ISO/HEVC, "+shq(path)+", fps="+timing.tsmuxer_fps+ar+"\n";
    return "V_MPEG-2, "+shq(path)+", fps="+timing.tsmuxer_fps+ar+"\n";
}
std::string audio_meta_for(const fs::path& path,const EncodingProfile& e,const std::string& lang){
    std::string type="A_AC3"; if(e.audio_codec==AudioCodec::Lpcm)type="A_LPCM";else if(e.audio_codec==AudioCodec::Dca)type="A_DTS";
    return type+", "+shq(path)+", lang="+lang+"\n";
}
int configured_audio_rate_kbps(const EncodingProfile& e, DiscTarget target, int channels) {
    channels = std::max(1, channels);
    switch (e.audio_codec) {
        case AudioCodec::Lpcm:
            return lpcm_bitrate_kbps(e.lpcm_sample_rate_hz, e.lpcm_bit_depth,
                                     std::min(channels, lpcm_max_channels_for_target(target, e.lpcm_sample_rate_hz)));
        case AudioCodec::Dca: return e.dca_bitrate_kbps;
        case AudioCodec::TrueHdAc3:
            // This helper is retained for the DVD fixed-rate preflight path,
            // where TrueHD is not a legal codec. Blu-ray/UHD TrueHD is encoded
            // first and its actual peak is measured before the video budget is set.
            return 0;
        case AudioCodec::Ac3: return e.ac3_bitrate_kbps;
    }
    return 0;
}

int fixed_encoded_audio_peak_kbps(const EncodingProfile& e, DiscTarget target, int channels) {
    channels = std::max(1, channels);
    switch (e.audio_codec) {
        case AudioCodec::Ac3: return e.ac3_bitrate_kbps;
        case AudioCodec::Dca: return e.dca_bitrate_kbps;
        case AudioCodec::Lpcm:
            return lpcm_bitrate_kbps(e.lpcm_sample_rate_hz, e.lpcm_bit_depth,
                                     std::min(channels, lpcm_max_channels_for_target(target, e.lpcm_sample_rate_hz)));
        case AudioCodec::TrueHdAc3: return 0; // measured after the lossless encode
    }
    return 0;
}

int derived_video_peak_limit_kbps(DiscTarget target, VideoCodec codec, int aggregate_audio_peak_kbps,
                                  int transport_budget_kbps, int requested_maxrate_kbps,
                                  bool allow_exceeding_format_limits) {
    if (allow_exceeding_format_limits) return std::max(1, requested_maxrate_kbps);
    const int codec_limit = std::min(video_codec_max_bitrate_kbps(target, codec), requested_maxrate_kbps);
    if (target_is_dvd(target)) return codec_limit;
    const int after_audio = transport_budget_kbps - std::max(0, aggregate_audio_peak_kbps);
    return std::max(0, std::min(codec_limit, after_audio));
}

int dvd_mpeg2_vbr_peak_kbps(int average_video_kbps, int requested_maxrate_kbps, int aggregate_audio_kbps,
                            const std::string& where, bool allow_exceeding_format_limits = false) {
    if (allow_exceeding_format_limits) {
        if (requested_maxrate_kbps < average_video_kbps)
            throw std::runtime_error(where + " maxrate of " + std::to_string(requested_maxrate_kbps) +
                " kb/s is below the configured average video bitrate of " + std::to_string(average_video_kbps) + " kb/s");
        return requested_maxrate_kbps;
    }
    // FFmpeg's documented DVD target uses constrained VBR at 6000 kb/s average,
    // 9000 kb/s peak, minrate 0 and the 1,835,008-bit MPEG-2 VBV.  Use that
    // player-friendly 9 Mb/s peak when the configured audio budget permits it.
    // If the user explicitly selected a higher average, allow the peak to rise
    // only as far as the DVD-Video 9.8 Mb/s video limit and the 10.08 Mb/s
    // combined program-stream budget permit.
    constexpr int practical_peak_kbps = 9000;
    const int spec_peak_kbps = std::max(0, std::min(
        std::min(video_codec_max_bitrate_kbps(DiscTarget::DvdVideo480p, VideoCodec::Mpeg2), requested_maxrate_kbps),
        target_max_combined_av_bitrate_kbps(DiscTarget::DvdVideo480p) - std::max(0, aggregate_audio_kbps)));
    const int preferred_peak_kbps = std::min(practical_peak_kbps, spec_peak_kbps);
    const int peak_kbps = average_video_kbps < preferred_peak_kbps ? preferred_peak_kbps : spec_peak_kbps;
    if (average_video_kbps > peak_kbps)
        throw std::runtime_error(where + " average video bitrate of " + std::to_string(average_video_kbps) +
            " kb/s exceeds the DVD peak/maxrate of " + std::to_string(peak_kbps) +
            " kb/s after reserving " + std::to_string(std::max(0, aggregate_audio_kbps)) +
            " kb/s for audio; reduce the configured video/audio bitrate or maxrate");
    return peak_kbps;
}

void validate_video_peak_budget(const EncodingProfile& e, DiscTarget target, int aggregate_audio_peak_kbps,
                                int peak_limit_kbps, int transport_budget_kbps, const std::string& where) {
    if (target_is_dvd(target)) return;
    if (peak_limit_kbps < 1000)
        throw std::runtime_error(where + " audio peak of " + std::to_string(aggregate_audio_peak_kbps) +
                                 " kb/s leaves less than 1 Mb/s for video under the " +
                                 std::to_string(transport_budget_kbps) + " kb/s transport limit");
    if (e.video_bitrate_kbps > peak_limit_kbps)
        throw std::runtime_error(where + " requested average video bitrate of " + std::to_string(e.video_bitrate_kbps) +
                                 " kb/s exceeds the " + std::to_string(peak_limit_kbps) +
                                 " kb/s video peak budget left after reserving " +
                                 std::to_string(aggregate_audio_peak_kbps) + " kb/s for authored audio");
}

void run(const std::string& c);

int probe_elementary_audio_peak_kbps(const ToolPaths& tools, const fs::path& elementary, const fs::path& probe_output) {
    fs::create_directories(probe_output.parent_path());
    std::string command = shq_s(tools.ffprobe) +
        " -v error -show_entries packet=pts_time,duration_time,size -of compact=p=0:nk=0 " +
        shq(elementary) + " > " + shq(probe_output) + " 2>";
#ifdef _WIN32
    command += "NUL";
#else
    command += "/dev/null";
#endif
    run(command);
    std::ifstream f(probe_output);
    if (!f) throw std::runtime_error("cannot read audio peak probe output: " + probe_output.string());
    struct Packet { double pts = 0.0; std::uint64_t bytes = 0; };
    std::deque<Packet> window;
    std::uint64_t window_bytes = 0;
    std::uint64_t max_window_bytes = 0;
    double fallback_pts = 0.0;
    double max_packet_kbps = 0.0;
    std::string line;
    while (std::getline(f, line)) {
        double pts = fallback_pts;
        double duration = 0.0;
        std::uint64_t size = 0;
        std::istringstream fields(line);
        for (std::string field; std::getline(fields, field, '|');) {
            const auto eq = field.find('=');
            if (eq == std::string::npos) continue;
            const auto key = field.substr(0, eq);
            const auto value = field.substr(eq + 1U);
            try {
                if (key == "pts_time" && value != "N/A") pts = std::stod(value);
                else if (key == "duration_time" && value != "N/A") duration = std::stod(value);
                else if (key == "size" && value != "N/A") { std::istringstream number(value); number >> size; }
            } catch (...) {}
        }
        if (size == 0U) { fallback_pts = pts + std::max(0.0, duration); continue; }
        while (!window.empty() && pts - window.front().pts >= 1.0) {
            window_bytes -= window.front().bytes;
            window.pop_front();
        }
        window.push_back({pts, size});
        window_bytes += size;
        max_window_bytes = std::max(max_window_bytes, window_bytes);
        if (duration > 0.0)
            max_packet_kbps = std::max(max_packet_kbps, static_cast<double>(size) * 8.0 / duration / 1000.0);
        fallback_pts = pts + std::max(0.0, duration);
    }
    if (max_window_bytes == 0U) throw std::runtime_error("ffprobe found no audio packets in " + elementary.string());
    const double rolling_kbps = static_cast<double>(max_window_bytes) * 8.0 / 1000.0;
    return static_cast<int>(std::ceil(std::max(rolling_kbps, max_packet_kbps)));
}

int max_audio_channels_for_encoding(const EncodingProfile& e, DiscTarget target) {
    if (e.audio_codec == AudioCodec::Lpcm) return lpcm_max_channels_for_target(target, e.lpcm_sample_rate_hz);
    return 6;
}

int resolved_audio_output_channels(int source_channels, const EncodingProfile& e, DiscTarget target,
                                   int requested_channels, const std::string& where) {
    source_channels = std::max(1, source_channels);
    const int maximum = max_audio_channels_for_encoding(e, target);
    if (requested_channels <= 0) return std::min(source_channels, maximum);
    if (requested_channels > source_channels)
        throw std::runtime_error(where + " requests " + std::to_string(requested_channels) +
                                 " output channels from a " + std::to_string(source_channels) +
                                 "-channel source; BDMV Author does not upmix");
    if (requested_channels > maximum)
        throw std::runtime_error(where + " requests " + std::to_string(requested_channels) +
                                 " channels but the selected codec/target supports at most " + std::to_string(maximum));
    return requested_channels;
}

double passthrough_video_rate_kbps(const VideoStreamInfo& info) {
    if (info.codec_name == "h264" && info.h264_hrd_max_bitrate_bps > 0.0) return info.h264_hrd_max_bitrate_bps / 1000.0;
    if (info.codec_name == "hevc" && info.hevc_hrd_max_bitrate_bps > 0.0) return info.hevc_hrd_max_bitrate_bps / 1000.0;
    if (info.codec_name == "mpeg2video" && info.mpeg2_header_bitrate_bps > 0.0) return info.mpeg2_header_bitrate_bps / 1000.0;
    if (info.sampled_average_bitrate_bps > 0.0) return info.sampled_average_bitrate_bps / 1000.0;
    return 0.0;
}
bool cache_file_usable(const fs::path& cached) {
    std::error_code ec;
    if (!fs::is_regular_file(cached, ec) || ec) return false;
    const auto size = fs::file_size(cached, ec);
    return !ec && size > 0;
}

fs::path cache_publish_temp_path(const fs::path& cached) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    // Small metadata publications (for example compliance verdicts) use a
    // sibling temporary file so the final rename never crosses filesystems.
    return cached.parent_path() / (cached.filename().string() + ".tmp-" + std::to_string(stamp));
}

fs::path cache_encode_temp_dir(const fs::path& cached) {
    std::error_code ec;
    fs::create_directories(cached.parent_path(), ec);
    if (ec) throw std::runtime_error("cannot create encode cache directory: " + cached.parent_path().string());
    for (unsigned attempt = 0; attempt < 100U; ++attempt) {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto dir = cached.parent_path() /
            ("." + cached.filename().string() + ".tmp-" + std::to_string(stamp) + "-" + std::to_string(attempt));
        ec.clear();
        if (fs::create_directory(dir, ec)) return dir;
        if (ec && ec != std::errc::file_exists)
            throw std::runtime_error("cannot create temporary encode-cache directory: " + dir.string());
    }
    throw std::runtime_error("cannot allocate a unique temporary encode-cache directory beside " + cached.string());
}

struct CacheTempDirCleanup {
    fs::path dir;
    ~CacheTempDirCleanup() {
        if (dir.empty()) return;
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

struct RenderTempCleanup {
    fs::path work_dir;
    fs::path temporary_image;
    ~RenderTempCleanup() {
        std::error_code ec;
        if (!temporary_image.empty()) fs::remove(temporary_image, ec);
        if (authoring_cancel_requested() && !work_dir.empty()) {
            ec.clear();
            fs::remove_all(work_dir, ec);
        }
    }
};

void publish_rendered_image(const fs::path& temporary, const fs::path& final_path) {
    std::error_code ec;
    if (!fs::is_regular_file(temporary, ec) || ec || fs::file_size(temporary, ec) == 0 || ec)
        throw std::runtime_error("render completed without creating a usable image: " + temporary.string());
    const auto parent = final_path.parent_path().empty() ? fs::path(".") : final_path.parent_path();
    fs::create_directories(parent, ec);
    if (ec) throw std::runtime_error("cannot create output directory: " + parent.string());
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path backup = parent / ("." + final_path.filename().string() + ".previous-" + std::to_string(stamp));
    const bool had_existing = fs::exists(final_path, ec) && !ec;
    if (had_existing) {
        fs::rename(final_path, backup, ec);
        if (ec) throw std::runtime_error("cannot move the previous output image aside before publishing: " + final_path.string());
    }
    fs::rename(temporary, final_path, ec);
    if (ec) {
        if (had_existing) {
            std::error_code rollback_ec;
            fs::rename(backup, final_path, rollback_ec);
        }
        throw std::runtime_error("cannot publish completed image: " + final_path.string());
    }
    if (had_existing) { ec.clear(); fs::remove(backup, ec); }
}

void finalize_encoded_cache_file(const fs::path& temporary, const fs::path& cached, bool replace_existing,
                                 const CacheMetadata& metadata) {
    std::error_code ec;
    if (!fs::is_regular_file(temporary, ec) || ec)
        throw std::runtime_error("encoder returned success but did not create cache output: " + temporary.string());
    const auto temporary_size = fs::file_size(temporary, ec);
    if (ec || temporary_size == 0)
        throw std::runtime_error("encoder returned success but created an empty cache output: " + temporary.string());

    fs::create_directories(cached.parent_path(), ec);
    if (ec) throw std::runtime_error("cannot create encode cache directory: " + cached.parent_path().string());

    // A concurrent successful writer may have published the same content while
    // this encoder was running.  If replacement was not requested, prefer the
    // already-complete final object and discard this private temporary later.
    if (!replace_existing && cache_file_usable(cached)) { touch_cache_metadata(cached, metadata); return; }

    // The encoder wrote the temporary object beneath cached.parent_path(), so
    // this publication is always a same-filesystem rename.  No copy fallback is
    // permitted: an interrupted/nonzero encode can therefore never expose a
    // partial file under the final content-addressed cache name.
    if (fs::exists(cached, ec)) {
        ec.clear();
        fs::remove(cached, ec);
        if (ec) throw std::runtime_error("cannot replace encoded cache entry: " + cached.string());
    }
    ec.clear();
    fs::rename(temporary, cached, ec);
    if (ec) throw std::runtime_error("cannot atomically publish encoded cache entry: " + cached.string());
    if (!cache_file_usable(cached))
        throw std::runtime_error("published encoded cache entry failed validation: " + cached.string());
    touch_cache_metadata(cached, metadata);
}


fs::path executable_directory() {
#if defined(_WIN32)
    std::array<wchar_t, 32768> buf{};
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0 || n >= buf.size()) return {};
    return fs::path(std::wstring(buf.data(), n)).parent_path();
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    (void)_NSGetExecutablePath(nullptr, &size);
    if (size == 0) return {};
    std::vector<char> buf(size);
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return {};
    std::error_code ec;
    auto p = fs::weakly_canonical(fs::path(buf.data()), ec);
    if (ec) p = fs::path(buf.data());
    return p.parent_path();
#elif defined(__linux__)
    std::array<char, 4096> buf{};
    const auto n = ::readlink("/proc/self/exe", buf.data(), buf.size() - 1);
    if (n <= 0) return {};
    return fs::path(std::string(buf.data(), static_cast<std::size_t>(n))).parent_path();
#else
    return {};
#endif
}

std::string resolve_sibling_default_tool(std::string configured, const char* command) {
    fs::path configured_path(configured);
    if (configured_path.has_parent_path()) return configured;
    std::string name = configured_path.filename().string();
#if defined(_WIN32)
    auto lower=[](std::string v){
        std::transform(v.begin(),v.end(),v.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
        return v;
    };
    auto configured_lower=lower(name);
    auto command_lower=lower(command);
    if (configured_lower.size()>4U && configured_lower.ends_with(".exe")) configured_lower.resize(configured_lower.size()-4U);
    if (configured_lower!=command_lower) return configured;
#else
    if (name != command) return configured;
#endif
    const auto dir=executable_directory();
    if (dir.empty()) return configured;
#if defined(_WIN32)
    const auto candidate=dir/(std::string(command)+".exe");
#else
    const auto candidate=dir/command;
#endif
    std::error_code ec;
    if (fs::is_regular_file(candidate,ec) && !ec) return candidate.string();
    return configured;
}

std::string tsmuxer_command(const ToolPaths& tools) {
    if (!tools.tsmuxer.empty()) return tools.tsmuxer;
    auto dir = executable_directory();
    if (!dir.empty()) {
#if defined(_WIN32)
        auto candidate = dir / "tsmuxer.exe";
#else
        auto candidate = dir / "tsmuxer";
#endif
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec) && !ec) return candidate.string();
    }
#if defined(_WIN32)
    return "tsmuxer.exe";
#else
    return "tsmuxer";
#endif
}

std::string command_status(int rc) {
#if !defined(_WIN32)
    if (rc == -1) return "could not start";
    if (WIFEXITED(rc)) return "exit " + std::to_string(WEXITSTATUS(rc));
    if (WIFSIGNALED(rc)) return "signal " + std::to_string(WTERMSIG(rc));
    if (WIFSTOPPED(rc)) return "stopped by signal " + std::to_string(WSTOPSIG(rc));
#endif
    return "status " + std::to_string(rc);
}

std::string fail_on_interrupt_command(const std::string& c) {
#if defined(_WIN32)
    return c;
#else
    // The POSIX launcher gives helpers a fresh signal mask/defaults and their
    // own process group. Keep explicit shell traps as a second line of defense
    // so Ctrl-C, TERM, or a lost terminal cannot turn an interrupted encoder
    // into a successful cache publish merely because it flushed a partial file.
    return "trap 'exit 130' INT TERM HUP; " + c;
#endif
}

#if defined(_WIN32)
std::wstring windows_widen(const std::string& text) {
    if (text.empty()) return {};
    auto convert = [&](UINT code_page, DWORD flags) -> std::wstring {
        const int needed = MultiByteToWideChar(code_page, flags, text.data(), static_cast<int>(text.size()), nullptr, 0);
        if (needed <= 0) return {};
        std::wstring out(static_cast<std::size_t>(needed), L'\0');
        if (MultiByteToWideChar(code_page, flags, text.data(), static_cast<int>(text.size()), out.data(), needed) != needed)
            return {};
        return out;
    };
    auto out = convert(CP_UTF8, MB_ERR_INVALID_CHARS);
    if (!out.empty()) return out;
    return convert(CP_ACP, 0);
}

std::wstring windows_command_processor() {
    std::array<wchar_t, 32768> buffer{};
    const DWORD count = GetEnvironmentVariableW(L"COMSPEC", buffer.data(), static_cast<DWORD>(buffer.size()));
    if (count > 0 && count < buffer.size()) return std::wstring(buffer.data(), count);
    std::array<wchar_t, MAX_PATH + 1> system_dir{};
    const UINT system_count = GetSystemDirectoryW(system_dir.data(), static_cast<UINT>(system_dir.size()));
    if (system_count > 0 && system_count < system_dir.size())
        return std::wstring(system_dir.data(), system_count) + L"\\cmd.exe";
    return L"cmd.exe";
}

std::vector<wchar_t> windows_shell_command_line(const std::wstring& command_processor,
                                                const std::string& command) {
    // /S /C needs the extra outer pair when the command itself begins with a
    // quoted Program Files executable and also contains redirection/pipes.
    const auto wide_command = windows_widen(command);
    if (wide_command.empty() && !command.empty()) return {};
    std::wstring line = L"\"" + command_processor + L"\" /D /S /C \"" + wide_command + L"\"";
    std::vector<wchar_t> mutable_line(line.begin(), line.end());
    mutable_line.push_back(L'\0');
    return mutable_line;
}

HANDLE windows_inheritable_standard_handle(DWORD which, DWORD fallback_access) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    const HANDLE original = GetStdHandle(which);
    if (original && original != INVALID_HANDLE_VALUE) {
        HANDLE duplicate = nullptr;
        if (DuplicateHandle(GetCurrentProcess(), original, GetCurrentProcess(), &duplicate,
                            0, TRUE, DUPLICATE_SAME_ACCESS))
            return duplicate;
    }
    return CreateFileW(L"NUL", fallback_access, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
}

struct WindowsChildProcess {
    HANDLE process = nullptr;
    HANDLE job = nullptr;
};

void windows_close_child(WindowsChildProcess& child) {
    if (child.process) CloseHandle(child.process);
    if (child.job) CloseHandle(child.job);
    child.process = nullptr;
    child.job = nullptr;
}

void windows_terminate_child_tree(WindowsChildProcess& child, UINT exit_code = ERROR_CANCELLED) {
    if (child.job) {
        (void)TerminateJobObject(child.job, exit_code);
    } else if (child.process) {
        (void)TerminateProcess(child.process, exit_code);
    }
}

bool windows_start_hidden_shell(const std::string& command, HANDLE stdout_handle,
                                WindowsChildProcess* process_out) {
    const auto command_processor = windows_command_processor();
    auto command_line = windows_shell_command_line(command_processor, command);
    if (command_line.empty()) return false;

    HANDLE stdin_handle = windows_inheritable_standard_handle(STD_INPUT_HANDLE, GENERIC_READ);
    HANDLE stderr_handle = windows_inheritable_standard_handle(STD_ERROR_HANDLE, GENERIC_WRITE);
    HANDLE owned_stdout = nullptr;
    if (!stdout_handle) {
        owned_stdout = windows_inheritable_standard_handle(STD_OUTPUT_HANDLE, GENERIC_WRITE);
        stdout_handle = owned_stdout;
    }
    if (!stdin_handle || stdin_handle == INVALID_HANDLE_VALUE ||
        !stdout_handle || stdout_handle == INVALID_HANDLE_VALUE ||
        !stderr_handle || stderr_handle == INVALID_HANDLE_VALUE) {
        if (stdin_handle && stdin_handle != INVALID_HANDLE_VALUE) CloseHandle(stdin_handle);
        if (owned_stdout && owned_stdout != INVALID_HANDLE_VALUE) CloseHandle(owned_stdout);
        if (stderr_handle && stderr_handle != INVALID_HANDLE_VALUE) CloseHandle(stderr_handle);
        return false;
    }

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) {
        CloseHandle(stdin_handle);
        if (owned_stdout) CloseHandle(owned_stdout);
        CloseHandle(stderr_handle);
        return false;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_info{};
    job_info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &job_info, sizeof(job_info))) {
        CloseHandle(job);
        CloseHandle(stdin_handle);
        if (owned_stdout) CloseHandle(owned_stdout);
        CloseHandle(stderr_handle);
        return false;
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdInput = stdin_handle;
    startup.hStdOutput = stdout_handle;
    startup.hStdError = stderr_handle;
    PROCESS_INFORMATION process{};
    const BOOL started = CreateProcessW(command_processor.c_str(), command_line.data(), nullptr, nullptr,
                                        TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &startup, &process);
    CloseHandle(stdin_handle);
    if (owned_stdout) CloseHandle(owned_stdout);
    CloseHandle(stderr_handle);
    if (!started) {
        CloseHandle(job);
        return false;
    }
    if (!AssignProcessToJobObject(job, process.hProcess)) {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, INFINITE);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        CloseHandle(job);
        return false;
    }
    if (ResumeThread(process.hThread) == static_cast<DWORD>(-1)) {
        TerminateJobObject(job, 1);
        WaitForSingleObject(process.hProcess, INFINITE);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        CloseHandle(job);
        return false;
    }
    CloseHandle(process.hThread);
    process_out->process = process.hProcess;
    process_out->job = job;
    return true;
}

std::mutex windows_pipe_mutex;
std::unordered_map<FILE*, WindowsChildProcess> windows_pipe_processes;
#endif

int shell_system(const std::string& command) {
    authoring_cancellation_point();
#if defined(_WIN32)
    WindowsChildProcess child;
    if (!windows_start_hidden_shell(command, nullptr, &child)) return -1;
    for (;;) {
        const DWORD wait = WaitForSingleObject(child.process, 100);
        if (wait == WAIT_OBJECT_0) {
            DWORD exit_code = static_cast<DWORD>(-1);
            (void)GetExitCodeProcess(child.process, &exit_code);
            windows_close_child(child);
            authoring_cancellation_point();
            return static_cast<int>(exit_code);
        }
        if (wait != WAIT_TIMEOUT) {
            windows_close_child(child);
            return -1;
        }
        if (authoring_cancel_requested()) {
            windows_terminate_child_tree(child);
            (void)WaitForSingleObject(child.process, INFINITE);
            windows_close_child(child);
            throw AuthorCancelled();
        }
    }
#else
    // Authoring runs from a worker thread in the Qt GUI. Calling fork() from a
    // multithreaded process can leave the child with libc/runtime locks that
    // were held by vanished sibling threads, causing an apparent encoder hang
    // before /bin/sh or FFmpeg ever starts. posix_spawn() avoids that unsafe
    // post-fork window while still letting us create a cancellable process group.
    posix_spawnattr_t attr{};
    if (posix_spawnattr_init(&attr) != 0) return -1;
    struct SpawnAttrCleanup {
        posix_spawnattr_t* attr;
        ~SpawnAttrCleanup() { (void)posix_spawnattr_destroy(attr); }
    } attr_cleanup{&attr};

    // The helper gets its own process group so cancellation can kill the whole
    // command tree.  A process group created from a terminal-launched GUI is no
    // longer the terminal foreground group, however, and terminal-aware tools
    // such as FFmpeg may try to configure/read stdin for interactive commands.
    // The kernel then stops that background process group with SIGTTIN/SIGTTOU,
    // which used to look like an encoder hang because waitpid(WNOHANG) did not
    // report stopped children.  Authoring helpers are non-interactive, so detach
    // their stdin from the controlling terminal before spawning the shell.
    posix_spawn_file_actions_t actions{};
    if (posix_spawn_file_actions_init(&actions) != 0) return -1;
    struct SpawnActionsCleanup {
        posix_spawn_file_actions_t* actions;
        ~SpawnActionsCleanup() { (void)posix_spawn_file_actions_destroy(actions); }
    } actions_cleanup{&actions};
    if (posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0) != 0)
        return -1;

    sigset_t empty_mask{};
    sigemptyset(&empty_mask);
    sigset_t default_signals{};
    sigemptyset(&default_signals);
    sigaddset(&default_signals, SIGHUP);
    sigaddset(&default_signals, SIGINT);
    sigaddset(&default_signals, SIGTERM);
    sigaddset(&default_signals, SIGPIPE);

    short flags = POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
    if (posix_spawnattr_setflags(&attr, flags) != 0 ||
        posix_spawnattr_setpgroup(&attr, 0) != 0 ||
        posix_spawnattr_setsigmask(&attr, &empty_mask) != 0 ||
        posix_spawnattr_setsigdefault(&attr, &default_signals) != 0)
        return -1;

    char* const argv[] = {
        const_cast<char*>("sh"),
        const_cast<char*>("-c"),
        const_cast<char*>(command.c_str()),
        nullptr
    };
    pid_t pid = -1;
    if (posix_spawn(&pid, "/bin/sh", &actions, &attr, argv, environ) != 0) return -1;

    int status = 0;
    for (;;) {
        const pid_t waited = waitpid(pid, &status, WNOHANG | WUNTRACED);
        if (waited == pid) {
            if (WIFSTOPPED(status)) {
                // A helper should never need terminal job control.  Do not spin
                // forever if a future regression reconnects it to a controlling
                // TTY or otherwise stops the process group.
                const int stopped_status = status;
                (void)kill(-pid, SIGKILL);
                while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
                return stopped_status;
            }
            authoring_cancellation_point();
            return status;
        }
        if (waited < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (authoring_cancel_requested()) {
            (void)kill(-pid, SIGTERM);
            for (int attempt = 0; attempt < 20; ++attempt) {
                const pid_t stopped = waitpid(pid, &status, WNOHANG);
                if (stopped == pid) throw AuthorCancelled();
                if (stopped < 0 && errno != EINTR) throw AuthorCancelled();
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            }
            (void)kill(-pid, SIGKILL);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            throw AuthorCancelled();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
#endif
}

FILE* shell_popen(const std::string& command, const char* mode) {
    authoring_cancellation_point();
#if defined(_WIN32)
    if (!mode || (std::strcmp(mode, "r") != 0 && std::strcmp(mode, "rb") != 0)) return nullptr;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE read_handle = nullptr;
    HANDLE write_handle = nullptr;
    if (!CreatePipe(&read_handle, &write_handle, &sa, 0)) return nullptr;
    if (!SetHandleInformation(read_handle, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(read_handle);
        CloseHandle(write_handle);
        return nullptr;
    }
    WindowsChildProcess child;
    const bool started = windows_start_hidden_shell(command, write_handle, &child);
    CloseHandle(write_handle);
    if (!started) {
        CloseHandle(read_handle);
        return nullptr;
    }
    const int flags = _O_RDONLY | (std::strchr(mode, 'b') ? _O_BINARY : _O_TEXT);
    const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(read_handle), flags);
    if (fd == -1) {
        CloseHandle(read_handle);
        windows_terminate_child_tree(child, 1);
        WaitForSingleObject(child.process, INFINITE);
        windows_close_child(child);
        return nullptr;
    }
    FILE* pipe = _fdopen(fd, mode);
    if (!pipe) {
        _close(fd);
        windows_terminate_child_tree(child, 1);
        WaitForSingleObject(child.process, INFINITE);
        windows_close_child(child);
        return nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(windows_pipe_mutex);
        windows_pipe_processes.emplace(pipe, child);
    }
    return pipe;
#else
    return popen(command.c_str(), mode);
#endif
}

int shell_pclose(FILE* pipe) {
#if defined(_WIN32)
    WindowsChildProcess child;
    {
        std::lock_guard<std::mutex> lock(windows_pipe_mutex);
        const auto it = windows_pipe_processes.find(pipe);
        if (it == windows_pipe_processes.end()) return -1;
        child = it->second;
        windows_pipe_processes.erase(it);
    }
    const int close_rc = std::fclose(pipe);
    for (;;) {
        const DWORD wait = WaitForSingleObject(child.process, 100);
        if (wait == WAIT_OBJECT_0) {
            DWORD exit_code = static_cast<DWORD>(-1);
            (void)GetExitCodeProcess(child.process, &exit_code);
            windows_close_child(child);
            authoring_cancellation_point();
            if (close_rc != 0) return -1;
            return static_cast<int>(exit_code);
        }
        if (wait != WAIT_TIMEOUT) {
            windows_close_child(child);
            return -1;
        }
        if (authoring_cancel_requested()) {
            windows_terminate_child_tree(child);
            (void)WaitForSingleObject(child.process, INFINITE);
            windows_close_child(child);
            throw AuthorCancelled();
        }
    }
#else
    const int rc = pclose(pipe);
    authoring_cancellation_point();
    return rc;
#endif
}

void run(const std::string& c) {
    const auto guarded = fail_on_interrupt_command(c);
    const int rc = shell_system(guarded);
    if (rc != 0) throw std::runtime_error("command failed (" + command_status(rc) + "): " + c);
}

std::string read_small_text(const fs::path& p) {
    std::ifstream f(p);
    if (!f) return {};
    std::ostringstream s;
    s << f.rdbuf();
    auto x = s.str();
    constexpr std::size_t Max = 16384;
    if (x.size() > Max) x = "...\n" + x.substr(x.size() - Max);
    return x;
}

void write_text(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p);
    if (!f) throw std::runtime_error("cannot write " + p.string());
    f << s;
}

void copy_one(const fs::path& src, const fs::path& dst) {
    if (!fs::exists(src)) throw std::runtime_error("tsMuxeR did not produce " + src.string());
    fs::create_directories(dst.parent_path());
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing);
}

std::string bd_number(unsigned n) {
    std::ostringstream name;
    name << std::setw(5) << std::setfill('0') << n;
    return name.str();
}

void copy_playlist_clips(const fs::path& from, const fs::path& to, unsigned playlist,
                         std::initializer_list<unsigned> clips) {
    for (const auto clip : clips) {
        const auto x = bd_number(clip);
        copy_one(from / "BDMV/STREAM" / (x + ".m2ts"), to / "BDMV/STREAM" / (x + ".m2ts"));
        copy_one(from / "BDMV/CLIPINF" / (x + ".clpi"), to / "BDMV/CLIPINF" / (x + ".clpi"));
    }
    const auto p = bd_number(playlist);
    copy_one(from / "BDMV/PLAYLIST" / (p + ".mpls"), to / "BDMV/PLAYLIST" / (p + ".mpls"));
}

void backup_stream_metadata(const fs::path& root, unsigned playlist, std::initializer_list<unsigned> clips) {
    fs::create_directories(root / "BDMV/BACKUP/CLIPINF");
    fs::create_directories(root / "BDMV/BACKUP/PLAYLIST");
    for (const auto clip : clips) {
        const auto x = bd_number(clip);
        copy_one(root / "BDMV/CLIPINF" / (x + ".clpi"), root / "BDMV/BACKUP/CLIPINF" / (x + ".clpi"));
    }
    const auto p = bd_number(playlist);
    copy_one(root / "BDMV/PLAYLIST" / (p + ".mpls"), root / "BDMV/BACKUP/PLAYLIST" / (p + ".mpls"));
}

std::string trim_copy(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

struct SourceTrackDescriptor {
    int source_ordinal = 0; // 0-based ordinal within the source media type; -1 for attached files
    std::string codec_name;
    std::string language = "und";
    fs::path external_source;
    SubtitleStyle subtitle_style;
};

struct SourceTrackCatalog {
    std::vector<SourceTrackDescriptor> audio;
    std::vector<SourceTrackDescriptor> subtitles;
};

std::string normalized_language(std::string language, std::string fallback = "und") {
    language = trim_copy(std::move(language));
    std::transform(language.begin(), language.end(), language.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (language.size() == 3U && std::all_of(language.begin(), language.end(), [](unsigned char c) {
            return std::isalpha(c) != 0;
        })) return language;
    return fallback;
}

std::string effective_audio_language(const Title& title, const SourceTrackDescriptor& track, std::size_t authored_index) {
    if (const auto* settings = find_audio_stream_settings(title, track.source_ordinal); settings && !settings->language.empty())
        return normalized_language(settings->language);
    if (track.language != "und") return track.language;
    return authored_index == 0U ? normalized_language(title.audio_language, "eng") : std::string("und");
}

std::string effective_subtitle_language(const Title& title, const SourceTrackDescriptor& track) {
    if (const auto* settings = find_subtitle_stream_settings(title, track.source_ordinal); settings && !settings->language.empty())
        return normalized_language(settings->language);
    return track.language;
}

bool supported_subtitle_codec(const std::string& codec) {
    static constexpr std::array<const char*, 7> text_codecs = {
        "subrip", "srt", "ass", "ssa", "webvtt", "text", "mov_text"
    };
    return codec == "hdmv_pgs_subtitle" || codec == "external_text" ||
           std::any_of(text_codecs.begin(), text_codecs.end(), [&](const char* c) { return codec == c; });
}

std::string null_device();
bool run_probe(const std::string& command);

SourceTrackCatalog probe_source_track_catalog(const ToolPaths& tools, const fs::path& source, const fs::path& out) {
    fs::create_directories(out.parent_path());
    const fs::path err = fs::path(out.string() + ".stderr");
    std::error_code ec;
    fs::remove(err, ec);
    const std::string command = shq_s(tools.ffprobe) +
        " -v error -show_entries stream=codec_type,codec_name:stream_tags=language "
        "-of compact=p=0:nk=0 " + shq(source) + " > " + shq(out) + " 2> " + shq(err);
    const int rc = shell_system(command);
    if (rc != 0) {
        const auto detail = read_small_text(err);
        throw std::runtime_error("ffprobe could not inspect title streams (" + command_status(rc) + "): " +
                                 source.string() +
                                 (detail.empty() ? std::string() : "\n\n--- ffprobe output ---\n" + detail));
    }
    fs::remove(err, ec);
    std::ifstream f(out);
    if (!f) throw std::runtime_error("cannot read ffprobe title stream list: " + out.string());
    SourceTrackCatalog catalog;
    int audio_ordinal = 0;
    int subtitle_ordinal = 0;
    for (std::string line; std::getline(f, line);) {
        std::string type, codec, language;
        std::size_t start = 0;
        while (start <= line.size()) {
            const auto end = line.find('|', start);
            const auto token = line.substr(start, end == std::string::npos ? std::string::npos : end - start);
            const auto eq = token.find('=');
            if (eq != std::string::npos) {
                const auto key = token.substr(0, eq);
                const auto value = token.substr(eq + 1);
                if (key == "codec_type") type = value;
                else if (key == "codec_name") codec = value;
                else if (key == "tag:language" || key == "TAG:language") language = value;
            }
            if (end == std::string::npos) break;
            start = end + 1;
        }
        if (type == "audio") {
            catalog.audio.push_back({audio_ordinal++, codec, normalized_language(language), {}, {}});
        } else if (type == "subtitle") {
            SourceTrackDescriptor track{subtitle_ordinal++, codec, normalized_language(language), {}, {}};
            if (supported_subtitle_codec(codec)) catalog.subtitles.push_back(std::move(track));
        }
    }
    return catalog;
}

std::optional<double> number_value(const std::string& s) {
    if (s.empty() || s == "N/A") return std::nullopt;
    try {
        std::size_t used = 0;
        const double value = std::stod(s, &used);
        if (used == 0 || !std::isfinite(value)) return std::nullopt;
        return value;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<long long> integer_value(const std::string& s) {
    const auto n = number_value(s);
    if (!n) return std::nullopt;
    return static_cast<long long>(*n);
}

double rational_value(const std::string& s) {
    auto sep = s.find('/');
    if (sep == std::string::npos) sep = s.find(':');
    if (sep == std::string::npos) return number_value(s).value_or(0.0);
    const auto a = number_value(s.substr(0, sep));
    const auto b = number_value(s.substr(sep + 1));
    if (!a || !b || *b == 0.0) return 0.0;
    return *a / *b;
}

std::unordered_map<std::string, std::string> read_key_values(const fs::path& path) {
    std::unordered_map<std::string, std::string> values;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        values[trim_copy(line.substr(0, eq))] = trim_copy(line.substr(eq + 1));
    }
    return values;
}


bool timing_rate_near(double a, double b, double epsilon = 0.035) {
    return std::isfinite(a) && std::abs(a - b) <= epsilon;
}

bool timing_source_interlaced(const VideoStreamInfo& i) {
    return i.field_order == "tt" || i.field_order == "bb" || i.field_order == "tb" || i.field_order == "bt";
}

struct TimingCandidate {
    const char* frame_rate;
    double frame_rate_value;
    bool interlaced;
    const char* temporal_rate;
    double temporal_rate_value;
    const char* tsmuxer_fps;
    const char* description;
    bool fake_interlaced = false;
};

constexpr std::array<TimingCandidate,4> kBluRay1080TimingModes{{
    {"24000/1001", 24000.0/1001.0, false, "24000/1001", 24000.0/1001.0, "23.976", "1080p23.976"},
    {"24",          24.0,             false, "24",          24.0,             "24",     "1080p24"},
    {"25",          25.0,             true,  "50",          50.0,             "25",     "1080i50 (25 frames/s, 50 fields/s)"},
    {"30000/1001",  30000.0/1001.0,  true,  "60000/1001",  60000.0/1001.0,  "29.97",  "1080i59.94 (29.97 frames/s, 59.94 fields/s)"}
}};

// x264 can encode progressive 25/29.97 pictures while signaling a PAFF-capable
// interlaced sequence.  Blu-ray players therefore see the legal 1080i50/i59.94
// signaling while every coded picture remains progressive.  Keep these modes
// AVC-only; MPEG-2 continues to use the actual interlaced modes above.
constexpr std::array<TimingCandidate,6> kBluRay1080AvcTimingModes{{
    kBluRay1080TimingModes[0],
    kBluRay1080TimingModes[1],
    kBluRay1080TimingModes[2],
    kBluRay1080TimingModes[3],
    {"25",          25.0,             false, "25",          25.0,             "25",     "1080p25 (x264 fake interlaced)", true},
    {"30000/1001",  30000.0/1001.0,  false, "30000/1001",  30000.0/1001.0,  "29.97",  "1080p29.97 (x264 fake interlaced)", true}
}};

constexpr std::array<TimingCandidate,4> kBluRay720TimingModes{{
    {"24000/1001", 24000.0/1001.0, false, "24000/1001", 24000.0/1001.0, "23.976", "720p23.976"},
    {"24",          24.0,             false, "24",          24.0,             "24",     "720p24"},
    {"50",          50.0,             false, "50",          50.0,             "50",     "720p50"},
    {"60000/1001",  60000.0/1001.0,  false, "60000/1001",  60000.0/1001.0,  "59.94",  "720p59.94"}
}};
constexpr std::array<TimingCandidate,1> kBluRay480TimingModes{{
    {"30000/1001", 30000.0/1001.0, true, "60000/1001", 60000.0/1001.0, "29.97", "480i59.94"}
}};
constexpr std::array<TimingCandidate,1> kBluRay576TimingModes{{
    {"25", 25.0, true, "50", 50.0, "25", "576i50"}
}};

constexpr std::array<TimingCandidate,6> kUhdTimingModes{{
    {"24000/1001", 24000.0/1001.0, false, "24000/1001", 24000.0/1001.0, "23.976", "2160p23.976"},
    {"24",          24.0,             false, "24",          24.0,             "24",     "2160p24"},
    {"25",          25.0,             false, "25",          25.0,             "25",     "2160p25"},
    {"50",          50.0,             false, "50",          50.0,             "50",     "2160p50"},
    {"60000/1001",  60000.0/1001.0,  false, "60000/1001",  60000.0/1001.0,  "59.94",  "2160p59.94"},
    {"60",          60.0,             false, "60",          60.0,             "60",     "2160p60"}
}};

constexpr std::array<TimingCandidate,3> kDvdTimingModes{{
    {"24000/1001", 24000.0/1001.0, false, "24000/1001", 24000.0/1001.0, "23.976", "DVD-Video Film 480p23.976"},
    {"25",          25.0,             true,  "50",          50.0,             "25",     "DVD-Video PAL 576i50"},
    {"30000/1001", 30000.0/1001.0,  true,  "60000/1001",  60000.0/1001.0,  "29.97",  "DVD-Video NTSC 480i59.94"}
}};

template <std::size_t N>
VideoTimingMode automatic_timing_from_candidates(const std::array<TimingCandidate,N>& candidates,
                                                 double source_frame_rate, bool source_interlaced) {
    double source_cadence = source_frame_rate;
    if (source_interlaced) source_cadence *= 2.0;
    if (!std::isfinite(source_cadence) || source_cadence <= 0.0) source_cadence = 24000.0/1001.0;

    const TimingCandidate* best = &candidates.front();
    double best_score = 1e100;
    for (const auto& c : candidates) {
        const double out = c.temporal_rate_value;
        double score = 0.0;
        if (timing_rate_near(source_cadence, out)) {
            score = std::abs(source_cadence - out) / std::max(1.0, source_cadence);
        } else {
            const double up_ratio = out / source_cadence;
            const double up_integer = std::round(up_ratio);
            const double down_ratio = source_cadence / out;
            const double down_integer = std::round(down_ratio);
            const bool regular_duplicate = up_integer >= 2.0 && up_integer <= 5.0 &&
                std::abs(up_ratio - up_integer) <= 0.004 * up_integer;
            const bool regular_drop = down_integer >= 2.0 && down_integer <= 8.0 &&
                std::abs(down_ratio - down_integer) <= 0.004 * down_integer;
            if (regular_duplicate) {
                // Repeating every source picture an integer number of times keeps
                // duration and cadence exact and loses no temporal samples.
                score = 0.015 * (up_integer - 1.0) + std::abs(up_ratio - up_integer);
            } else if (regular_drop) {
                // A regular N:1 decimation is preferable to an irregular cadence,
                // but rank it behind a nearby no-drop conversion.
                score = 0.08 * (down_integer - 1.0) + std::abs(down_ratio - down_integer);
            } else {
                const double relative = std::abs(out - source_cadence) / source_cadence;
                score = relative + (out < source_cadence ? 0.20 : 0.0);
            }
        }
        if (score < best_score) { best_score = score; best = &c; }
    }
    VideoTimingMode t;
    t.frame_rate = best->frame_rate;
    t.frame_rate_value = best->frame_rate_value;
    t.interlaced = best->interlaced;
    t.fake_interlaced = best->fake_interlaced;
    t.temporal_rate = best->temporal_rate;
    t.temporal_rate_value = best->temporal_rate_value;
    t.tsmuxer_fps = best->tsmuxer_fps;
    t.description = best->description;
    return t;
}

std::string normalize_rate_token(std::string value) {
    value = trim_copy(std::move(value));
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char c) { return std::isspace(c) != 0; }), value.end());
    return value;
}

template <std::size_t N>
std::optional<VideoTimingMode> explicit_timing_from_candidates(const std::array<TimingCandidate,N>& candidates,
                                                               const std::string& raw) {
    const std::string v = normalize_rate_token(raw);
    auto make = [](const TimingCandidate& c) {
        VideoTimingMode t; t.frame_rate=c.frame_rate; t.frame_rate_value=c.frame_rate_value; t.interlaced=c.interlaced;
        t.fake_interlaced=c.fake_interlaced; t.temporal_rate=c.temporal_rate; t.temporal_rate_value=c.temporal_rate_value; t.tsmuxer_fps=c.tsmuxer_fps;
        t.description=c.description; return t;
    };
    for (const auto& c : candidates) {
        if (v == c.frame_rate || v == c.tsmuxer_fps) return make(c);
    }

    // Resolve human-friendly aliases by properties rather than fixed array
    // indexes.  The legal-mode tables have different sizes (Blu-ray has 4,
    // UHD has 6, DVD has 3), so indexed UHD assumptions here would make this
    // template out-of-bounds for the smaller instantiations.
    auto progressive = [&](double rate) -> std::optional<VideoTimingMode> {
        for (const auto& c : candidates)
            if (!c.interlaced && timing_rate_near(c.frame_rate_value, rate)) return make(c);
        return std::nullopt;
    };
    auto interlaced = [&](double temporal_rate) -> std::optional<VideoTimingMode> {
        for (const auto& c : candidates)
            if (c.interlaced && timing_rate_near(c.temporal_rate_value, temporal_rate)) return make(c);
        return std::nullopt;
    };

    if (v == "23.976p" || v == "23.98p") return progressive(24000.0/1001.0);
    if (v == "24p") return progressive(24.0);
    if (v == "25p") return progressive(25.0);
    if (v == "29.97p" || v == "30000/1001p") return progressive(30000.0/1001.0);
    if (v == "50p") return progressive(50.0);
    if (v == "59.94p" || v == "60000/1001p") return progressive(60000.0/1001.0);
    if (v == "60p") return progressive(60.0);
    if (v == "50i" || v == "25i") return interlaced(50.0);
    if (v == "59.94i" || v == "29.97i" || v == "60000/1001i") return interlaced(60000.0/1001.0);
    return std::nullopt;
}

std::string normalize_frame_rate_for_target(DiscTarget target, std::string value) {
    value = normalize_rate_token(std::move(value));
    if (target != DiscTarget::DvdVideo480p) return value;
    if (value == "film-dvd" || value == "dvd-film") return "23.976p";
    if (value == "ntsc-dvd" || value == "dvd-ntsc") return "59.94i";
    if (value == "pal-dvd" || value == "dvd-pal") return "50i";
    return value;
}


struct RasterCandidate { int width; int height; const char* aspect; };

std::string normalize_aspect_token(std::string value) {
    value = normalize_rate_token(std::move(value));
    if (value == "4x3" || value == "4/3") return "4:3";
    if (value == "16x9" || value == "16/9") return "16:9";
    return value;
}

std::string normalize_resolution_token(std::string value) {
    value = normalize_rate_token(std::move(value));
    for (std::size_t pos = value.find("×"); pos != std::string::npos; pos = value.find("×", pos + 1))
        value.replace(pos, std::string("×").size(), "x");
    return value;
}

std::vector<RasterCandidate> raster_candidates(DiscTarget target, std::optional<bool> dvd_pal = std::nullopt) {
    if (target == DiscTarget::UltraHdBluRay2160) return {
        {3840,2160,"16:9"},{1920,1080,"16:9"}
    };
    if (target == DiscTarget::BluRay1080) return {
        {1920,1080,"16:9"},{1440,1080,"16:9"},{1280,720,"16:9"},
        {720,576,"16:9"},{720,576,"4:3"},{720,480,"16:9"},{720,480,"4:3"}
    };
    const bool pal = dvd_pal.value_or(false);
    if (pal) return {
        {720,576,"16:9"},{720,576,"4:3"},{704,576,"4:3"},
        {352,576,"4:3"},{352,288,"4:3"}
    };
    return {
        {720,480,"16:9"},{720,480,"4:3"},{704,480,"4:3"},
        {352,480,"4:3"},{352,240,"4:3"}
    };
}

std::string sample_aspect_ratio_for_raster(int width, int height, const std::string& aspect) {
    if (width == 3840 || width == 1920 || width == 1280) return "1/1";
    if (width == 1440 && height == 1080) return "4/3"; // 1440x1080 is anamorphic 16:9 on BD.
    if (height == 480) {
        if (width == 720) return aspect == "4:3" ? "8/9" : "32/27";
        if (width == 704) return aspect == "4:3" ? "10/11" : "40/33";
        if (width == 352 && height == 480) return aspect == "4:3" ? "20/11" : "80/33";
    }
    if (width == 352 && height == 240) return aspect == "4:3" ? "10/11" : "40/33";
    if (height == 576) {
        if (width == 720) return aspect == "4:3" ? "16/15" : "64/45";
        if (width == 704) return aspect == "4:3" ? "12/11" : "16/11";
        if (width == 352 && height == 576) return aspect == "4:3" ? "24/11" : "32/11";
    }
    if (width == 352 && height == 288) return aspect == "4:3" ? "12/11" : "16/11";
    return "1/1";
}

TargetGeometry graphics_geometry_for_raster(DiscTarget target, int width, int height) {
    if (target == DiscTarget::UltraHdBluRay2160) return {width,height,1920,1080};
    if (target == DiscTarget::DvdVideo480p) return {width,height,width,height};
    if (height == 1080) return {width,height,1920,1080};
    if (width == 1280 && height == 720) return {width,height,1280,720};
    return {width,height,width,height};
}

template <std::size_t N>
bool explicit_rate_in(const std::array<TimingCandidate,N>& c, const std::string& value) {
    return explicit_timing_from_candidates(c, value).has_value();
}

bool raster_supports_rate(DiscTarget target, int width, int height, const std::string& requested_rate, VideoCodec codec) {
    const auto v = normalize_frame_rate_for_target(target, requested_rate);
    const bool automatic = v.empty() || v == "auto" || v == "inherit" || v == "default" || v == "project";
    if (target == DiscTarget::UltraHdBluRay2160 && codec == VideoCodec::X264) {
        if (width != 1920 || height != 1080) return false;
        if (automatic) return true;
        const std::array<TimingCandidate,2> avc{{kUhdTimingModes[0],kUhdTimingModes[1]}};
        return explicit_rate_in(avc,v);
    }
    if (automatic) return true;
    if (target == DiscTarget::UltraHdBluRay2160) {
        return explicit_rate_in(kUhdTimingModes, v);
    }
    if (target == DiscTarget::DvdVideo480p) {
        if (height == 576 || height == 288) return explicit_rate_in(std::array<TimingCandidate,1>{kDvdTimingModes[1]}, v);
        return explicit_rate_in(std::array<TimingCandidate,2>{kDvdTimingModes[0],kDvdTimingModes[2]}, v);
    }
    if (width == 1280 && height == 720) return explicit_rate_in(kBluRay720TimingModes, v);
    if (width == 720 && height == 480) return explicit_rate_in(kBluRay480TimingModes, v);
    if (width == 720 && height == 576) return explicit_rate_in(kBluRay576TimingModes, v);
    if (codec == VideoCodec::X264) return explicit_rate_in(kBluRay1080AvcTimingModes, v);
    return explicit_rate_in(kBluRay1080TimingModes, v);
}

VideoTimingMode timing_for_raster(DiscTarget target, int width, int height, const std::string& requested_rate,
                                  double source_rate, bool source_interlaced, VideoCodec codec) {
    const auto v = normalize_frame_rate_for_target(target, requested_rate);
    const bool automatic = v.empty() || v == "auto";
    if (target == DiscTarget::UltraHdBluRay2160) {
        if (codec == VideoCodec::X264) {
            const std::array<TimingCandidate,2> avc{{kUhdTimingModes[0],kUhdTimingModes[1]}};
            return automatic ? automatic_timing_from_candidates(avc,source_rate,source_interlaced)
                             : *explicit_timing_from_candidates(avc,v);
        }
        return automatic ? automatic_timing_from_candidates(kUhdTimingModes,source_rate,source_interlaced)
                         : *explicit_timing_from_candidates(kUhdTimingModes,v);
    }
    if (target == DiscTarget::DvdVideo480p) {
        if (height == 576 || height == 288) {
            const std::array<TimingCandidate,1> c{{kDvdTimingModes[1]}};
            if (automatic) return automatic_timing_from_candidates(c,source_rate,source_interlaced);
            if (auto x=explicit_timing_from_candidates(c,v)) return *x;
        } else {
            const std::array<TimingCandidate,2> c{{kDvdTimingModes[0],kDvdTimingModes[2]}};
            if (automatic) return automatic_timing_from_candidates(c,source_rate,source_interlaced);
            if (auto x=explicit_timing_from_candidates(c,v)) return *x;
        }
    } else if (width == 1280 && height == 720) {
        if (automatic) return automatic_timing_from_candidates(kBluRay720TimingModes,source_rate,source_interlaced);
        if (auto x=explicit_timing_from_candidates(kBluRay720TimingModes,v)) return *x;
    } else if (width == 720 && height == 480) {
        if (automatic) return automatic_timing_from_candidates(kBluRay480TimingModes,source_rate,source_interlaced);
        if (auto x=explicit_timing_from_candidates(kBluRay480TimingModes,v)) return *x;
    } else if (width == 720 && height == 576) {
        if (automatic) return automatic_timing_from_candidates(kBluRay576TimingModes,source_rate,source_interlaced);
        if (auto x=explicit_timing_from_candidates(kBluRay576TimingModes,v)) return *x;
    } else {
        if (codec == VideoCodec::X264) {
            if (automatic) return automatic_timing_from_candidates(kBluRay1080AvcTimingModes,source_rate,source_interlaced);
            if (auto x=explicit_timing_from_candidates(kBluRay1080AvcTimingModes,v)) return *x;
        } else {
            if (automatic) return automatic_timing_from_candidates(kBluRay1080TimingModes,source_rate,source_interlaced);
            if (auto x=explicit_timing_from_candidates(kBluRay1080TimingModes,v)) return *x;
        }
    }
    throw std::runtime_error("frame-rate override '" + requested_rate + "' is not legal for the selected resolution");
}

double source_display_aspect(const VideoStreamInfo& source) {
    if (source.width <= 0 || source.height <= 0) return 16.0/9.0;
    double sar = rational_value(source.sample_aspect_ratio);
    if (!std::isfinite(sar) || sar <= 0.0) sar = 1.0;
    return static_cast<double>(source.width) * sar / static_cast<double>(source.height);
}

bool source_prefers_four_three(const VideoStreamInfo& source) {
    const double dar = source_display_aspect(source);
    return std::abs(dar - 4.0/3.0) < std::abs(dar - 16.0/9.0);
}

std::optional<bool> dvd_family_from_resolution(const std::string& raw) {
    const auto r = normalize_resolution_token(raw);
    if (r.empty() || r == "auto" || r == "highest" || r == "inherit" || r == "default" || r == "project") return std::nullopt;
    if (r.ends_with("x576") || r.ends_with("x288")) return true;
    if (r.ends_with("x480") || r.ends_with("x240")) return false;
    return std::nullopt;
}

bool resolution_token_valid(DiscTarget target, const std::string& raw, bool menu) {
    const auto r = normalize_resolution_token(raw);
    if (r.empty() || r == "auto" || (menu && r == "highest")) return true;
    for (const auto& c : raster_candidates(target)) {
        if (r == std::to_string(c.width) + "x" + std::to_string(c.height)) return true;
    }
    if (target == DiscTarget::DvdVideo480p) for (const auto& c : raster_candidates(target,true))
        if (r == std::to_string(c.width)+"x"+std::to_string(c.height)) return true;
    return false;
}

bool aspect_token_valid(DiscTarget target, const std::string& raw, bool title) {
    const auto a = normalize_aspect_token(raw);
    if (title && (a.empty() || a == "auto")) return true;
    if (a == "16:9") return true;
    return a == "4:3" && target != DiscTarget::UltraHdBluRay2160;
}

bool static_video_mode_selection_valid(DiscTarget target, const std::string& requested_resolution,
                                       const std::string& requested_aspect, const std::string& requested_rate,
                                       bool menu, VideoCodec codec) {
    if (!resolution_token_valid(target, requested_resolution, menu) ||
        !aspect_token_valid(target, requested_aspect, !menu)) return false;
    const auto r = normalize_resolution_token(requested_resolution);
    const auto a = normalize_aspect_token(requested_aspect);
    auto any_candidate = [&](std::optional<bool> dvd_pal) {
        for (const auto& c : raster_candidates(target, dvd_pal)) {
            const auto cr = std::to_string(c.width) + "x" + std::to_string(c.height);
            if (!r.empty() && r != "auto" && r != "highest" && cr != r) continue;
            if (!a.empty() && a != "auto" && c.aspect != a) continue;
            if (!raster_supports_rate(target, c.width, c.height, requested_rate, codec)) continue;
            return true;
        }
        return false;
    };
    if (any_candidate(std::nullopt)) return true;
    return target == DiscTarget::DvdVideo480p && any_candidate(true);
}

RasterCandidate choose_raster(DiscTarget target, const std::string& requested_resolution, const std::string& requested_aspect,
                              const std::string& requested_rate, const VideoStreamInfo& source, bool menu, VideoCodec codec,
                              std::optional<bool> dvd_pal = std::nullopt) {
    const auto r = normalize_resolution_token(requested_resolution);
    const auto a = normalize_aspect_token(requested_aspect);
    if (!resolution_token_valid(target,requested_resolution,menu))
        throw std::runtime_error("resolution '"+requested_resolution+"' is not legal for the selected output target");
    if (!aspect_token_valid(target,requested_aspect,!menu))
        throw std::runtime_error("aspect ratio '"+requested_aspect+"' is not legal for the selected output target");
    auto candidates = raster_candidates(target,dvd_pal);
    std::vector<RasterCandidate> eligible;
    for (const auto& c : candidates) {
        const std::string cr=std::to_string(c.width)+"x"+std::to_string(c.height);
        if (!r.empty() && r!="auto" && r!="highest" && cr!=r) continue;
        if (!a.empty() && a!="auto" && c.aspect!=a) continue;
        if (!raster_supports_rate(target,c.width,c.height,requested_rate,codec)) continue;
        eligible.push_back(c);
    }
    if (eligible.empty())
        throw std::runtime_error("selected resolution/aspect/frame-rate combination is not legal for the output standard");
    if (menu || r == "highest") {
        return *std::max_element(eligible.begin(),eligible.end(),[](const auto& x,const auto& y){
            const long long ax=1LL*x.width*x.height, ay=1LL*y.width*y.height;
            return ax==ay ? x.width<y.width : ax<ay;
        });
    }
    if (r!="auto" && !r.empty()) {
        if (a=="auto" || a.empty()) {
            const bool want43=source_prefers_four_three(source);
            for (const auto& c:eligible) if ((std::string(c.aspect)=="4:3")==want43) return c;
        }
        return eligible.front();
    }
    const bool want43 = (a=="4:3") || ((a.empty()||a=="auto") && source_prefers_four_three(source));
    const double sw=source.width>0?source.width:1920, sh=source.height>0?source.height:1080;
    const RasterCandidate* best=&eligible.front();
    double best_score=1e100; bool best_down=true; long long best_area=0;
    for (const auto& c:eligible) {
        const bool mismatch=((std::string(c.aspect)=="4:3")!=want43);
        double score=(mismatch?10.0:0.0)+std::abs(c.width-sw)/sw+std::abs(c.height-sh)/sh;
        const bool down=c.width<sw || c.height<sh;
        const long long area=1LL*c.width*c.height;
        if (score < best_score-1e-9 || (std::abs(score-best_score)<=1e-9 && best_down && !down) ||
            (std::abs(score-best_score)<=1e-9 && best_down==down && area>best_area)) {
            best=&c;best_score=score;best_down=down;best_area=area;
        }
    }
    return *best;
}

VideoTimingMode resolve_output_mode(DiscTarget target, const std::string& resolution, const std::string& aspect,
                                    const std::string& rate, const VideoStreamInfo& source, bool menu, VideoCodec codec,
                                    std::optional<bool> dvd_pal = std::nullopt) {
    const auto raster=choose_raster(target,resolution,aspect,rate,source,menu,codec,dvd_pal);
    auto timing=timing_for_raster(target,raster.width,raster.height,rate,source.frame_rate,timing_source_interlaced(source),codec);
    const auto g=graphics_geometry_for_raster(target,raster.width,raster.height);
    timing.video_width=raster.width;timing.video_height=raster.height;timing.graphics_width=g.graphics_width;timing.graphics_height=g.graphics_height;
    timing.aspect_ratio=raster.aspect;timing.sample_aspect_ratio=sample_aspect_ratio_for_raster(raster.width,raster.height,raster.aspect);
    timing.description=std::to_string(raster.width)+"x"+std::to_string(raster.height)+" "+raster.aspect+" "+timing.description;
    return timing;
}

bool source_matches_output_mode(const VideoStreamInfo& source, const VideoTimingMode& timing) {
    if (source.width != timing.video_width || source.height != timing.video_height) return false;
    if (!timing_rate_near(source.frame_rate,timing.frame_rate_value)) return false;
    if (timing.fake_interlaced) {
        if (source.codec_name != "h264" || source.h264_frame_mbs_only_flag != 0 ||
            source.h264_mb_adaptive_frame_field_flag != 0) return false;
    } else if (timing_source_interlaced(source) != timing.interlaced) {
        return false;
    }
    const double source_sar = rational_value(source.sample_aspect_ratio);
    const double target_sar = rational_value(timing.sample_aspect_ratio);
    return source_sar > 0.0 && target_sar > 0.0 && std::abs(source_sar-target_sar) <= 0.01;
}

bool source_matches_requested_keyframe_interval(const VideoStreamInfo& source, const EncodingProfile& encoding,
                                                const VideoTimingMode& timing) {
    if (encoding.keyframe_interval_frames <= 0) return true;
    if (source.max_keyframe_interval_seconds <= 0.0) return false;
    const double requested_seconds = static_cast<double>(encoding.keyframe_interval_frames) /
                                     std::max(1.0, timing.frame_rate_value);
    // Packet timestamps are quantized and source encoders may place the access
    // point a fraction of a frame late; one output-frame tolerance avoids
    // needlessly re-encoding an otherwise matching GOP.
    const double tolerance = 1.0 / std::max(1.0, timing.frame_rate_value) + 0.001;
    return source.max_keyframe_interval_seconds <= requested_seconds + tolerance;
}

VideoTimingMode resolve_video_timing_for_rate(DiscTarget target, const std::string& requested_rate,
                                                  double source_frame_rate, bool source_interlaced) {
    const auto override = normalize_frame_rate_for_target(target, requested_rate);
    if (override != "auto" && !override.empty()) {
        std::optional<VideoTimingMode> fixed;
        if (target == DiscTarget::UltraHdBluRay2160) fixed = explicit_timing_from_candidates(kUhdTimingModes, override);
        else if (target == DiscTarget::DvdVideo480p) fixed = explicit_timing_from_candidates(kDvdTimingModes, override);
        else fixed = explicit_timing_from_candidates(kBluRay1080TimingModes, override);
        if (!fixed) throw std::runtime_error("frame-rate override '" + requested_rate + "' is not legal for the selected output target");
        fixed->description += " (explicit override)";
        return *fixed;
    }
    if (target == DiscTarget::UltraHdBluRay2160)
        return automatic_timing_from_candidates(std::array<TimingCandidate,3>{kUhdTimingModes[0],kUhdTimingModes[1],kUhdTimingModes[2]}, source_frame_rate, source_interlaced);
    if (target == DiscTarget::DvdVideo480p)
        return automatic_timing_from_candidates(kDvdTimingModes, source_frame_rate, source_interlaced);
    return automatic_timing_from_candidates(kBluRay1080TimingModes, source_frame_rate, source_interlaced);
}

std::string effective_title_frame_rate(const Project& p, const Title& title) {
    const auto& title_rate = frame_rate_for_target(title, p.target);
    const auto normalized = normalize_rate_token(title_rate);
    if (normalized.empty() || normalized == "inherit" || normalized == "default" || normalized == "project")
        return p.frame_rate;
    return title_rate;
}

bool frame_rate_override_valid(DiscTarget target, const std::string& value) {
    const auto normalized = normalize_frame_rate_for_target(target, value);
    if (normalized.empty() || normalized == "auto") return true;
    if (target == DiscTarget::UltraHdBluRay2160) return explicit_timing_from_candidates(kUhdTimingModes,normalized).has_value();
    if (target == DiscTarget::DvdVideo480p) return explicit_timing_from_candidates(kDvdTimingModes,normalized).has_value();
    return explicit_timing_from_candidates(kBluRay1080AvcTimingModes,normalized).has_value() ||
           explicit_timing_from_candidates(kBluRay720TimingModes,normalized).has_value() ||
           explicit_timing_from_candidates(kBluRay480TimingModes,normalized).has_value() ||
           explicit_timing_from_candidates(kBluRay576TimingModes,normalized).has_value();
}

bool title_frame_rate_override_valid(DiscTarget target, const std::string& value) {
    const auto normalized = normalize_rate_token(value);
    if (normalized.empty() || normalized == "inherit" || normalized == "default" || normalized == "project") return true;
    return frame_rate_override_valid(target, value);
}

std::optional<bool> explicit_dvd_pal_family(const std::string& requested_rate) {
    const auto normalized = normalize_frame_rate_for_target(DiscTarget::DvdVideo480p, requested_rate);
    if (normalized.empty() || normalized == "auto" || normalized == "inherit" || normalized == "default" || normalized == "project")
        return std::nullopt;
    const auto timing = explicit_timing_from_candidates(kDvdTimingModes, normalized);
    if (!timing) throw std::runtime_error("frame-rate override '" + requested_rate + "' is not legal for DVD-Video");
    return dvd_timing_is_pal(*timing);
}

double probed_frame_rate(const fs::path& source, const std::string& avg_text, const std::string& nominal_text) {
    const double avg = rational_value(avg_text);
    const double nominal = rational_value(nominal_text);
    std::string ext = source.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    // ffprobe cannot derive a reliable average rate from timestamps when the
    // input is a raw elementary stream.  It may report an arbitrary 25 fps
    // avg_frame_rate even while r_frame_rate correctly reflects the sequence
    // headers (for example 30000/1001 MPEG-2 or 60000/1001 HEVC).
    const bool raw_elementary = ext == ".264" || ext == ".h264" || ext == ".265" || ext == ".hevc" ||
                                ext == ".m2v" || ext == ".mpeg2" || ext == ".vc1";
    if (raw_elementary && nominal > 0.0) return nominal;
    if (avg > 0.0) return avg;
    return nominal;
}

VideoStreamInfo probe_video_timing_metadata(const ToolPaths& tools, const fs::path& source, const fs::path& out) {
    fs::create_directories(out.parent_path());
    const std::string command = shq_s(tools.ffprobe) +
        " -v error -select_streams v:0 -show_entries stream=codec_name,field_order,avg_frame_rate,r_frame_rate,width,height,sample_aspect_ratio "
        "-of default=nw=1:nk=0 " + shq(source) + " > " + shq(out) + " 2>" + null_device();
    if (!run_probe(command)) throw std::runtime_error("ffprobe could not inspect source frame rate: " + source.string());
    const auto values = read_key_values(out);
    auto text = [&](const char* key) -> std::string { const auto it=values.find(key); return it==values.end()?std::string():it->second; };
    VideoStreamInfo info;
    info.codec_name = text("codec_name");
    info.field_order = text("field_order");
    info.sample_aspect_ratio = text("sample_aspect_ratio");
    if (!parse_integer_exact(text("width"), info.width)) info.width = 0;
    if (!parse_integer_exact(text("height"), info.height)) info.height = 0;
    info.frame_rate = probed_frame_rate(source, text("avg_frame_rate"), text("r_frame_rate"));
    return info;
}

std::string null_device() {
#ifdef _WIN32
    return "NUL";
#else
    return "/dev/null";
#endif
}

bool run_probe(const std::string& command) {
    return shell_system(command) == 0;
}

struct SampleWindow {
    double start_seconds = 0.0;
    double length_seconds = 60.0;
};

std::vector<SampleWindow> compliance_sample_windows(double duration_seconds) {
    constexpr double kSampleSeconds = 60.0;
    if (!std::isfinite(duration_seconds) || duration_seconds <= 0.0)
        return {{0.0, kSampleSeconds}};
    if (duration_seconds <= kSampleSeconds * 3.0)
        return {{0.0, duration_seconds}};
    return {
        {0.0, kSampleSeconds},
        {std::max(0.0, duration_seconds * 0.5 - kSampleSeconds * 0.5), kSampleSeconds},
        {std::max(0.0, duration_seconds - kSampleSeconds), kSampleSeconds}
    };
}

std::string ffprobe_interval(const SampleWindow& w) {
    std::ostringstream s;
    s << std::fixed << std::setprecision(3) << w.start_seconds << "%+" << w.length_seconds;
    return s.str();
}

bool stream_command_lines(const std::string& command, const std::function<void(const std::string&)>& consume) {
    FILE* pipe = shell_popen(command, "r");
    if (!pipe) return false;
    std::array<char, 8192> buffer{};
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe))
        consume(std::string(buffer.data()));
#if defined(_WIN32)
    return shell_pclose(pipe) == 0;
#else
    const int status = shell_pclose(pipe);
    return status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
}

std::optional<std::uint64_t> count_command_bytes(const std::string& command) {
#if defined(_WIN32)
    FILE* pipe = shell_popen(command, "rb");
#else
    FILE* pipe = shell_popen(command, "r");
#endif
    if (!pipe) return std::nullopt;
    std::array<unsigned char, 65536> buffer{};
    std::uint64_t total = 0;
    for (;;) {
        const auto n = std::fread(buffer.data(), 1, buffer.size(), pipe);
        total += static_cast<std::uint64_t>(n);
        if (n < buffer.size()) {
            if (std::feof(pipe)) break;
            if (std::ferror(pipe)) {
                (void)shell_pclose(pipe);
                return std::nullopt;
            }
        }
    }
#if defined(_WIN32)
    if (shell_pclose(pipe) != 0) return std::nullopt;
#else
    const int status = shell_pclose(pipe);
    if (status == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) return std::nullopt;
#endif
    return total;
}

std::optional<std::vector<unsigned char>> capture_command_bytes(const std::string& command,
                                                                std::size_t max_bytes) {
#if defined(_WIN32)
    FILE* pipe = shell_popen(command, "rb");
#else
    FILE* pipe = shell_popen(command, "r");
#endif
    if (!pipe) return std::nullopt;
    std::vector<unsigned char> data;
    std::array<unsigned char, 65536> buffer{};
    bool overflow = false;
    for (;;) {
        const auto n = std::fread(buffer.data(), 1, buffer.size(), pipe);
        if (n > 0) {
            if (data.size() + n <= max_bytes)
                data.insert(data.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(n));
            else
                overflow = true;
        }
        if (n < buffer.size()) {
            if (std::feof(pipe)) break;
            if (std::ferror(pipe)) {
                (void)shell_pclose(pipe);
                return std::nullopt;
            }
        }
    }
#if defined(_WIN32)
    if (shell_pclose(pipe) != 0) return std::nullopt;
#else
    const int status = shell_pclose(pipe);
    if (status == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) return std::nullopt;
#endif
    if (overflow) return std::nullopt;
    return data;
}


std::optional<double> probe_dts_core_bitrate_sampled(const ToolPaths& tools, const fs::path& source,
                                                      int ordinal, double duration_seconds) {
    double maximum = 0.0;
    bool saw = false;
    for (const auto& sample : compliance_sample_windows(duration_seconds)) {
        std::ostringstream command;
        command << shq_s(tools.ffmpeg) << " -hide_banner -loglevel error -ss "
                << std::fixed << std::setprecision(3) << sample.start_seconds
                << " -i " << shq(source) << " -t 0.250 -map 0:a:" << ordinal
                << " -vn -sn -dn -c:a copy -f dts pipe:1 2>" << null_device();
        const auto bytes = capture_command_bytes(command.str(), 2U * 1024U * 1024U);
        if (!bytes) continue;
        const auto rate = detail::dts_core_actual_bitrate_from_bytes(*bytes);
        if (!rate) continue;
        maximum = std::max(maximum, *rate);
        saw = true;
    }
    return saw ? std::optional<double>(maximum) : std::nullopt;
}

std::unordered_map<std::string, std::string> parse_compact_fields(std::string line) {
    std::unordered_map<std::string, std::string> values;
    line = trim_copy(std::move(line));
    std::size_t begin = 0;
    while (begin <= line.size()) {
        const auto end = line.find('|', begin);
        const auto field = line.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        const auto eq = field.find('=');
        if (eq != std::string::npos) values[field.substr(0, eq)] = field.substr(eq + 1);
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return values;
}

struct PacketStats {
    bool any = false;
    bool first_is_key = false;
    double max_sample_average_bps = 0.0;
    double max_key_interval_seconds = 0.0;
};

PacketStats probe_video_packets_sampled(const ToolPaths& tools, const fs::path& source, double duration_seconds) {
    PacketStats combined;
    const auto windows = compliance_sample_windows(duration_seconds);
    for (std::size_t wi = 0; wi < windows.size(); ++wi) {
        const auto& sample = windows[wi];
        std::uint64_t sample_bytes = 0;
        std::optional<double> first_time;
        std::optional<double> last_end_time;
        std::optional<double> first_key_time;
        std::optional<double> last_key_time;
        std::size_t packet_index = 0;
        bool sample_any = false;

        std::string command = shq_s(tools.ffprobe) +
            " -v error -read_intervals " + shq_s(ffprobe_interval(sample)) +
            " -select_streams v:0 -show_packets -show_entries packet=pts_time,dts_time,duration_time,size,flags "
            "-of compact=p=0:nk=0 " + shq(source) + " 2>" + null_device();
        const bool ok = stream_command_lines(command, [&](const std::string& line) {
            const auto packet = parse_compact_fields(line);
            if (packet.empty()) return;
            auto get = [&](const char* key) -> std::string {
                const auto it = packet.find(key); return it == packet.end() ? std::string() : it->second;
            };
            auto time = number_value(get("dts_time"));
            if (!time) time = number_value(get("pts_time"));
            const auto size = integer_value(get("size"));
            const auto packet_duration = number_value(get("duration_time"));
            const bool key = get("flags").find('K') != std::string::npos;
            if (wi == 0 && packet_index == 0) combined.first_is_key = key;
            ++packet_index;
            sample_any = true;
            combined.any = true;
            if (!time) return;
            if (!first_time) first_time = *time;
            const double end_time = *time + ((packet_duration && *packet_duration > 0.0) ? *packet_duration : 0.0);
            if (!last_end_time || end_time > *last_end_time) last_end_time = end_time;
            if (size && *size > 0) sample_bytes += static_cast<std::uint64_t>(*size);
            if (key) {
                if (!first_key_time) first_key_time = *time;
                if (last_key_time)
                    combined.max_key_interval_seconds = std::max(combined.max_key_interval_seconds, *time - *last_key_time);
                last_key_time = *time;
            }
        });
        if (!ok) return PacketStats{};
        if (!sample_any || !first_time || !last_end_time) continue;
        const double sampled_span = *last_end_time - *first_time;
        if (sample_bytes > 0 && sampled_span > 0.0) {
            combined.max_sample_average_bps = std::max(combined.max_sample_average_bps,
                static_cast<double>(sample_bytes) * 8.0 / sampled_span);
        }
        if (!first_key_time || !last_key_time) {
            combined.max_key_interval_seconds = std::max(combined.max_key_interval_seconds,
                std::max(0.001, *last_end_time - *first_time));
        } else {
            combined.max_key_interval_seconds = std::max(combined.max_key_interval_seconds, *first_key_time - *first_time);
            combined.max_key_interval_seconds = std::max(combined.max_key_interval_seconds, *last_end_time - *last_key_time);
            if (*last_end_time > *first_time && combined.max_key_interval_seconds == 0.0)
                combined.max_key_interval_seconds = std::max(0.001, *last_end_time - *first_time);
        }
    }
    return combined;
}

int probe_max_consecutive_b_frames_sampled(const ToolPaths& tools, const fs::path& source, double duration_seconds) {
    int maximum = 0;
    bool saw_frame = false;
    for (const auto& sample : compliance_sample_windows(duration_seconds)) {
        int current = 0;
        bool first_slice_of_picture = false;
        std::ostringstream command;
        command << shq_s(tools.ffmpeg) << " -hide_banner -loglevel info -ss "
                << std::fixed << std::setprecision(3) << sample.start_seconds
                << " -i " << shq(source) << " -t " << sample.length_seconds
                << " -map 0:v:0 -an -sn -dn -c:v copy -bsf:v trace_headers -f null "
                << null_device() << " 2>&1";
        const bool ok = stream_command_lines(command.str(), [&](const std::string& line) {
            if (line.find("first_mb_in_slice") != std::string::npos) {
                const auto eq = line.rfind('=');
                const auto value = eq == std::string::npos ? std::optional<long long>{} :
                    integer_value(trim_copy(line.substr(eq + 1)));
                first_slice_of_picture = value && *value == 0;
                return;
            }
            if (!first_slice_of_picture || line.find("slice_type") == std::string::npos) return;
            const auto eq = line.rfind('=');
            const auto value = eq == std::string::npos ? std::optional<long long>{} :
                integer_value(trim_copy(line.substr(eq + 1)));
            first_slice_of_picture = false;
            if (!value) return;
            saw_frame = true;
            const int slice_type = static_cast<int>((*value % 5 + 5) % 5);
            if (slice_type == 1) { // H.264 B slice
                ++current;
                maximum = std::max(maximum, current);
            } else {
                current = 0;
            }
        });
        if (!ok) return -1;
    }
    return saw_frame ? maximum : -1;
}

std::vector<long long> trace_values(const fs::path& trace, const std::string& name) {
    std::vector<long long> values;
    std::ifstream f(trace);
    std::string line;
    while (std::getline(f, line)) {
        std::size_t pos = 0;
        while ((pos = line.find(name, pos)) != std::string::npos) {
            const bool left_ok = pos == 0 || (!std::isalnum(static_cast<unsigned char>(line[pos - 1])) && line[pos - 1] != '_');
            const auto end = pos + name.size();
            const bool right_ok = end >= line.size() || (!std::isalnum(static_cast<unsigned char>(line[end])) && line[end] != '_');
            if (left_ok && right_ok) {
                const auto eq = line.find('=', end);
                if (eq != std::string::npos) {
                    const auto value = integer_value(trim_copy(line.substr(eq + 1)));
                    if (value) values.push_back(*value);
                }
                break;
            }
            pos = end;
        }
    }
    return values;
}

bool trace_contains_marker(const fs::path& trace, const std::string& marker) {
    std::ifstream f(trace);
    std::string line;
    while (std::getline(f, line)) if (line.find(marker) != std::string::npos) return true;
    return false;
}

std::optional<long long> last_trace_value(const fs::path& trace, const std::string& name) {
    const auto values = trace_values(trace, name);
    if (values.empty()) return std::nullopt;
    return values.back();
}

struct H264HrdStats {
    bool saw_bitrate = false;
    bool saw_cpb = false;
    double max_bitrate_bps = 0.0;
    double max_cpb_bits = 0.0;
};

H264HrdStats parse_h264_hrd_stats(const fs::path& trace) {
    H264HrdStats stats;
    std::optional<int> bitrate_scale;
    std::optional<int> cpb_scale;
    std::ifstream f(trace);
    std::string line;
    auto line_value = [](const std::string& text) -> std::optional<long long> {
        const auto eq = text.rfind('=');
        if (eq == std::string::npos) return std::nullopt;
        return integer_value(trim_copy(text.substr(eq + 1)));
    };
    while (std::getline(f, line)) {
        if (line.find("bit_rate_scale") != std::string::npos &&
            line.find("bit_rate_value") == std::string::npos) {
            const auto value = line_value(line);
            if (value && *value >= 0 && *value <= 15) bitrate_scale = static_cast<int>(*value);
            continue;
        }
        if (line.find("cpb_size_scale") != std::string::npos &&
            line.find("cpb_size_value") == std::string::npos) {
            const auto value = line_value(line);
            if (value && *value >= 0 && *value <= 15) cpb_scale = static_cast<int>(*value);
            continue;
        }
        if (line.find("bit_rate_value_minus1[") != std::string::npos) {
            const auto value = line_value(line);
            if (value && *value >= 0 && bitrate_scale) {
                stats.saw_bitrate = true;
                stats.max_bitrate_bps = std::max(stats.max_bitrate_bps,
                    static_cast<double>(*value + 1) * std::ldexp(1.0, 6 + *bitrate_scale));
            }
            continue;
        }
        if (line.find("cpb_size_value_minus1[") != std::string::npos) {
            const auto value = line_value(line);
            if (value && *value >= 0 && cpb_scale) {
                stats.saw_cpb = true;
                stats.max_cpb_bits = std::max(stats.max_cpb_bits,
                    static_cast<double>(*value + 1) * std::ldexp(1.0, 4 + *cpb_scale));
            }
        }
    }
    return stats;
}

double probe_audio_elementary_bitrate_sampled(const ToolPaths& tools, const fs::path& source,
                                                int ordinal, double duration_seconds) {
    double max_average_bps = 0.0;
    bool saw_rate = false;
    for (const auto& sample : compliance_sample_windows(duration_seconds)) {
        std::ostringstream command;
        command << shq_s(tools.ffmpeg) << " -hide_banner -loglevel error -ss "
                << std::fixed << std::setprecision(3) << sample.start_seconds
                << " -i " << shq(source) << " -t " << sample.length_seconds
                << " -map 0:a:" << ordinal << " -vn -sn -dn -c:a copy -f dts pipe:1 2>" << null_device();
        const auto bytes = count_command_bytes(command.str());
        if (!bytes || *bytes == 0 || sample.length_seconds <= 0.0) continue;
        // Count bytes after FFmpeg has demuxed and remuxed the selected codec as
        // raw DTS.  This strips BDAV/PES/container packetization that may be
        // present in AVPacket::size and measures the elementary DTS/DTS-HD
        // stream itself.  A 60 s window makes boundary-frame error negligible.
        const double rate = static_cast<double>(*bytes) * 8.0 / sample.length_seconds;
        max_average_bps = std::max(max_average_bps, rate);
        saw_rate = true;
    }
    return saw_rate ? max_average_bps : 0.0;
}

VideoStreamInfo probe_video_stream(const ToolPaths& tools, const fs::path& source, const fs::path& out,
                                   double duration_seconds, DiscTarget target) {
    VideoStreamInfo info;
    fs::create_directories(out);
    const auto stream_file = out / "video-stream.txt";
    const std::string probe = shq_s(tools.ffprobe) +
        " -v error -select_streams v:0 -show_entries "
        "stream=codec_name,profile,level,width,height,pix_fmt,field_order,r_frame_rate,avg_frame_rate,bit_rate,sample_aspect_ratio,color_primaries,color_transfer,color_space,color_range "
        "-of default=nw=1:nk=0 " + shq(source) + " > " + shq(stream_file) + " 2>" + null_device();
    if (!run_probe(probe)) return info;
    const auto values = read_key_values(stream_file);
    auto text = [&](const char* key) -> std::string {
        const auto it = values.find(key); return it == values.end() ? std::string() : it->second;
    };
    info.codec_name = text("codec_name");
    info.profile = text("profile");
    info.pixel_format = text("pix_fmt");
    info.field_order = text("field_order");
    info.sample_aspect_ratio = text("sample_aspect_ratio");
    info.color_primaries = text("color_primaries");
    info.color_transfer = text("color_transfer");
    info.color_space = text("color_space");
    info.color_range = text("color_range");
    info.level = static_cast<int>(integer_value(text("level")).value_or(-1));
    info.width = static_cast<int>(integer_value(text("width")).value_or(0));
    info.height = static_cast<int>(integer_value(text("height")).value_or(0));
    info.frame_rate = probed_frame_rate(source, text("avg_frame_rate"), text("r_frame_rate"));

    // Header/SPS inspection is cheap and is deliberately done before packet/frame sampling.
    // This lets obviously non-Blu-ray streams fail fast without scanning any sample windows.
    if (info.codec_name == "h264" || info.codec_name == "mpeg2video" || info.codec_name == "hevc") {
        const auto trace = out / "video-trace.txt";
        const std::string trace_command = shq_s(tools.ffmpeg) +
            " -hide_banner -loglevel verbose -i " + shq(source) +
            " -map 0:v:0 -c:v copy -bsf:v trace_headers -frames:v 1 -f null " + null_device() +
            " >" + null_device() + " 2>" + shq(trace);
        if (!run_probe(trace_command)) return info;

        if (info.codec_name == "hevc") {
            info.hevc_profile_idc = static_cast<int>(last_trace_value(trace, "general_profile_idc").value_or(-1));
            info.hevc_tier_flag = static_cast<int>(last_trace_value(trace, "general_tier_flag").value_or(-1));
            info.hevc_level_idc = static_cast<int>(last_trace_value(trace, "general_level_idc").value_or(-1));
            info.hevc_chroma_format_idc = static_cast<int>(last_trace_value(trace, "chroma_format_idc").value_or(-1));
            info.hevc_bit_depth_luma_minus8 = static_cast<int>(last_trace_value(trace, "bit_depth_luma_minus8").value_or(-1));
            info.hevc_bit_depth_chroma_minus8 = static_cast<int>(last_trace_value(trace, "bit_depth_chroma_minus8").value_or(-1));
            const auto nal_hrd = last_trace_value(trace, "nal_hrd_parameters_present_flag").value_or(0);
            const auto vcl_hrd = last_trace_value(trace, "vcl_hrd_parameters_present_flag").value_or(0);
            info.hevc_hrd_present = nal_hrd != 0 || vcl_hrd != 0;
            const auto hrd = parse_h264_hrd_stats(trace);
            if (hrd.saw_bitrate) info.hevc_hrd_max_bitrate_bps = hrd.max_bitrate_bps;
            if (hrd.saw_cpb) info.hevc_hrd_max_cpb_bits = hrd.max_cpb_bits;
        } else if (info.codec_name == "h264") {
            info.h264_profile_idc = static_cast<int>(last_trace_value(trace, "profile_idc").value_or(-1));
            info.h264_level_idc = static_cast<int>(last_trace_value(trace, "level_idc").value_or(-1));
            info.h264_chroma_format_idc = static_cast<int>(last_trace_value(trace, "chroma_format_idc").value_or(-1));
            info.h264_bit_depth_luma_minus8 = static_cast<int>(last_trace_value(trace, "bit_depth_luma_minus8").value_or(-1));
            info.h264_bit_depth_chroma_minus8 = static_cast<int>(last_trace_value(trace, "bit_depth_chroma_minus8").value_or(-1));
            info.h264_max_num_ref_frames = static_cast<int>(last_trace_value(trace, "max_num_ref_frames").value_or(-1));
            info.h264_frame_mbs_only_flag = static_cast<int>(last_trace_value(trace, "frame_mbs_only_flag").value_or(-1));
            info.h264_mb_adaptive_frame_field_flag = static_cast<int>(last_trace_value(trace, "mb_adaptive_frame_field_flag").value_or(-1));
            const auto nal_hrd = last_trace_value(trace, "nal_hrd_parameters_present_flag").value_or(0);
            const auto vcl_hrd = last_trace_value(trace, "vcl_hrd_parameters_present_flag").value_or(0);
            info.h264_hrd_present = nal_hrd != 0 || vcl_hrd != 0;
            info.h264_buffering_period_sei_present = trace_contains_marker(trace, "Buffering Period");
            info.h264_picture_timing_sei_present = trace_contains_marker(trace, "Picture Timing");
            info.h264_initial_cpb_removal_delay_90k = static_cast<double>(
                last_trace_value(trace, "initial_cpb_removal_delay[0]").value_or(0));
            // HRD scale syntax belongs to the immediately following value array.
            // Do not combine the largest scale from one repeated/NAL/VCL HRD
            // structure with values from another; that can inflate a valid
            // Blu-ray maxrate by 2x or more.
            const auto hrd = parse_h264_hrd_stats(trace);
            if (hrd.saw_bitrate) info.h264_hrd_max_bitrate_bps = hrd.max_bitrate_bps;
            if (hrd.saw_cpb) info.h264_hrd_max_cpb_bits = hrd.max_cpb_bits;
        } else {
            info.mpeg2_profile_and_level_indication = static_cast<int>(last_trace_value(trace, "profile_and_level_indication").value_or(-1));
            const auto bitrate = last_trace_value(trace, "bit_rate_value");
            const auto bitrate_ext = last_trace_value(trace, "bit_rate_extension").value_or(0);
            if (bitrate && *bitrate >= 0)
                info.mpeg2_header_bitrate_bps = static_cast<double>((bitrate_ext << 18) | *bitrate) * 400.0;
            const auto vbv = last_trace_value(trace, "vbv_buffer_size_value");
            const auto vbv_ext = last_trace_value(trace, "vbv_buffer_size_extension").value_or(0);
            if (vbv && *vbv >= 0)
                info.mpeg2_vbv_bits = static_cast<double>((vbv_ext << 10) | *vbv) * 16.0 * 1024.0;
        }
    }

    if (!evaluate_blu_ray_video_headers(info, target).compliant) return info;

    const auto packets = probe_video_packets_sampled(tools, source, duration_seconds);
    info.sampled_average_bitrate_bps = packets.max_sample_average_bps;
    info.first_packet_is_keyframe = packets.first_is_key;
    info.max_keyframe_interval_seconds = packets.max_key_interval_seconds;
    if (info.codec_name == "h264")
        info.max_consecutive_b_frames = probe_max_consecutive_b_frames_sampled(tools, source, duration_seconds);
    return info;
}

AudioStreamInfo probe_audio_stream(const ToolPaths& tools, const fs::path& source, const fs::path& out, int ordinal, double duration_seconds) {
    AudioStreamInfo info;
    fs::create_directories(out);
    const auto stream_file = out / "audio-stream.txt";
    const std::string probe = shq_s(tools.ffprobe) +
        " -v error -select_streams a:" + std::to_string(ordinal) + " -show_entries "
        "stream=codec_name,profile,sample_fmt,sample_rate,channels,bit_rate,bits_per_raw_sample,bits_per_sample "
        "-of default=nw=1:nk=0 " + shq(source) + " > " + shq(stream_file) + " 2>" + null_device();
    if (!run_probe(probe)) return info;
    const auto values = read_key_values(stream_file);
    auto text = [&](const char* key) -> std::string {
        const auto it = values.find(key); return it == values.end() ? std::string() : it->second;
    };
    info.codec_name = text("codec_name");
    info.present = !info.codec_name.empty();
    info.profile = text("profile");
    info.sample_format = text("sample_fmt");
    info.sample_rate = static_cast<int>(integer_value(text("sample_rate")).value_or(0));
    info.channels = static_cast<int>(integer_value(text("channels")).value_or(0));
    info.reported_bitrate_bps = number_value(text("bit_rate")).value_or(0.0);
    info.bitrate_bps = info.reported_bitrate_bps;
    info.bits_per_sample = static_cast<int>(integer_value(text("bits_per_raw_sample")).value_or(0));
    if (info.bits_per_sample <= 0)
        info.bits_per_sample = static_cast<int>(integer_value(text("bits_per_sample")).value_or(0));
    if (info.bitrate_bps <= 0.0 && info.codec_name.rfind("pcm_", 0) == 0 &&
        info.sample_rate > 0 && info.channels > 0 && info.bits_per_sample > 0)
        info.bitrate_bps = static_cast<double>(info.sample_rate) * static_cast<double>(info.channels) * static_cast<double>(info.bits_per_sample);
    // ffprobe's stream-level DTS bit_rate is not a reliable elementary-stream
    // compliance value. For example, a standard 1.50975 Mb/s DTS core is
    // commonly reported as 1,536,000 b/s. Measure the demuxed raw DTS elementary stream over bounded sample windows
    // instead, including DTS-HD extensions when present.
    if (info.codec_name == "dts") {
        const auto core_rate = probe_dts_core_bitrate_sampled(tools, source, ordinal, duration_seconds);
        if (core_rate) info.dts_core_bitrate_bps = *core_rate;
        const bool hd = info.profile.find("DTS-HD") != std::string::npos;
        if (hd) {
            const double measured = probe_audio_elementary_bitrate_sampled(tools, source, ordinal, duration_seconds);
            if (measured > 0.0) info.bitrate_bps = measured;
        } else if (core_rate) {
            info.bitrate_bps = *core_rate;
        }
    }
    return info;
}

struct TitleStreamAnalysis {
    VideoStreamInfo video;
    BluRayStreamDecision video_decision;
    struct AudioTrack {
        SourceTrackDescriptor source;
        AudioStreamInfo info;
        BluRayStreamDecision decision;
    };
    std::vector<AudioTrack> audio;
    std::vector<SourceTrackDescriptor> subtitles;
};

TitleStreamAnalysis analyze_title_streams(const ToolPaths& tools, const fs::path& source, const fs::path& out,
                                          const SourceTrackCatalog& catalog, double duration_seconds, DiscTarget target) {
    TitleStreamAnalysis analysis;
    analysis.video = probe_video_stream(tools, source, out / "video", duration_seconds, target);
    analysis.video_decision = evaluate_blu_ray_video(analysis.video, target);
    for (const auto& track : catalog.audio) {
        auto info = probe_audio_stream(tools, source, out / ("audio-" + std::to_string(track.source_ordinal + 1)), track.source_ordinal, duration_seconds);
        analysis.audio.push_back({track, info, evaluate_blu_ray_audio(info)});
    }
    analysis.subtitles = catalog.subtitles;
    return analysis;
}


constexpr int ComplianceCacheSchema = 11;

std::string compliance_cache_key(const MediaFingerprint& fp, const SourceTrackCatalog& catalog,
                                 double duration_seconds, DiscTarget target) {
    std::ostringstream k;
    k << "bdmvauthor-compliance-cache-v11\n" << fp.canonical()
      << "\ntarget=" << static_cast<int>(target)
      << "\nrules=bd-uhd-primary-av-v11-h264-cpb-delay"
      << "\nsampling=first-middle-last-60s-v1"
      << "\nduration-ms=" << static_cast<long long>(std::llround(std::max(0.0, duration_seconds) * 1000.0));
    for (const auto& track : catalog.audio)
        k << "\naudio=" << track.source_ordinal << ',' << track.codec_name << ',' << track.language;
    for (const auto& track : catalog.subtitles)
        k << "\nsubtitle=" << track.source_ordinal << ',' << track.codec_name << ',' << track.language;
    return sha256_hex(k.str());
}

bool load_compliance_cache(const fs::path& file, const SourceTrackCatalog& catalog,
                           TitleStreamAnalysis& analysis) {
    std::ifstream f(file);
    if (!f) return false;
    std::string magic;
    int schema = 0;
    if (!(f >> magic >> schema) || magic != "BDMVAUTHOR_COMPLIANCE" || schema != ComplianceCacheSchema)
        return false;

    int video_compliant = 0;
    if (!(f >> std::quoted(analysis.video.codec_name)
            >> analysis.video.width >> analysis.video.height >> analysis.video.frame_rate
            >> std::quoted(analysis.video.color_primaries) >> std::quoted(analysis.video.color_transfer)
            >> std::quoted(analysis.video.color_space) >> std::quoted(analysis.video.color_range)
            >> video_compliant >> std::quoted(analysis.video_decision.reason)
            >> std::quoted(analysis.video_decision.tsmuxer_codec)
            >> std::quoted(analysis.video_decision.elementary_extension)
            >> analysis.video.h264_hrd_max_bitrate_bps
            >> analysis.video.hevc_hrd_max_bitrate_bps
            >> analysis.video.mpeg2_header_bitrate_bps
            >> analysis.video.sampled_average_bitrate_bps)) return false;
    analysis.video_decision.compliant = video_compliant != 0;

    std::size_t audio_count = 0;
    if (!(f >> audio_count) || audio_count != catalog.audio.size()) return false;
    analysis.audio.clear();
    for (std::size_t i = 0; i < audio_count; ++i) {
        int ordinal = -1;
        int compliant = 0;
        std::string source_codec;
        std::string reason;
        std::string tsmuxer_codec;
        std::string extension;
        int sample_rate = 0, channels = 0, bits_per_sample = 0;
        double bitrate_bps = 0.0;
        if (!(f >> ordinal >> std::quoted(source_codec) >> compliant >> std::quoted(reason)
                >> std::quoted(tsmuxer_codec) >> std::quoted(extension)
                >> sample_rate >> channels >> bits_per_sample >> bitrate_bps)) return false;
        const auto& source = catalog.audio[i];
        if (ordinal != source.source_ordinal || source_codec != source.codec_name) return false;
        AudioStreamInfo info;
        info.present = true;
        info.codec_name = source.codec_name;
        info.sample_rate = sample_rate;
        info.channels = channels;
        info.bits_per_sample = bits_per_sample;
        info.bitrate_bps = bitrate_bps;
        BluRayStreamDecision decision;
        decision.compliant = compliant != 0;
        decision.reason = std::move(reason);
        decision.tsmuxer_codec = std::move(tsmuxer_codec);
        decision.elementary_extension = std::move(extension);
        analysis.audio.push_back({source, std::move(info), std::move(decision)});
    }
    analysis.subtitles = catalog.subtitles;
    return f.good() || f.eof();
}

void publish_compliance_cache(const fs::path& file, const TitleStreamAnalysis& analysis,
                              const CacheMetadata& metadata) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    if (ec) return;
    const fs::path tmp = cache_publish_temp_path(file);
    fs::remove(tmp, ec); ec.clear();
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f) return;
        f << "BDMVAUTHOR_COMPLIANCE " << ComplianceCacheSchema << '\n';
        f << std::quoted(analysis.video.codec_name) << ' '
          << analysis.video.width << ' ' << analysis.video.height << ' '
          << std::setprecision(17) << analysis.video.frame_rate << ' '
          << std::quoted(analysis.video.color_primaries) << ' ' << std::quoted(analysis.video.color_transfer) << ' '
          << std::quoted(analysis.video.color_space) << ' ' << std::quoted(analysis.video.color_range) << ' '
          << (analysis.video_decision.compliant ? 1 : 0) << ' '
          << std::quoted(analysis.video_decision.reason) << ' '
          << std::quoted(analysis.video_decision.tsmuxer_codec) << ' '
          << std::quoted(analysis.video_decision.elementary_extension) << ' '
          << analysis.video.h264_hrd_max_bitrate_bps << ' '
          << analysis.video.hevc_hrd_max_bitrate_bps << ' '
          << analysis.video.mpeg2_header_bitrate_bps << ' '
          << analysis.video.sampled_average_bitrate_bps << '\n';
        f << analysis.audio.size() << '\n';
        for (const auto& audio : analysis.audio)
            f << audio.source.source_ordinal << ' ' << std::quoted(audio.source.codec_name) << ' '
              << (audio.decision.compliant ? 1 : 0) << ' ' << std::quoted(audio.decision.reason) << ' '
              << std::quoted(audio.decision.tsmuxer_codec) << ' '
              << std::quoted(audio.decision.elementary_extension) << ' '
              << audio.info.sample_rate << ' ' << audio.info.channels << ' ' << audio.info.bits_per_sample << ' '
              << audio.info.bitrate_bps << '\n';
        f.flush();
        if (!f) { f.close(); fs::remove(tmp, ec); return; }
    }
    const auto tmp_size=fs::file_size(tmp,ec);
    if(ec||tmp_size==0){fs::remove(tmp,ec);return;}
    if(fs::exists(file,ec)){ec.clear();fs::remove(file,ec);if(ec){fs::remove(tmp,ec);return;}}
    ec.clear();fs::rename(tmp,file,ec);
    if(ec)fs::remove(tmp,ec);
    else touch_cache_metadata(file, metadata);
}

std::string remux_compliant_video(const ToolPaths& tools, const fs::path& source, const fs::path& dir,
                                  const VideoStreamInfo& info, const BluRayStreamDecision& decision) {
    fs::create_directories(dir);
    const fs::path out = dir / (std::string("video") + decision.elementary_extension);
    std::ostringstream command;
    command << shq_s(tools.ffmpeg) << " -hide_banner -loglevel error -y -i " << shq(source)
            << " -map 0:v:0 -an -sn -dn -c:v copy ";
    const auto color_unspecified = [](const std::string& value) {
        return value.empty() || value == "unknown" || value == "unspecified" || value == "reserved" || value == "N/A";
    };
    const auto bt709_or_unknown = [&](const std::string& value) {
        return color_unspecified(value) || value == "bt709";
    };
    const bool bt709_compatible = bt709_or_unknown(info.color_primaries) && bt709_or_unknown(info.color_transfer) &&
                                  bt709_or_unknown(info.color_space);
    const bool tag_bt709 = bt709_compatible &&
                           (color_unspecified(info.color_primaries) || color_unspecified(info.color_transfer) ||
                            color_unspecified(info.color_space));
    if (decision.tsmuxer_codec == "V_MPEG4/ISO/AVC") {
        command << "-bsf:v h264_mp4toannexb";
        if (tag_bt709) command << ",h264_metadata=colour_primaries=1:transfer_characteristics=1:matrix_coefficients=1:video_full_range_flag=0";
        command << " -f h264 ";
    } else if (decision.tsmuxer_codec == "V_MPEG-2") {
        if (tag_bt709) command << "-bsf:v mpeg2_metadata=colour_primaries=1:transfer_characteristics=1:matrix_coefficients=1 ";
        command << "-f mpeg2video ";
    } else if (decision.tsmuxer_codec == "V_MS/VFW/WVC1") {
        if (tag_bt709) throw std::runtime_error("untagged VC-1 cannot be safely color-tagged without re-encoding");
        command << "-f vc1 ";
    } else if (decision.tsmuxer_codec == "V_MPEGH/ISO/HEVC") {
        command << "-bsf:v hevc_mp4toannexb";
        if (tag_bt709) command << ",hevc_metadata=colour_primaries=1:transfer_characteristics=1:matrix_coefficients=1:video_full_range_flag=0";
        command << " -f hevc ";
    }
    else throw std::runtime_error("unsupported compliant video passthrough codec: " + decision.tsmuxer_codec);
    command << shq(out);
    run(command.str());
    std::string meta = decision.tsmuxer_codec + ", " + shq(out);
    if (decision.tsmuxer_codec == "V_MPEG4/ISO/AVC") meta += ", insertSEI, contSPS";
    return meta + "\n";
}

std::string remux_compliant_audio(const ToolPaths& tools, const fs::path& source, const fs::path& dir,
                                  const BluRayStreamDecision& decision, const std::string& language, int ordinal) {
    fs::create_directories(dir);
    const fs::path out = dir / (std::string("audio") + decision.elementary_extension);
    std::ostringstream command;
    command << shq_s(tools.ffmpeg) << " -hide_banner -loglevel error -y -i " << shq(source)
            << " -map 0:a:" << ordinal << " -vn -sn -dn -c:a copy ";
    if (decision.elementary_extension == ".ac3") command << "-f ac3 ";
    else if (decision.elementary_extension == ".eac3") command << "-f eac3 ";
    else if (decision.elementary_extension == ".dts") command << "-f dts ";
    else if (decision.elementary_extension == ".wav") command << "-f wav ";
    else throw std::runtime_error("unsupported compliant audio passthrough format: " + decision.elementary_extension);
    command << shq(out);
    run(command.str());
    return decision.tsmuxer_codec + ", " + shq(out) + ", lang=" + language + "\n";
}

std::string subtitle_argb_hex(const Rgba& c) {
    const std::uint32_t v=(static_cast<std::uint32_t>(c.a)<<24)|(static_cast<std::uint32_t>(c.r)<<16)|
                          (static_cast<std::uint32_t>(c.g)<<8)|static_cast<std::uint32_t>(c.b);
    std::ostringstream out;out<<"0x"<<std::uppercase<<std::hex<<std::setw(8)<<std::setfill('0')<<v;return out.str();
}
std::string subtitle_spumux_color(const Rgba& c) {
    std::ostringstream out;out<<"rgba("<<static_cast<int>(c.r)<<','<<static_cast<int>(c.g)<<','<<static_cast<int>(c.b)<<','<<static_cast<int>(c.a)<<')';return out.str();
}
int subtitle_units_to_pixels(int units, int subtitle_plane_height, int minimum = 0) {
    const int height = std::max(1, subtitle_plane_height);
    const int scaled = static_cast<int>(std::lround(static_cast<double>(units) * height / 1080.0));
    return std::max(minimum, scaled);
}

std::string subtitle_tsmuxer_style_meta(const SourceTrackDescriptor& track, int subtitle_plane_height) {
    const auto& st=track.subtitle_style;
    const int bottom_offset = subtitle_units_to_pixels(st.bottom_offset_px, subtitle_plane_height);
    if(track.codec_name=="hdmv_pgs_subtitle") {
        if(!st.override_style) return {};
        std::ostringstream meta;
        meta<<", bottom-offset="<<bottom_offset<<", font-border="<<st.border_width;
        return meta.str();
    }

    // tsMuxer does not reliably turn generic CSS-style family aliases into an
    // installed face. Always supply a concrete family for text subtitles, even
    // when the user has not enabled the rest of the style override.
    const std::string font_family = detail::resolve_font_family(st.font_family, st.bold, st.italic);
    const int font_size = subtitle_units_to_pixels(st.font_size_px, subtitle_plane_height, 4);
    std::ostringstream meta;
    // Always send an explicit ARGB color for text subtitles. tsMuxer's Win32
    // renderer initializes its Font color to 0x00ffffff (transparent white),
    // and the meta parser only promotes alpha to opaque when font-color is
    // present. Omitting this for the default (non-override) style therefore
    // produces a valid-but-blank PGS subtitle track on Windows.
    meta<<", font-name=\""<<font_family<<"\", font-size="<<font_size
        <<", bottom-offset="<<bottom_offset
        <<", font-color="<<subtitle_argb_hex(st.font_color);
    if(!st.override_style) return meta.str();
    meta<<", line-spacing="<<std::fixed<<std::setprecision(3)<<st.line_spacing
        <<", font-border="<<st.border_width
        <<", fadein-time="<<st.fade_in_ms<<", fadeout-time="<<st.fade_out_ms;
    if(st.bold) meta << ", font-bold";
    if(st.italic) meta << ", font-italic";
    if(st.underline) meta << ", font-underline";
    // The bundled parser accepts font-strike-out (the GUI/help text historically used font-strikeout).
    if(st.strikeout)meta<<", font-strike-out";
    return meta.str();
}

std::string prepare_subtitle(const ToolPaths& tools, const fs::path& source, const fs::path& dir,
                             const SourceTrackDescriptor& track, int width, int height, double fps) {
    fs::create_directories(dir);
    const bool pgs = track.codec_name == "hdmv_pgs_subtitle";
    const fs::path out = dir / (pgs ? "subtitle.sup" : "subtitle.srt");
    const fs::path subtitle_source = track.external_source.empty() ? source : track.external_source;
    const int subtitle_ordinal = track.external_source.empty() ? track.source_ordinal : 0;
    std::ostringstream command;
    command << shq_s(tools.ffmpeg) << " -hide_banner -loglevel error -y -i " << shq(subtitle_source)
            << " -map 0:s:" << subtitle_ordinal << " -vn -an -dn ";
    if (pgs) command << "-c:s copy -f sup ";
    else command << "-c:s srt -f srt ";
    command << shq(out);
    run(command.str());

    std::ostringstream meta;
    meta << (pgs ? "S_HDMV/PGS" : "S_TEXT/UTF8") << ", " << shq(out)
         << ", lang=" << track.language
         << ", video-width=" << width << ", video-height=" << height;
    if (fps > 0.0) meta << ", fps=" << std::fixed << std::setprecision(3) << fps;
    meta << subtitle_tsmuxer_style_meta(track, height);
    meta << "\n";
    return meta.str();
}

double probe_duration_seconds(const ToolPaths& tools, const fs::path& source, const fs::path& out) {
    fs::create_directories(out.parent_path());
    std::string c = shq_s(tools.ffprobe) +
                    " -v error -show_entries format=duration -of default=nw=1:nk=1 " +
                    shq(source) + " > " + shq(out) + " 2>";
#ifdef _WIN32
    c += "NUL";
#else
    c += "/dev/null";
#endif
    const int rc = shell_system(c);
    if (rc != 0) return 0.0;
    std::ifstream f(out);
    double seconds = 0.0;
    f >> seconds;
    return std::isfinite(seconds) && seconds > 0.0 ? seconds : 0.0;
}

bool probe_stream_present(const ToolPaths& tools, const fs::path& source, const std::string& selector,
                          const fs::path& out) {
    fs::create_directories(out.parent_path());
    const std::string c = shq_s(tools.ffprobe) + " -v error -select_streams " + shq_s(selector) +
        " -show_entries stream=index -of csv=p=0 " + shq(source) + " > " + shq(out) + " 2>" + null_device();
    if (!run_probe(c)) return false;
    std::ifstream f(out);
    std::string line;
    while (std::getline(f, line))
        if (!line.empty()) return true;
    return false;
}

double probe_stream_duration_seconds(const ToolPaths& tools, const fs::path& source, const std::string& selector,
                                     const fs::path& out) {
    fs::create_directories(out.parent_path());
    const std::string c = shq_s(tools.ffprobe) + " -v error -select_streams " + shq_s(selector) +
        " -show_entries stream=duration -of default=nw=1:nk=1 " + shq(source) + " > " + shq(out) + " 2>" + null_device();
    if (run_probe(c)) {
        std::ifstream f(out);
        double seconds = 0.0;
        f >> seconds;
        if (std::isfinite(seconds) && seconds > 0.0) return seconds;
    }
    // Some containers do not publish per-stream duration.  In that case the
    // container duration is the best available fallback after stream presence
    // has already been established by the caller.
    return probe_duration_seconds(tools, source, out.string() + ".format");
}


std::vector<double> probe_source_chapter_starts(const ToolPaths& tools, const fs::path& source, const fs::path& out) {
    std::string c = shq_s(tools.ffprobe) +
                    " -v error -show_entries chapter=start_time -of csv=p=0 " +
                    shq(source) + " > " + shq(out) + " 2>";
#ifdef _WIN32
    c += "NUL";
#else
    c += "/dev/null";
#endif
    const int rc = shell_system(c);
    if (rc != 0) return {};
    std::ifstream f(out);
    std::vector<double> starts;
    std::string line;
    while (std::getline(f, line)) {
        try {
            std::size_t used = 0;
            const double value = std::stod(line, &used);
            if (used != 0U && std::isfinite(value) && value >= 0.0) starts.push_back(value);
        } catch (...) {}
    }
    return starts;
}

std::vector<double> normalize_chapter_starts(std::vector<double> starts, double duration_seconds) {
    std::sort(starts.begin(), starts.end());
    std::vector<double> out;
    for (double value : starts) {
        if (!std::isfinite(value) || value <= 0.001) continue;
        if (duration_seconds > 0.0 && value >= duration_seconds - 0.001) continue;
        if (!out.empty() && std::abs(value - out.back()) <= 0.001) continue;
        out.push_back(value);
    }
    return out;
}

std::vector<double> interval_chapter_starts(double duration_seconds, double interval_seconds) {
    std::vector<double> out;
    if (!(duration_seconds > 0.0) || !(interval_seconds > 0.0) || !std::isfinite(interval_seconds)) return out;
    for (double t = interval_seconds; t < duration_seconds - 0.001; t += interval_seconds) {
        if (out.size() >= 998U) break;
        out.push_back(t);
    }
    return out;
}

std::vector<double> resolve_title_chapters(const Title& title, double duration_seconds,
                                           std::vector<double> source_chapters) {
    switch (title.chapter_mode) {
        case ChapterMode::None: return {};
        case ChapterMode::Manual: return normalize_chapter_starts(title.chapters_seconds, duration_seconds);
        case ChapterMode::Interval: return interval_chapter_starts(duration_seconds, title.chapter_interval_seconds);
        case ChapterMode::SourceOrFiveMinute: {
            auto resolved = normalize_chapter_starts(std::move(source_chapters), duration_seconds);
            if (!resolved.empty()) return resolved;
            return interval_chapter_starts(duration_seconds, 300.0);
        }
    }
    return {};
}

using VideoProgressCallback = std::function<void(double, int, int)>;

void run_with_ffmpeg_progress(const std::string& command, const fs::path& progress_file,
                              double duration_seconds, const std::function<void(double)>& progress) {
    std::error_code ec;
    fs::remove(progress_file, ec);
    const fs::path stderr_file = progress_file.string() + ".stderr";
    fs::remove(stderr_file, ec);
    const auto guarded_command = fail_on_interrupt_command("(" + command + ") 2> " + shq(stderr_file));
    auto process = std::async(std::launch::async, [guarded_command] { return shell_system(guarded_command); });
    double last_fraction = -1.0;
    while (process.wait_for(std::chrono::milliseconds(125)) != std::future_status::ready) {
        if (!progress || duration_seconds <= 0.0) continue;
        const auto seconds = detail::read_ffmpeg_progress_seconds(progress_file);
        if (!seconds) continue;
        const double fraction = std::min(detail::progress_fraction(*seconds, duration_seconds), 0.999);
        if (fraction > last_fraction + 0.0001) {
            last_fraction = fraction;
            progress(fraction);
        }
    }
    const int rc = process.get();
    if (rc != 0) {
        const auto diagnostic = trim_copy(read_small_text(stderr_file));
        fs::remove(stderr_file, ec);
        std::string message = "command failed (" + command_status(rc) + "): " + command;
        if (!diagnostic.empty()) message += "\nFFmpeg: " + diagnostic;
        throw std::runtime_error(message);
    }
    fs::remove(stderr_file, ec);
    if (progress) progress(1.0);
}

int audio_channels(const ToolPaths& tools, const fs::path& source, const fs::path& tmp, int ordinal) {
    // Several callers deliberately pass a per-purpose nested probe directory
    // (for example menu-1/audio-rate).  Shell redirection opens the output
    // file before ffprobe starts, so the directory must exist first.
    fs::create_directories(tmp);
    auto out = tmp / "probe-channels.txt";
    std::string c = shq_s(tools.ffprobe) +
                    " -v error -select_streams a:" + std::to_string(ordinal) +
                    " -show_entries stream=channels -of default=nw=1:nk=1 " +
                    shq(source) + " > " + shq(out) + " 2>";
#ifdef _WIN32
    c += "NUL";
#else
    c += "/dev/null";
#endif
    run(c);
    std::ifstream f(out);
    int channels = 0;
    f >> channels;
    if (channels < 1) return 2;
    return channels;
}

bool valid_x264_preset(const std::string& preset) {
    static constexpr std::array<const char*, 10> presets = {
        "ultrafast", "superfast", "veryfast", "faster", "fast",
        "medium", "slow", "slower", "veryslow", "placebo"
    };
    return std::any_of(presets.begin(), presets.end(), [&](const char* p) { return preset == p; });
}

void validate_encoding(const EncodingProfile& e, const char* where, bool allow_exceeding_format_limits) {
    if (e.keyframe_interval_frames < 0)
        throw std::runtime_error(std::string(where) + " keyframe/GOP interval must be zero (default) or a positive frame count");
    const int max_video_bitrate = e.video_codec == VideoCodec::Hevc ? 86000 : 40000;
    if (e.video_bitrate_kbps < (allow_exceeding_format_limits ? 1 : 1000) ||
        (!allow_exceeding_format_limits && e.video_bitrate_kbps > max_video_bitrate))
        throw std::runtime_error(std::string(where) + " video bitrate must be between " +
                                 std::to_string(allow_exceeding_format_limits ? 1 : 1000) + " and " +
                                 std::to_string(allow_exceeding_format_limits ? 1000000 : max_video_bitrate) + " kb/s");
    if (e.video_min_bitrate_kbps < 0)
        throw std::runtime_error(std::string(where) + " video minrate must not be negative");
    if (e.video_max_bitrate_kbps < 1)
        throw std::runtime_error(std::string(where) + " video maxrate must be at least 1 kb/s");
    if (e.video_min_bitrate_kbps > e.video_max_bitrate_kbps)
        throw std::runtime_error(std::string(where) + " video minrate must not exceed video maxrate");
    if (e.video_bitrate_kbps < e.video_min_bitrate_kbps || e.video_bitrate_kbps > e.video_max_bitrate_kbps)
        throw std::runtime_error(std::string(where) + " average video bitrate must be between minrate and maxrate");
    if (!valid_x264_preset(e.x264_preset))
        throw std::runtime_error(std::string(where) + " has an invalid x264 preset: " + e.x264_preset);
    if (!valid_x264_preset(e.x265_preset))
        throw std::runtime_error(std::string(where) + " has an invalid x265 preset: " + e.x265_preset);
    if (e.ac3_bitrate_kbps < (allow_exceeding_format_limits ? 1 : 192) ||
        (!allow_exceeding_format_limits && e.ac3_bitrate_kbps > 640))
        throw std::runtime_error(std::string(where) + " AC-3 bitrate is outside the permitted range");
    if (e.dca_bitrate_kbps < (allow_exceeding_format_limits ? 1 : 384) ||
        (!allow_exceeding_format_limits && e.dca_bitrate_kbps > 1536))
        throw std::runtime_error(std::string(where) + " DTS/DCA bitrate is outside the permitted range");
    if (!lossless_bit_depth_supported_by_encoder(e.lpcm_bit_depth))
        throw std::runtime_error(std::string(where) + " lossless audio bit depth must be Auto, 16 or 24 bits");
}

void validate_encoding_for_target(const EncodingProfile& e, DiscTarget target, const std::string& where,
                                  bool allow_exceeding_format_limits = false) {
    validate_encoding(e, where.c_str(), allow_exceeding_format_limits);
    if (!video_codec_allowed_for_target(target, e.video_codec)) {
        if (target == DiscTarget::UltraHdBluRay2160)
            throw std::runtime_error(where + " must use HEVC/H.265 or AVC/H.264 video for Ultra HD Blu-ray");
        if (target == DiscTarget::DvdVideo480p)
            throw std::runtime_error(where + " must use MPEG-2 video for DVD-Video");
        throw std::runtime_error(where + " must use H.264/AVC or MPEG-2 video for standard Blu-ray");
    }
    if (!audio_codec_allowed_for_target(target, e.audio_codec))
        throw std::runtime_error(where + " DVD-Video audio must use AC-3 or LPCM; DTS/DCA and TrueHD are not supported by BDMV Author's DVD target");
    const int max_video = video_codec_max_bitrate_kbps(target, e.video_codec);
    if (!allow_exceeding_format_limits && e.video_bitrate_kbps > max_video)
        throw std::runtime_error(where + " video bitrate exceeds the target maximum of " + std::to_string(max_video) + " kb/s");
    if (!allow_exceeding_format_limits && e.video_max_bitrate_kbps > max_video)
        throw std::runtime_error(where + " video maxrate exceeds the target maximum of " + std::to_string(max_video) + " kb/s");
    if (!allow_exceeding_format_limits && e.video_min_bitrate_kbps < video_codec_min_bitrate_kbps(target, e.video_codec))
        throw std::runtime_error(where + " video minrate is below the target minimum");
    if (!allow_exceeding_format_limits && target == DiscTarget::DvdVideo480p && e.audio_codec == AudioCodec::Ac3 && e.ac3_bitrate_kbps > 448)
        throw std::runtime_error(where + " DVD-Video AC-3 bitrate must not exceed 448 kb/s");
    if (!allow_exceeding_format_limits && lossless_audio_codec(e.audio_codec) && !lossless_sample_rate_allowed_for_target(target, e.lpcm_sample_rate_hz))
        throw std::runtime_error(where + " lossless audio sample rate is not legal for the selected output target");
    try { validate_advanced_codec_options(e,target); }
    catch (const std::exception& error) { throw std::runtime_error(where + " " + error.what()); }
}

void validate_codec_for_output_mode(const EncodingProfile& e, DiscTarget target, const VideoTimingMode& timing, const std::string& where) {
    if (target != DiscTarget::UltraHdBluRay2160 || e.video_codec != VideoCodec::X264) return;
    const bool rate_ok = !timing.interlaced &&
        (std::abs(timing.frame_rate_value - 24000.0/1001.0) < 0.02 || std::abs(timing.frame_rate_value - 24.0) < 0.02);
    if (timing.video_width != 1920 || timing.video_height != 1080 || timing.aspect_ratio != "16:9" || !rate_ok)
        throw std::runtime_error(where + " AVC/H.264 in Ultra HD Blu-ray v3 is limited to 1920x1080 16:9 at 23.976p or 24p");
}

void normalize_button_image(const ToolPaths& tools, const fs::path& input, const fs::path& output,
                            int width, int height) {
    fs::create_directories(output.parent_path());
    std::ostringstream filter;
    filter << "scale=" << width << ":" << height
           << ":force_original_aspect_ratio=decrease:flags=lanczos,format=rgba,pad="
           << width << ":" << height << ":(ow-iw)/2:(oh-ih)/2:color=black@0.0";
    std::string c = shq_s(tools.ffmpeg) + " -hide_banner -loglevel error -y -i " + shq(input) +
                    " -frames:v 1 -an -sn -dn -vf " + shq_s(filter.str()) +
                    " -pix_fmt rgba -f rawvideo " + shq(output);
    run(c);
    const auto expected = static_cast<std::uintmax_t>(width) * static_cast<std::uintmax_t>(height) * 4U;
    std::error_code ec;
    const auto actual = fs::file_size(output, ec);
    if (ec || actual != expected)
        throw std::runtime_error("FFmpeg produced an invalid normalized menu button image: " + output.string());
}

std::string rgb_hex(const Rgba& c);

int scaled_design_x(int value, int target_width, const std::string& aspect_ratio = "16:9") {
    if (aspect_ratio == "4:3") {
        constexpr int aperture_x = (kProjectDesignWidth - (kProjectDesignHeight * 4 / 3)) / 2;
        constexpr int aperture_width = kProjectDesignHeight * 4 / 3;
        return static_cast<int>(std::lround(static_cast<double>(value - aperture_x) * target_width / aperture_width));
    }
    return static_cast<int>(std::lround(static_cast<double>(value) * target_width / kProjectDesignWidth));
}
int scaled_design_width(int value, int target_width, const std::string& aspect_ratio = "16:9") {
    const int source_width = aspect_ratio == "4:3" ? kProjectDesignHeight * 4 / 3 : kProjectDesignWidth;
    return static_cast<int>(std::lround(static_cast<double>(value) * target_width / source_width));
}
int scaled_design_y(int value, int target_height) {
    return static_cast<int>(std::lround(static_cast<double>(value) * target_height / kProjectDesignHeight));
}
void scale_button_style(ButtonStyle& style, int target_height) {
    const double factor = static_cast<double>(target_height) / kProjectDesignHeight;
    auto scale = [factor](int value, int minimum = 0) {
        return std::max(minimum, static_cast<int>(std::lround(value * factor)));
    };
    style.font_size_px = scale(style.font_size_px, 1);
    style.corner_radius = scale(style.corner_radius);
    style.normal.border_width = scale(style.normal.border_width);
    style.selected.border_width = scale(style.selected.border_width);
    style.activated.border_width = scale(style.activated.border_width);
}
Rect clip_scaled_rect(Rect r, int target_width, int target_height) {
    const int x1 = std::clamp(r.x, 0, std::max(0, target_width - 1));
    const int y1 = std::clamp(r.y, 0, std::max(0, target_height - 1));
    const int x2 = std::clamp(r.x + std::max(1, r.width), x1 + 1, target_width);
    const int y2 = std::clamp(r.y + std::max(1, r.height), y1 + 1, target_height);
    return {x1,y1,std::max(1,x2-x1),std::max(1,y2-y1)};
}
Menu scale_menu_from_design(const Menu& source, int target_width, int target_height,
                            const std::string& aspect_ratio = "16:9") {
    Menu out = source;
    out.width = target_width;
    out.height = target_height;
    scale_button_style(out.default_button_style, target_height);
    auto scale_button = [&](MenuButton& b) {
        b.bounds = clip_scaled_rect({scaled_design_x(b.bounds.x,target_width,aspect_ratio), scaled_design_y(b.bounds.y,target_height),
                    scaled_design_width(b.bounds.width,target_width,aspect_ratio), scaled_design_y(b.bounds.height,target_height)},
                    target_width,target_height);
        if (b.use_custom_style) scale_button_style(b.style, target_height);
    };
    for (auto& b : out.buttons) scale_button(b);
    scale_button(out.back_button);
    for (auto& o : out.overlays) {
        o.bounds = clip_scaled_rect({scaled_design_x(o.bounds.x,target_width,aspect_ratio), scaled_design_y(o.bounds.y,target_height),
                    scaled_design_width(o.bounds.width,target_width,aspect_ratio), scaled_design_y(o.bounds.height,target_height)},
                    target_width,target_height);
        o.font_size_px = std::max(1,scaled_design_y(o.font_size_px,target_height));
    }
    out.submenus.clear();
    return out;
}

Menu prepare_menu_button_images(const ToolPaths& tools, const Menu& source, const fs::path& work) {
    Menu menu = source;
    for (std::size_t i = 0; i < menu.buttons.size(); ++i) {
        auto& b = menu.buttons[i];
        if (b.kind != MenuButtonKind::Image) continue;
        const int width = std::max(16, b.bounds.width);
        const int height = std::max(12, b.bounds.height);
        std::ostringstream base;
        base << "menu-button-" << std::setw(3) << std::setfill('0') << (i + 1U);
        const auto normal = work / (base.str() + "-normal.rgba");
        normalize_button_image(tools, b.normal_image, normal, width, height);
        b.normal_image = normal;
        if (!b.selected_image.empty()) {
            const auto selected = work / (base.str() + "-selected.rgba");
            normalize_button_image(tools, b.selected_image, selected, width, height);
            b.selected_image = selected;
        }
    }
    return menu;
}

void alpha_over(std::uint8_t* dst, const std::uint8_t* src) {
    const unsigned sa = src[3];
    if (sa == 0U) return;
    if (sa == 255U) {
        std::copy_n(src, 4, dst);
        return;
    }
    const unsigned da = dst[3];
    const unsigned inv = 255U - sa;
    const unsigned out_a = sa + (da * inv + 127U) / 255U;
    if (out_a == 0U) { std::fill_n(dst, 4, static_cast<std::uint8_t>(0)); return; }
    for (int c = 0; c < 3; ++c) {
        const unsigned premul = static_cast<unsigned>(src[c]) * sa +
                                (static_cast<unsigned>(dst[c]) * da * inv + 127U) / 255U;
        dst[c] = static_cast<std::uint8_t>(std::min(255U, (premul + out_a / 2U) / out_a));
    }
    dst[3] = static_cast<std::uint8_t>(std::min(255U, out_a));
}

std::vector<std::uint8_t> read_rgba(const fs::path& p, int width, int height) {
    const auto expected = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U;
    std::ifstream f(p, std::ios::binary);
    if (!f) throw std::runtime_error("cannot read RGBA image: " + p.string());
    std::vector<std::uint8_t> out(expected);
    f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
    if (f.gcount() != static_cast<std::streamsize>(out.size()))
        throw std::runtime_error("short RGBA image: " + p.string());
    return out;
}

void composite_rgba(std::vector<std::uint8_t>& canvas, int canvas_w, int canvas_h,
                    const std::vector<std::uint8_t>& src, int src_w, int src_h, int x0, int y0) {
    for (int y = 0; y < src_h; ++y) {
        const int dy = y0 + y;
        if (dy < 0 || dy >= canvas_h) continue;
        for (int x = 0; x < src_w; ++x) {
            const int dx = x0 + x;
            if (dx < 0 || dx >= canvas_w) continue;
            const auto si = (static_cast<std::size_t>(y) * static_cast<std::size_t>(src_w) + static_cast<std::size_t>(x)) * 4U;
            const auto di = (static_cast<std::size_t>(dy) * static_cast<std::size_t>(canvas_w) + static_cast<std::size_t>(dx)) * 4U;
            alpha_over(canvas.data() + di, src.data() + si);
        }
    }
}

fs::path prepare_menu_background(const ToolPaths& tools, const Menu& menu, const fs::path& work) {
    if (menu.overlays.empty()) return menu.background_image;
    const int W = menu.width, H = menu.height;
    std::vector<std::uint8_t> canvas(static_cast<std::size_t>(W) * static_cast<std::size_t>(H) * 4U);
    for (std::size_t i = 0; i < canvas.size(); i += 4U) {
        canvas[i] = menu.background_color.r; canvas[i+1] = menu.background_color.g;
        canvas[i+2] = menu.background_color.b; canvas[i+3] = 255U;
    }
    if (!menu.background_image.empty()) {
        const auto base = work / "menu-background-base.rgba";
        normalize_button_image(tools, menu.background_image, base, W, H);
        const auto rgba = read_rgba(base, W, H);
        composite_rgba(canvas, W, H, rgba, W, H, 0, 0);
    }
    for (std::size_t i = 0; i < menu.overlays.size(); ++i) {
        const auto& o = menu.overlays[i];
        const int ow = o.bounds.width, oh = o.bounds.height;
        if (o.kind == MenuOverlayKind::Image) {
            const auto raw = work / ("menu-label-image-" + std::to_string(i + 1U) + ".rgba");
            normalize_button_image(tools, o.image, raw, ow, oh);
            composite_rgba(canvas, W, H, read_rgba(raw, ow, oh), ow, oh, o.bounds.x, o.bounds.y);
        } else {
            ButtonStyle style;
            style.font_family = o.font_family; style.font_size_px = o.font_size_px;
            style.bold = o.bold; style.italic = o.italic;
            const auto mask = detail::render_button_text_mask(o.text, style, ow, oh, 0);
            const std::array<std::uint8_t,4> px{o.text_color.r,o.text_color.g,o.text_color.b,o.text_color.a};
            for (int y = 0; y < oh; ++y) for (int x = 0; x < ow; ++x) {
                if (!mask.pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(ow) + static_cast<std::size_t>(x)]) continue;
                const int dx=o.bounds.x+x, dy=o.bounds.y+y;
                if(dx<0||dx>=W||dy<0||dy>=H) continue;
                const auto di=(static_cast<std::size_t>(dy)*static_cast<std::size_t>(W)+static_cast<std::size_t>(dx))*4U;
                alpha_over(canvas.data()+di, px.data());
            }
        }
    }
    const auto raw = work / "menu-composited.rgba";
    { std::ofstream f(raw, std::ios::binary); if(!f) throw std::runtime_error("cannot write "+raw.string()); f.write(reinterpret_cast<const char*>(canvas.data()), static_cast<std::streamsize>(canvas.size())); }
    const auto png = work / "menu-composited.png";
    const std::string c = shq_s(tools.ffmpeg) + " -hide_banner -loglevel error -y -f rawvideo -pixel_format rgba -video_size " + std::to_string(W) + "x" + std::to_string(H) + " -i " + shq(raw) + " -frames:v 1 " + shq(png);
    run(c);
    return png;
}

fs::path prepare_menu_overlay(const ToolPaths& tools, const Menu& menu, const fs::path& work) {
    if (menu.overlays.empty()) return {};
    const int W = menu.width, H = menu.height;
    std::vector<std::uint8_t> canvas(static_cast<std::size_t>(W) * static_cast<std::size_t>(H) * 4U, 0U);
    for (std::size_t i = 0; i < menu.overlays.size(); ++i) {
        const auto& o = menu.overlays[i];
        const int ow = o.bounds.width, oh = o.bounds.height;
        if (o.kind == MenuOverlayKind::Image) {
            const auto raw = work / ("menu-video-label-image-" + std::to_string(i + 1U) + ".rgba");
            normalize_button_image(tools, o.image, raw, ow, oh);
            composite_rgba(canvas, W, H, read_rgba(raw, ow, oh), ow, oh, o.bounds.x, o.bounds.y);
        } else {
            ButtonStyle style;
            style.font_family = o.font_family; style.font_size_px = o.font_size_px;
            style.bold = o.bold; style.italic = o.italic;
            const auto mask = detail::render_button_text_mask(o.text, style, ow, oh, 0);
            const std::array<std::uint8_t,4> px{o.text_color.r,o.text_color.g,o.text_color.b,o.text_color.a};
            for (int y = 0; y < oh; ++y) for (int x = 0; x < ow; ++x) {
                if (!mask.pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(ow) + static_cast<std::size_t>(x)]) continue;
                const int dx=o.bounds.x+x, dy=o.bounds.y+y;
                if(dx<0||dx>=W||dy<0||dy>=H) continue;
                const auto di=(static_cast<std::size_t>(dy)*static_cast<std::size_t>(W)+static_cast<std::size_t>(dx))*4U;
                alpha_over(canvas.data()+di, px.data());
            }
        }
    }
    const auto raw = work / "menu-video-overlay.rgba";
    { std::ofstream f(raw, std::ios::binary); if(!f) throw std::runtime_error("cannot write "+raw.string()); f.write(reinterpret_cast<const char*>(canvas.data()), static_cast<std::streamsize>(canvas.size())); }
    const auto png = work / "menu-video-overlay.png";
    const std::string c = shq_s(tools.ffmpeg) + " -hide_banner -loglevel error -y -f rawvideo -pixel_format rgba -video_size " + std::to_string(W) + "x" + std::to_string(H) + " -i " + shq(raw) + " -frames:v 1 " + shq(png);
    run(c);
    return png;
}


fs::path write_rgba_png(const ToolPaths& tools, const fs::path& raw, const fs::path& png,
                        const std::vector<std::uint8_t>& rgba, int width, int height) {
    fs::create_directories(raw.parent_path());
    {
        std::ofstream f(raw, std::ios::binary);
        if (!f) throw std::runtime_error("cannot write " + raw.string());
        f.write(reinterpret_cast<const char*>(rgba.data()), static_cast<std::streamsize>(rgba.size()));
        if (!f) throw std::runtime_error("failed while writing " + raw.string());
    }
    run(shq_s(tools.ffmpeg) + " -hide_banner -loglevel error -y -f rawvideo -pixel_format rgba -video_size " +
        std::to_string(width) + "x" + std::to_string(height) + " -i " + shq(raw) + " -frames:v 1 " + shq(png));
    return png;
}

const ButtonStyle& dvd_button_style(const Menu& menu, const MenuButton& button) {
    return button.use_custom_style ? button.style : menu.default_button_style;
}

bool dvd_rounded_inside(int x, int y, int w, int h, int radius) {
    if (radius <= 0) return true;
    if ((x >= radius && x < w - radius) || (y >= radius && y < h - radius)) return true;
    const int cx = x < radius ? radius - 1 : w - radius;
    const int cy = y < radius ? radius - 1 : h - radius;
    const long long dx = static_cast<long long>(x - cx);
    const long long dy = static_cast<long long>(y - cy);
    return dx * dx + dy * dy <= static_cast<long long>(radius) * radius;
}

std::vector<std::uint8_t> dvd_text_button_rgba(const Menu& menu, const MenuButton& button, int state) {
    const int w = std::max(1, button.bounds.width);
    const int h = std::max(1, button.bounds.height);
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4U, 0U);
    const auto& style = dvd_button_style(menu, button);
    const auto& visual = state == 0 ? style.normal : (state == 1 ? style.selected : style.activated);
    const int border = std::clamp(visual.border_width, 0, std::min(w, h) / 2);
    const int radius = std::clamp(style.corner_radius, 0, std::min(w, h) / 2);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (!dvd_rounded_inside(x, y, w, h, radius)) continue;
            bool is_border = false;
            if (border > 0) {
                if (x < border || y < border || x >= w - border || y >= h - border) is_border = true;
                else if (radius > border && !dvd_rounded_inside(x - border, y - border,
                                                                 w - 2 * border, h - 2 * border,
                                                                 radius - border)) is_border = true;
            }
            const auto& c = is_border ? visual.border_color : visual.background_color;
            const auto i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)) * 4U;
            rgba[i] = c.r; rgba[i + 1U] = c.g; rgba[i + 2U] = c.b; rgba[i + 3U] = c.a;
        }
    }
    const int padding = std::max(3, border + 4);
    const auto mask = detail::render_button_text_mask(button.label, style, w, h, padding);
    if (mask.width == w && mask.height == h && mask.pixels.size() == static_cast<std::size_t>(w) * static_cast<std::size_t>(h)) {
        for (std::size_t i = 0; i < mask.pixels.size(); ++i) {
            if (!mask.pixels[i]) continue;
            auto* px = rgba.data() + i * 4U;
            const std::array<std::uint8_t,4> src{visual.text_color.r, visual.text_color.g,
                                                 visual.text_color.b, visual.text_color.a};
            alpha_over(px, src.data());
        }
    }
    return rgba;
}

std::vector<std::uint8_t> dvd_image_button_rgba(const ToolPaths& tools, const MenuButton& button,
                                                 const fs::path& work, std::size_t index) {
    const int w = std::max(1, button.bounds.width);
    const int h = std::max(1, button.bounds.height);
    const auto raw = work / ("dvd-button-" + std::to_string(index + 1U) + "-normal.rgba");
    normalize_button_image(tools, button.normal_image, raw, w, h);
    return read_rgba(raw, w, h);
}

void dvd_draw_palette_safe_highlight(std::vector<std::uint8_t>& canvas, int canvas_w, int canvas_h,
                                     const Rect& bounds, bool activated) {
    // DVD subpictures are palette constrained.  Use one common highlight color
    // for all buttons so custom normal-state artwork remains visible while the
    // selectable overlay stays safely within the SPU palette budget.
    const std::array<std::uint8_t,4> color = activated
        ? std::array<std::uint8_t,4>{255,255,255,220}
        : std::array<std::uint8_t,4>{128,200,255,200};
    const int border = activated ? 5 : 3;
    for (int y = 0; y < bounds.height; ++y) {
        const int dy = bounds.y + y;
        if (dy < 0 || dy >= canvas_h) continue;
        for (int x = 0; x < bounds.width; ++x) {
            const int dx = bounds.x + x;
            if (dx < 0 || dx >= canvas_w) continue;
            if (x >= border && y >= border && x < bounds.width - border && y < bounds.height - border) continue;
            const auto di = (static_cast<std::size_t>(dy) * static_cast<std::size_t>(canvas_w) + static_cast<std::size_t>(dx)) * 4U;
            alpha_over(canvas.data() + di, color.data());
        }
    }
}

struct DvdMenuButtonVisuals {
    fs::path normal_overlay;
    fs::path highlight;
    fs::path select;
};

DvdMenuButtonVisuals prepare_dvd_menu_button_visuals(const ToolPaths& tools, const Menu& menu,
                                                       const fs::path& work) {
    const int w = menu.width, h = menu.height;
    std::vector<std::uint8_t> normal(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4U, 0U);
    std::vector<std::uint8_t> highlight(normal.size(), 0U);
    std::vector<std::uint8_t> select(normal.size(), 0U);
    for (std::size_t i = 0; i < menu.buttons.size(); ++i) {
        const auto& button = menu.buttons[i];
        std::vector<std::uint8_t> button_rgba;
        if (button.kind == MenuButtonKind::Image) button_rgba = dvd_image_button_rgba(tools, button, work, i);
        else button_rgba = dvd_text_button_rgba(menu, button, 0);
        composite_rgba(normal, w, h, button_rgba, button.bounds.width, button.bounds.height,
                       button.bounds.x, button.bounds.y);
        dvd_draw_palette_safe_highlight(highlight, w, h, button.bounds, false);
        dvd_draw_palette_safe_highlight(select, w, h, button.bounds, true);
    }
    DvdMenuButtonVisuals out;
    out.normal_overlay = write_rgba_png(tools, work / "dvd-menu-buttons-normal.rgba",
                                        work / "dvd-menu-buttons-normal.png", normal, w, h);
    out.highlight = write_rgba_png(tools, work / "dvd-menu-buttons-highlight.rgba",
                                   work / "dvd-menu-buttons-highlight.png", highlight, w, h);
    out.select = write_rgba_png(tools, work / "dvd-menu-buttons-select.rgba",
                                work / "dvd-menu-buttons-select.png", select, w, h);
    return out;
}

fs::path combine_rgba_overlays(const ToolPaths& tools, const fs::path& lower, const fs::path& upper,
                               int width, int height, const fs::path& work) {
    if (lower.empty()) return upper;
    if (upper.empty()) return lower;
    const auto lower_raw = work / "dvd-overlay-lower.rgba";
    const auto upper_raw = work / "dvd-overlay-upper.rgba";
    normalize_button_image(tools, lower, lower_raw, width, height);
    normalize_button_image(tools, upper, upper_raw, width, height);
    auto canvas = read_rgba(lower_raw, width, height);
    const auto top = read_rgba(upper_raw, width, height);
    composite_rgba(canvas, width, height, top, width, height, 0, 0);
    return write_rgba_png(tools, work / "dvd-menu-combined-overlay.rgba",
                          work / "dvd-menu-combined-overlay.png", canvas, width, height);
}

struct VideoColorPlan {
    std::string filter_prefix;
    std::string ffmpeg_output_flags;
    std::string x265_params;
    std::string description;
};

bool color_value_unspecified(const std::string& value) {
    return value.empty() || value == "unknown" || value == "unspecified" || value == "reserved" || value == "N/A";
}

VideoStreamInfo probe_source_color(const ToolPaths& tools, const fs::path& input) {
    VideoStreamInfo info;
    if (input.empty()) return info;
    std::ostringstream command;
    command << shq_s(tools.ffprobe)
            << " -v error -select_streams v:0 -show_entries stream=color_primaries,color_transfer,color_space,color_range"
               " -of default=nw=1:nk=0 " << shq(input) << " 2>" << null_device();
    stream_command_lines(command.str(), [&](const std::string& raw) {
        const auto line = trim_copy(raw);
        const auto eq = line.find('=');
        if (eq == std::string::npos) return;
        const auto key = line.substr(0, eq);
        const auto value = line.substr(eq + 1);
        if (key == "color_primaries") info.color_primaries = value;
        else if (key == "color_transfer") info.color_transfer = value;
        else if (key == "color_space") info.color_space = value;
        else if (key == "color_range") info.color_range = value;
    });
    return info;
}

bool source_color_all_unspecified(const VideoStreamInfo& i) {
    return color_value_unspecified(i.color_primaries) && color_value_unspecified(i.color_transfer) &&
           color_value_unspecified(i.color_space);
}
bool source_color_bt709_or_unspecified(const VideoStreamInfo& i) {
    const auto bt709_or_unknown = [](const std::string& value) {
        return color_value_unspecified(value) || value == "bt709";
    };
    return bt709_or_unknown(i.color_primaries) && bt709_or_unknown(i.color_transfer) &&
           bt709_or_unknown(i.color_space);
}
bool source_color_bt709_needs_tagging(const VideoStreamInfo& i) {
    return source_color_bt709_or_unspecified(i) &&
           (color_value_unspecified(i.color_primaries) || color_value_unspecified(i.color_transfer) ||
            color_value_unspecified(i.color_space));
}
bool source_range_is_full(const VideoStreamInfo& i) {
    return i.color_range == "pc" || i.color_range == "jpeg" || i.color_range == "full";
}
std::string ffmpeg_setparams_color_input(const VideoStreamInfo& i) {
    auto assumed = [](const std::string& value, const char* fallback) {
        return color_value_unspecified(value) ? std::string(fallback) : value;
    };
    std::string range = i.color_range;
    if (color_value_unspecified(range)) range = "limited";
    else if (range == "tv" || range == "mpeg") range = "limited";
    else if (range == "pc" || range == "jpeg") range = "full";
    return "setparams=range=" + range +
           ":color_primaries=" + assumed(i.color_primaries, "bt709") +
           ":color_trc=" + assumed(i.color_transfer, "bt709") +
           ":colorspace=" + assumed(i.color_space, "bt709") + ",";
}
bool source_color_bt2020_sdr(const VideoStreamInfo& i) {
    return i.color_primaries == "bt2020" &&
           (i.color_transfer == "bt2020-10" || i.color_transfer == "bt2020_10") &&
           (i.color_space == "bt2020nc" || i.color_space == "bt2020_ncl");
}
bool source_color_bt2020_pq(const VideoStreamInfo& i) {
    return i.color_primaries == "bt2020" && i.color_transfer == "smpte2084" &&
           (i.color_space == "bt2020nc" || i.color_space == "bt2020_ncl");
}

VideoColorPlan video_color_plan(const ToolPaths& tools, const fs::path& input, DiscTarget target, VideoCodec codec,
                                bool still, bool has_overlay) {
    VideoColorPlan plan;
    const VideoStreamInfo source = still ? VideoStreamInfo{} : probe_source_color(tools, input);
    const bool untagged = still || source_color_all_unspecified(source);
    const bool assumed_bt709 = still || source_color_bt709_or_unspecified(source);
    const bool force_bt709 = target != DiscTarget::UltraHdBluRay2160 || codec == VideoCodec::X264 || has_overlay;

    auto set_bt709 = [&] {
        plan.ffmpeg_output_flags = "-color_primaries bt709 -color_trc bt709 -colorspace bt709 -color_range tv ";
        plan.x265_params = "colorprim=bt709:transfer=bt709:colormatrix=bt709:range=limited";
    };
    auto set_bt2020_sdr = [&] {
        plan.ffmpeg_output_flags = "-color_primaries bt2020 -color_trc bt2020-10 -colorspace bt2020nc -color_range tv ";
        plan.x265_params = "colorprim=bt2020:transfer=bt2020-10:colormatrix=bt2020nc:range=limited";
    };
    auto set_bt2020_pq = [&] {
        plan.ffmpeg_output_flags = "-color_primaries bt2020 -color_trc smpte2084 -colorspace bt2020nc -color_range tv ";
        plan.x265_params = "colorprim=bt2020:transfer=smpte2084:colormatrix=bt2020nc:range=limited";
    };

    if (assumed_bt709) {
        set_bt709();
        if (source_range_is_full(source)) {
            plan.filter_prefix = ffmpeg_setparams_color_input(source) + "colorspace=all=bt709:range=tv,";
            plan.description = "BT.709 full-range source converted to BT.709 limited-range SDR";
        } else if (untagged) plan.description = "untagged source assumed BT.709 SDR";
        else if (source_color_bt709_needs_tagging(source))
            plan.description = "partially tagged BT.709 source completed as BT.709 SDR";
        else plan.description = "BT.709 SDR";
        return plan;
    }

    if (source_color_bt2020_sdr(source)) {
        if (force_bt709) {
            plan.filter_prefix = "zscale=pin=bt2020:tin=bt2020-10:min=bt2020nc:rin=limited:p=bt709:t=bt709:m=bt709:r=limited,";
            set_bt709();
            plan.description = "BT.2020 SDR converted to BT.709 SDR";
        } else {
            set_bt2020_sdr();
            plan.description = "BT.2020 SDR preserved";
        }
        return plan;
    }

    if (source_color_bt2020_pq(source)) {
        if (force_bt709) {
            // Standard Blu-ray is SDR BT.709. Convert BT.2020/PQ to linear light,
            // tone-map HDR to SDR, gamut-map to BT.709, then apply BT.709 transfer.
            plan.filter_prefix = "zscale=pin=bt2020:tin=smpte2084:min=bt2020nc:rin=limited:t=linear:npl=100,"
                                 "format=gbrpf32le,zscale=p=bt709,tonemap=tonemap=hable:desat=0,"
                                 "zscale=t=bt709:m=bt709:r=limited,";
            set_bt709();
            plan.description = "BT.2020/ST 2084 HDR tone-mapped and converted to BT.709 SDR";
        } else {
            set_bt2020_pq();
            plan.description = "BT.2020/ST 2084 HDR preserved";
        }
        return plan;
    }

    // For other explicitly tagged SDR sources, ask FFmpeg's colorspace filter
    // to perform a real conversion instead of merely relabeling the samples.
    // This is primarily useful for SD/legacy sources authored to HD/UHD.
    plan.filter_prefix = ffmpeg_setparams_color_input(source) + "colorspace=all=bt709:range=tv,";
    set_bt709();
    plan.description = "explicit non-UHD color description converted to BT.709 SDR";
    return plan;
}

std::string video_input(const ToolPaths& tools, const fs::path& input, const Project& p, VideoCodec codec,
                        const VideoTimingMode& timing, bool source_interlaced,
                        bool still, double seconds, const fs::path& progress_file,
                        const Rgba& background_color, const VideoColorPlan& color_plan, bool loop_input = false,
                        const fs::path& overlay_image = {}, bool hold_last_frame = false) {
    std::ostringstream c;
    c << shq_s(tools.ffmpeg) << " -hide_banner -loglevel error -y -nostats -stats_period 0.25 -progress "
      << shq(progress_file) << " ";
    const auto geometry = target_geometry_for_timing(p.target, timing);
    const int output_width = geometry.video_width, output_height = geometry.video_height;
    if (still) {
        if (input.empty()) c << "-f lavfi -i \"color=c=" << rgb_hex(background_color) << ":s=" << output_width << "x" << output_height << ":r=" << timing.temporal_rate << "\" ";
        else c << "-loop 1 -i " << shq(input) << " ";
    } else {
        if (loop_input) c << "-stream_loop -1 ";
        c << "-i " << shq(input) << " ";
    }
    if (!overlay_image.empty()) c << "-loop 1 -i " << shq(overlay_image) << " ";
    if (still || loop_input || hold_last_frame) c << "-t " << seconds << " ";
    const std::string size = std::to_string(output_width) + ":" + std::to_string(output_height);
    std::string base_filter;
    // Blu-ray's high-rate 1080 modes are interlaced.  When the source is
    // interlaced, first reconstruct one progressive picture per source field
    // so scaling/color conversion never mixes the two temporal fields.  If the
    // target is interlaced, pair those progressive temporal samples back into
    // TFF frames after scaling.  Progressive 50/59.94 sources therefore retain
    // every source picture as a distinct output field instead of dropping half.
    if (!still && source_interlaced)
        base_filter += "bwdif=mode=send_field:parity=auto:deint=all,";
    // A non-looping menu with audio longer than its video must keep a video
    // frame alive until the audio finishes. Clone the final decoded frame
    // instead of replaying the video from its beginning.
    if (!still && hold_last_frame)
        base_filter += "tpad=stop_mode=clone:stop_duration=" + std::to_string(seconds) + ",";
    const std::string sar = timing.sample_aspect_ratio.empty() ? "1/1" : timing.sample_aspect_ratio;
    const std::string target_dar = timing.aspect_ratio == "4:3" ? "4/3" : "16/9";
    if (still) {
        // Still-menu composition has already been rendered in the selected
        // coded raster. Preserve that raster and attach its display SAR.
        base_filter += color_plan.filter_prefix + "scale=" + size + ":flags=lanczos,setsar=" + sar + ",";
    } else {
        // Fit in display-aspect space, not coded-pixel space. This is required
        // for anamorphic 1440x1080 Blu-ray and every SD/DVD mode, including 4:3.
        base_filter += color_plan.filter_prefix +
            "scale=w='if(gt(dar," + target_dar + ")," + std::to_string(output_width) +
            ",trunc((" + std::to_string(output_height) + "*dar/(" + sar + "))/2)*2)'"
            ":h='if(gt(dar," + target_dar + "),trunc((" + std::to_string(output_width) + "*(" + sar + ")/dar)/2)*2," +
            std::to_string(output_height) + ")':flags=lanczos,pad=" + size +
            ":(ow-iw)/2:(oh-ih)/2:color=" + rgb_hex(background_color) + ",setsar=" + sar + ",";
    }
    if (timing.interlaced)
        base_filter += "fps=" + timing.temporal_rate + ",tinterlace=mode=interleave_top,setfield=tff";
    else
        base_filter += "fps=" + timing.frame_rate;
    const std::string pixel_format = (target_is_uhd(p.target) && codec == VideoCodec::Hevc) ? "yuv420p10le" : "yuv420p";
    if (overlay_image.empty()) {
        c << "-map 0:v:0 -vf \"" << base_filter << ",format=" << pixel_format << "\" -pix_fmt " << pixel_format << " "
          << color_plan.ffmpeg_output_flags << "-an ";
    } else {
        c << "-filter_complex \"[0:v]" << base_filter
          << "[base];[1:v]scale=" << output_width << ":" << output_height
          << ":flags=lanczos,format=rgba[overlay];[base][overlay]overlay=0:0:format=auto,format=" << pixel_format << "[v]\" "
             "-map \"[v]\" -pix_fmt " << pixel_format << " " << color_plan.ffmpeg_output_flags << "-an ";
    }
    return c.str();
}

int default_keyframe_interval_frames(const EncodingProfile& e, DiscTarget target, const VideoTimingMode& timing) {
    if (target == DiscTarget::DvdVideo480p) return dvd_timing_is_pal(timing) ? 15 : 18;
    if (e.video_codec == VideoCodec::Mpeg2) return timing.frame_rate_value > 26.0 ? 15 : 12;
    return std::max(1, static_cast<int>(std::ceil(timing.frame_rate_value)));
}

int maximum_keyframe_interval_frames(const EncodingProfile& e, DiscTarget target, const VideoTimingMode& timing) {
    return maximum_keyframe_interval_for_rate(target,e.video_codec,timing.frame_rate_value,dvd_timing_is_pal(timing));
}

int effective_keyframe_interval_frames(const EncodingProfile& e, DiscTarget target, const VideoTimingMode& timing) {
    const int requested = e.keyframe_interval_frames;
    const int value = requested > 0 ? requested : default_keyframe_interval_frames(e,target,timing);
    const int maximum = maximum_keyframe_interval_frames(e,target,timing);
    if (value < 1 || value > maximum) {
        throw std::runtime_error("keyframe/GOP interval " + std::to_string(value) +
            " frames exceeds the legal maximum " + std::to_string(maximum) + " for " + timing.description);
    }
    return value;
}

std::string ffmpeg_private_options(const std::string& text) {
    std::ostringstream out;
    for (const auto& option : parse_advanced_codec_options(text)) {
        out << " -" << option.name;
        if (option.has_value) out << " " << shq_s(option.value);
    }
    return out.str();
}

std::string x264_private_options(const std::string& text) {
    std::ostringstream out;
    for (const auto& option : parse_advanced_codec_options(text)) {
        out << " --" << option.name;
        if (option.has_value) out << " " << shq_s(option.value);
    }
    return out.str();
}

std::string x264_private_params(const std::string& text) {
    std::ostringstream out;
    for (const auto& option : parse_advanced_codec_options(text)) {
        out << ":" << option.name << "=" << (option.has_value ? option.value : "1");
    }
    return out.str();
}

std::string x265_private_params(const std::string& text) {
    std::ostringstream out;
    for (const auto& option : parse_advanced_codec_options(text)) {
        out << ":" << option.name << "=" << (option.has_value ? option.value : "1");
    }
    return out.str();
}

std::string x264_options(const EncodingProfile& e, DiscTarget target, const VideoTimingMode& timing, int peak_bitrate_limit_kbps) {
    std::ostringstream x;
    const int keyint = effective_keyframe_interval_frames(e,target,timing);
    std::string sar = timing.sample_aspect_ratio.empty() ? "1/1" : timing.sample_aspect_ratio;
    std::replace(sar.begin(),sar.end(),'/',':');
    x << " --demuxer y4m --preset " << e.x264_preset
      << " --profile high --level 4.1 --bluray-compat --aud --nal-hrd vbr"
         " --vbv-maxrate " << peak_bitrate_limit_kbps << " --vbv-bufsize 30000 --keyint " << keyint << " --min-keyint 1"
         " --bframes 3 --ref 4 --colorprim bt709 --transfer bt709 --colormatrix bt709 --range tv --sar " << sar << " --bitrate "
      << e.video_bitrate_kbps << " --fps " << timing.frame_rate;
    if (timing.interlaced) x << " --tff";
    else if (timing.fake_interlaced) x << " --fake-interlaced";
    x << x264_private_options(e.x264_advanced_options);
    return x.str();
}

std::string x265_options(const EncodingProfile& e, DiscTarget target, const VideoTimingMode& timing,
                         const VideoColorPlan& color_plan, int peak_bitrate_limit_kbps) {
    std::ostringstream x;
    const int keyint = effective_keyframe_interval_frames(e,target,timing);
    x << " --y4m --preset " << e.x265_preset
      << " --profile main10 --level-idc 5.1 --high-tier --uhd-bd --aud --repeat-headers --hrd"
         " --vbv-maxrate " << peak_bitrate_limit_kbps << " --vbv-bufsize " << peak_bitrate_limit_kbps << " --keyint " << keyint << " --min-keyint 1"
      << " --output-depth 10 --bitrate " << e.video_bitrate_kbps << " --fps " << timing.frame_rate;
    for (const auto& option : parse_advanced_codec_options(e.x265_advanced_options)) {
        x << " --" << option.name;
        if (option.has_value) x << " " << shq_s(option.value);
    }
    // x265 accepts the same color signaling names used in -x265-params.
    std::istringstream color(color_plan.x265_params);
    for (std::string token; std::getline(color, token, ':');) {
        const auto eq=token.find('=');
        if(eq==std::string::npos)continue;
        x << " --" << token.substr(0,eq) << " " << token.substr(eq+1);
    }
    return x.str();
}

std::string encode_video(const ToolPaths& tools, const fs::path& input, const fs::path& output_base,
                         const EncodingProfile& e, const Project& p, const VideoTimingMode& timing,
                         bool source_interlaced, bool still, double seconds,
                         double expected_duration, const VideoProgressCallback& progress,
                         const Rgba& background_color = {0,0,0,255}, bool loop_input = false,
                         const fs::path& overlay_image = {}, bool hold_last_frame = false,
                         int peak_bitrate_limit_kbps = 0) {
    const int passes = e.two_pass ? 2 : 1;
    if (peak_bitrate_limit_kbps <= 0) {
        peak_bitrate_limit_kbps = target_is_dvd(p.target) && e.video_codec == VideoCodec::Mpeg2
            ? dvd_mpeg2_vbr_peak_kbps(e.video_bitrate_kbps, e.video_max_bitrate_kbps, 0, "DVD MPEG-2")
            : e.video_max_bitrate_kbps;
    }
    if (peak_bitrate_limit_kbps < e.video_bitrate_kbps)
        throw std::runtime_error("video maxrate/peak limit is below the configured average video bitrate");
    if (e.video_min_bitrate_kbps > peak_bitrate_limit_kbps)
        throw std::runtime_error("video minrate exceeds the effective maxrate/peak limit");
    auto run_pass = [&](int pass, const fs::path& progress_file, const std::string& command) {
        run_with_ffmpeg_progress(command, progress_file, expected_duration, [&](double pass_fraction) {
            if (progress) {
                const double overall = (static_cast<double>(pass - 1) + pass_fraction) /
                                       static_cast<double>(passes);
                progress(overall, pass, passes);
            }
        });
    };

    const auto color_plan = video_color_plan(tools, input, p.target, e.video_codec, still, !overlay_image.empty());

    if (e.video_codec == VideoCodec::X264) {
        const fs::path output = output_base.string() + ".264";
        if (tools.h264_provider == VideoEncoderProvider::Standalone) {
            if (e.video_min_bitrate_kbps > 0)
                throw std::runtime_error("standalone x264 does not expose a minrate control; select the FFmpeg/libx264 provider or set minrate to 0");
            const std::string common = shq_s(tools.x264) + x264_options(e, p.target, timing, peak_bitrate_limit_kbps);
            if (e.two_pass) {
                const fs::path stats = output_base.string() + ".x264.stats";
                const fs::path pass1 = output_base.string() + ".pass1.264";
                const fs::path progress1 = output_base.string() + ".pass1.progress";
                const fs::path progress2 = output_base.string() + ".pass2.progress";
                const std::string y4m1 = video_input(tools, input, p, e.video_codec, timing, source_interlaced, still, seconds, progress1, background_color, color_plan, loop_input, overlay_image, hold_last_frame) + "-f yuv4mpegpipe - | ";
                const std::string y4m2 = video_input(tools, input, p, e.video_codec, timing, source_interlaced, still, seconds, progress2, background_color, color_plan, loop_input, overlay_image, hold_last_frame) + "-f yuv4mpegpipe - | ";
                run_pass(1, progress1, y4m1 + common + " --pass 1 --stats " + shq(stats) + " --output " + shq(pass1) + " -");
                run_pass(2, progress2, y4m2 + common + " --pass 2 --stats " + shq(stats) + " --output " + shq(output) + " -");
                std::error_code ec;
                fs::remove(pass1, ec);
                fs::remove(stats, ec);
                fs::remove(stats.string() + ".mbtree", ec);
            } else {
                const fs::path progress_file = output_base.string() + ".progress";
                const std::string y4m = video_input(tools, input, p, e.video_codec, timing, source_interlaced, still, seconds, progress_file, background_color, color_plan, loop_input, overlay_image, hold_last_frame) + "-f yuv4mpegpipe - | ";
                run_pass(1, progress_file, y4m + common + " --output " + shq(output) + " -");
            }
        } else {
            auto h264_options = [&](int pass, const fs::path& stats) {
                std::ostringstream opts;
                const int keyint = effective_keyframe_interval_frames(e,p.target,timing);
                opts << "-c:v libx264 -preset " << e.x264_preset
                     << " -b:v " << e.video_bitrate_kbps << "k -minrate " << e.video_min_bitrate_kbps
                     << "k -maxrate " << peak_bitrate_limit_kbps << "k -bufsize 30000k"
                        " -profile:v high -level:v 4.1 -pix_fmt yuv420p -g " << keyint
                     << " -keyint_min 1 -bf 3 -refs 4 ";
                std::ostringstream params;
                params << "bluray-compat=1:aud=1:nal-hrd=vbr:force-cfr=1:bframes=3:ref=4"
                          ":colorprim=bt709:transfer=bt709:colormatrix=bt709:range=tv";
                if(timing.interlaced) params << ":tff=1";
                else if(timing.fake_interlaced) params << ":fake-interlaced=1";
                params << x264_private_params(e.x264_advanced_options);
                opts << "-x264-params " << shq_s(params.str()) << " ";
                if(pass>0) opts << "-pass " << pass << " -passlogfile " << shq(stats) << " ";
                return opts.str();
            };
            if (e.two_pass) {
                const fs::path stats = output_base.string() + ".x264-ffmpeg.stats";
                const fs::path pass1 = output_base.string() + ".pass1.264";
                const fs::path progress1 = output_base.string() + ".pass1.progress";
                const fs::path progress2 = output_base.string() + ".pass2.progress";
                const std::string base1 = video_input(tools, input, p, e.video_codec, timing, source_interlaced, still, seconds, progress1, background_color, color_plan, loop_input, overlay_image, hold_last_frame);
                const std::string base2 = video_input(tools, input, p, e.video_codec, timing, source_interlaced, still, seconds, progress2, background_color, color_plan, loop_input, overlay_image, hold_last_frame);
                run_pass(1, progress1, base1 + h264_options(1,stats) + "-f h264 " + shq(pass1));
                run_pass(2, progress2, base2 + h264_options(2,stats) + "-f h264 " + shq(output));
                std::error_code ec;
                fs::remove(pass1,ec); fs::remove(stats.string()+"-0.log",ec); fs::remove(stats.string()+"-0.log.mbtree",ec);
            } else {
                const fs::path progress_file = output_base.string() + ".progress";
                const std::string base = video_input(tools, input, p, e.video_codec, timing, source_interlaced, still, seconds, progress_file, background_color, color_plan, loop_input, overlay_image, hold_last_frame);
                run_pass(1, progress_file, base + h264_options(0,{}) + "-f h264 " + shq(output));
            }
        }
        return video_meta_for(output,e,timing);
    }

    if (e.video_codec == VideoCodec::Hevc) {
        const fs::path output = output_base.string() + ".265";
        if (tools.hevc_provider == VideoEncoderProvider::Standalone) {
            if (e.video_min_bitrate_kbps > 0)
                throw std::runtime_error("standalone x265 does not expose a minrate control; select the FFmpeg/libx265 provider or set minrate to 0");
            const std::string common = shq_s(tools.x265) + x265_options(e,p.target,timing,color_plan,peak_bitrate_limit_kbps);
            if (e.two_pass) {
                const fs::path stats = output_base.string() + ".x265.stats";
                const fs::path pass1 = output_base.string() + ".pass1.265";
                const fs::path progress1 = output_base.string() + ".pass1.progress";
                const fs::path progress2 = output_base.string() + ".pass2.progress";
                const std::string y4m1 = video_input(tools, input, p, e.video_codec, timing, source_interlaced, still, seconds, progress1, background_color, color_plan, loop_input, overlay_image, hold_last_frame) + "-f yuv4mpegpipe - | ";
                const std::string y4m2 = video_input(tools, input, p, e.video_codec, timing, source_interlaced, still, seconds, progress2, background_color, color_plan, loop_input, overlay_image, hold_last_frame) + "-f yuv4mpegpipe - | ";
                run_pass(1, progress1, y4m1 + common + " --pass 1 --stats " + shq(stats) + " --output " + shq(pass1) + " -");
                run_pass(2, progress2, y4m2 + common + " --pass 2 --stats " + shq(stats) + " --output " + shq(output) + " -");
                std::error_code ec;
                fs::remove(pass1,ec); fs::remove(stats,ec); fs::remove(stats.string()+".cutree",ec);
            } else {
                const fs::path progress_file = output_base.string() + ".progress";
                const std::string y4m = video_input(tools, input, p, e.video_codec, timing, source_interlaced, still, seconds, progress_file, background_color, color_plan, loop_input, overlay_image, hold_last_frame) + "-f yuv4mpegpipe - | ";
                run_pass(1, progress_file, y4m + common + " --output " + shq(output) + " -");
            }
        } else {
            auto hevc_options = [&](int pass, const fs::path& stats) {
                std::ostringstream opts;
                opts << "-c:v libx265 -preset " << e.x265_preset
                     << " -b:v " << e.video_bitrate_kbps << "k -minrate " << e.video_min_bitrate_kbps
                     << "k -maxrate " << peak_bitrate_limit_kbps << "k -bufsize " << peak_bitrate_limit_kbps << "k"
                        " -profile:v main10 -level:v 5.1 -pix_fmt yuv420p10le ";
                std::ostringstream params;
                const int keyint = effective_keyframe_interval_frames(e,p.target,timing);
                params << "uhd-bd=1:aud=1:repeat-headers=1:hrd=1:vbv-maxrate=" << peak_bitrate_limit_kbps << ":vbv-bufsize=" << peak_bitrate_limit_kbps
                          << ":keyint=" << keyint << ":min-keyint=1:" << color_plan.x265_params
                       << x265_private_params(e.x265_advanced_options);
                if (pass > 0) params << ":pass=" << pass << ":stats=" << stats.string();
                opts << "-x265-params " << shq_s(params.str()) << " ";
                return opts.str();
            };
            if (e.two_pass) {
                const fs::path stats = output_base.string() + ".x265.stats";
                const fs::path pass1 = output_base.string() + ".pass1.265";
                const fs::path progress1 = output_base.string() + ".pass1.progress";
                const fs::path progress2 = output_base.string() + ".pass2.progress";
                const std::string base1 = video_input(tools, input, p, e.video_codec, timing, source_interlaced, still, seconds, progress1, background_color, color_plan, loop_input, overlay_image, hold_last_frame);
                const std::string base2 = video_input(tools, input, p, e.video_codec, timing, source_interlaced, still, seconds, progress2, background_color, color_plan, loop_input, overlay_image, hold_last_frame);
                run_pass(1, progress1, base1 + hevc_options(1,stats) + "-f hevc " + shq(pass1));
                run_pass(2, progress2, base2 + hevc_options(2,stats) + "-f hevc " + shq(output));
                std::error_code ec;
                fs::remove(pass1,ec); fs::remove(stats,ec); fs::remove(stats.string()+".cutree",ec);
            } else {
                const fs::path progress_file = output_base.string() + ".progress";
                const std::string base = video_input(tools, input, p, e.video_codec, timing, source_interlaced, still, seconds, progress_file, background_color, color_plan, loop_input, overlay_image, hold_last_frame);
                run_pass(1, progress_file, base + hevc_options(0,{}) + "-f hevc " + shq(output));
            }
        }
        return video_meta_for(output,e,timing);
    }

    const fs::path output = output_base.string() + ".m2v";
    std::ostringstream opts;
    const bool dvd_video = target_is_dvd(p.target);
    if (dvd_video) {
        // Match the timing/geometry families behind FFmpeg's film-dvd,
        // ntsc-dvd and pal-dvd presets while retaining BDMV Author's user
        // bitrate and elementary-stream workflow.  PAL uses a 15-picture GOP;
        // Film/NTSC use 18.  Only interlaced modes enable interlaced DCT/ME.
        const int dvd_gop = effective_keyframe_interval_frames(e,p.target,timing);
        opts << "-c:v mpeg2video -b:v " << e.video_bitrate_kbps
             << "k -minrate " << e.video_min_bitrate_kbps << "k -maxrate " << peak_bitrate_limit_kbps << "k -bufsize 1835008 -g " << dvd_gop
             << " -bf 2 -mpv_flags +strict_gop"
                " -aspect " << timing.aspect_ratio << " -color_primaries bt709 -color_trc bt709 -colorspace bt709 -color_range tv ";
        if (timing.interlaced) opts << "-flags +ildct+ilme -field_order tt ";
    } else {
        // Blu-ray MPEG-2 is Main Profile @ High Level. Its MPEG-2 VBV limit is
        // 9,781,248 bits. FFmpeg does not clamp an oversized MPEG-2 VBV to the
        // profile/level limit, so pass the exact MP@HL value.
        const int mpeg2_gop = effective_keyframe_interval_frames(e,p.target,timing);
        opts << "-c:v mpeg2video -b:v " << e.video_bitrate_kbps
             << "k -minrate " << e.video_min_bitrate_kbps
             << "k -maxrate " << peak_bitrate_limit_kbps << "k -bufsize 9781248 -g " << mpeg2_gop << " -bf 2 -mpv_flags +strict_gop"
                " -aspect " << timing.aspect_ratio << " -color_primaries bt709 -color_trc bt709 -colorspace bt709 -color_range tv ";
        if (timing.interlaced) opts << "-flags +ildct+ilme -field_order tt ";
    }
    opts << ffmpeg_private_options(e.mpeg2_advanced_options) << " ";
    if (e.two_pass) {
        const fs::path stats = output_base.string() + ".mpeg2-pass";
        const fs::path pass1 = output_base.string() + ".pass1.m2v";
        const fs::path progress1 = output_base.string() + ".pass1.progress";
        const fs::path progress2 = output_base.string() + ".pass2.progress";
        const std::string base1 = video_input(tools, input, p, e.video_codec, timing, source_interlaced, still, seconds, progress1, background_color, color_plan, loop_input, overlay_image, hold_last_frame);
        const std::string base2 = video_input(tools, input, p, e.video_codec, timing, source_interlaced, still, seconds, progress2, background_color, color_plan, loop_input, overlay_image, hold_last_frame);
        run_pass(1, progress1, base1 + opts.str() + "-pass 1 -passlogfile " + shq(stats) + " -f mpeg2video " + shq(pass1));
        run_pass(2, progress2, base2 + opts.str() + "-pass 2 -passlogfile " + shq(stats) + " -f mpeg2video " + shq(output));
        std::error_code ec;
        fs::remove(pass1, ec);
        fs::remove(stats.string() + "-0.log", ec);
        fs::remove(stats.string() + "-0.log.mbtree", ec);
    } else {
        const fs::path progress_file = output_base.string() + ".progress";
        const std::string base = video_input(tools, input, p, e.video_codec, timing, source_interlaced, still, seconds, progress_file, background_color, color_plan, loop_input, overlay_image, hold_last_frame);
        run_pass(1, progress_file, base + opts.str() + "-f mpeg2video " + shq(output));
    }
    return video_meta_for(output,e,timing);
}

void write_binary(const fs::path& p, const std::vector<unsigned char>& b) {
    std::ofstream f(p, std::ios::binary);
    if (!f) throw std::runtime_error("cannot write " + p.string());
    f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    if (!f) throw std::runtime_error("failed while writing " + p.string());
}

std::string audio_input(const ToolPaths& tools, const fs::path& source, bool loop, double seconds, int ordinal) {
    std::ostringstream c;
    c << shq_s(tools.ffmpeg) << " -hide_banner -loglevel error -y ";
    if (loop) c << "-stream_loop -1 ";
    c << "-i " << shq(source) << " -map 0:a:" << ordinal << " ";
    if (loop) c << "-t " << seconds << " ";
    else if (seconds > 0.0) c << "-af apad -t " << seconds << " ";
    return c.str();
}

struct EncodedAudioResult {
    std::string meta;
    fs::path path;
    int peak_bitrate_kbps = 0;
};

EncodedAudioResult encode_audio(const ToolPaths& tools, const fs::path& source, const fs::path& dir,
                                const EncodingProfile& e, DiscTarget target, const std::string& language,
                                bool loop, double seconds, int ordinal = 0, int requested_channels = 0) {
    const int src_channels = audio_channels(tools, source, dir, ordinal);
    const auto source_info = probe_audio_stream(tools, source, dir / "lossless-format-probe", ordinal, seconds);
    EncodingProfile resolved = e;
    if (lossless_audio_codec(resolved.audio_codec)) {
        resolved.lpcm_sample_rate_hz = resolved_lossless_sample_rate_hz(target, e.lpcm_sample_rate_hz, source_info.sample_rate);
        resolved.lpcm_bit_depth = resolved_lossless_bit_depth(target, e.lpcm_bit_depth, source_info.bits_per_sample);
    }
    const int channels = resolved_audio_output_channels(src_channels, resolved, target, requested_channels, "audio stream");
    const std::string base = audio_input(tools, source, loop, seconds, ordinal);
    if (resolved.audio_codec == AudioCodec::Lpcm) {
        const fs::path out = dir / "audio.wav";
        const std::string pcm = resolved.lpcm_bit_depth == 24 ? "pcm_s24le" : "pcm_s16le";
        run(base + "-c:a " + pcm + " -ar " + std::to_string(resolved.lpcm_sample_rate_hz) + " -ac " + std::to_string(channels) +
            ffmpeg_private_options(resolved.lpcm_advanced_options) + " " + shq(out));
        return {"A_LPCM, " + shq(out) + ", lang=" + language + "\n", out, fixed_encoded_audio_peak_kbps(resolved, target, channels)};
    }
    if (resolved.audio_codec == AudioCodec::Dca) {
        const fs::path out = dir / "audio.dts";
        run(base + "-strict -2 -c:a dca -b:a " + std::to_string(e.dca_bitrate_kbps) +
            "k -ar 48000 -ac " + std::to_string(channels) + ffmpeg_private_options(e.dca_advanced_options) + " -f dts " + shq(out));
        return {"A_DTS, " + shq(out) + ", lang=" + language + "\n", out, fixed_encoded_audio_peak_kbps(e, target, channels)};
    }
    if (resolved.audio_codec == AudioCodec::TrueHdAc3) {
        const fs::path thd = dir / "audio.truehd";
        const fs::path core = dir / "audio-core.ac3";
        const fs::path merged = dir / "audio.thd+ac3";
        run(base + "-strict -2 -c:a truehd -ar " + std::to_string(resolved.lpcm_sample_rate_hz) +
            " -sample_fmt " + std::string(resolved.lpcm_bit_depth == 24 ? "s32p" : "s16p") + " -ac " + std::to_string(channels) +
            ffmpeg_private_options(resolved.truehd_advanced_options) + " -f truehd " + shq(thd));
        run(base + "-c:a ac3 -b:a " + std::to_string(resolved.ac3_bitrate_kbps) +
            "k -ar 48000 -ac " + std::to_string(channels) + ffmpeg_private_options(resolved.ac3_advanced_options) + " -f ac3 " + shq(core));
        const int peak_bitrate_kbps = detail::merge_truehd_ac3_core(thd, core, merged);
        return {"A_AC3, " + shq(merged) + ", lang=" + language + "\n", merged, peak_bitrate_kbps};
    }

    const fs::path out = dir / "audio.ac3";
    run(base + "-c:a ac3 -b:a " + std::to_string(e.ac3_bitrate_kbps) +
        "k -ar 48000 -ac " + std::to_string(channels) + ffmpeg_private_options(e.ac3_advanced_options) + " -f ac3 " + shq(out));
    return {"A_AC3, " + shq(out) + ", lang=" + language + "\n", out, fixed_encoded_audio_peak_kbps(e, target, channels)};
}

void tsmux(const ToolPaths& t, const fs::path& meta, const fs::path& out) {
    fs::create_directories(out);
    const auto log = out.parent_path() / (out.filename().string() + ".tsmuxer.log");
    std::string c = shq_s(tsmuxer_command(t)) + " " + shq(meta) + " " + shq(out) + " > " + shq(log) + " 2>&1";
    const int rc = shell_system(c);
    if (rc != 0) {
        auto detail = read_small_text(log);
        throw std::runtime_error("tsMuxer failed (" + command_status(rc) + "): " + shq_s(tsmuxer_command(t)) +
                                 " " + shq(meta) + " " + shq(out) +
                                 (detail.empty() ? std::string() : "\n\n--- tsMuxer output ---\n" + detail));
    }
}

std::string chapter_time(double t) {
    if (t < 0) throw std::runtime_error("chapter time cannot be negative");
    auto ms = static_cast<long long>(t * 1000.0 + 0.5);
    const auto h = ms / 3600000;
    ms %= 3600000;
    const auto m = ms / 60000;
    ms %= 60000;
    const auto sec = ms / 1000;
    ms %= 1000;
    std::ostringstream o;
    o << std::setw(2) << std::setfill('0') << h << ":" << std::setw(2) << m << ":" << std::setw(2) << sec << "."
      << std::setw(3) << ms;
    return o.str();
}


struct ResolvedMenu {
    Menu menu;
    std::string parent_id;
};

std::string xml_escape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 16U);
    for (char ch : text) {
        switch (ch) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '\"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default: out += ch; break;
        }
    }
    return out;
}

std::string dvd_language(std::string language) {
    language = normalized_language(std::move(language), "eng");
    static const std::unordered_map<std::string,std::string> map = {
        {"eng","en"},{"fra","fr"},{"fre","fr"},{"deu","de"},{"ger","de"},
        {"spa","es"},{"ita","it"},{"por","pt"},{"jpn","ja"},{"zho","zh"},
        {"chi","zh"},{"kor","ko"},{"rus","ru"},{"nld","nl"},{"dut","nl"},
        {"swe","sv"},{"nor","no"},{"dan","da"},{"fin","fi"},{"pol","pl"},
        {"ces","cs"},{"cze","cs"},{"hun","hu"},{"ell","el"},{"gre","el"}
    };
    if (const auto it = map.find(language); it != map.end()) return it->second;
    return language.size() >= 2U ? language.substr(0,2) : std::string("en");
}

std::string dvd_time(double seconds) {
    if (seconds < 0.0) seconds = 0.0;
    const auto total_ms = static_cast<long long>(std::llround(seconds * 1000.0));
    const auto h = total_ms / 3600000LL;
    const auto m = (total_ms / 60000LL) % 60LL;
    const auto sec = (total_ms / 1000LL) % 60LL;
    const auto ms = total_ms % 1000LL;
    std::ostringstream out;
    out << std::setw(2) << std::setfill('0') << h << ':' << std::setw(2) << m << ':'
        << std::setw(2) << sec << '.' << std::setw(3) << ms;
    return out.str();
}

std::string dvd_chapters(const std::vector<double>& chapters) {
    std::ostringstream out;
    out << "0";
    for (double chapter : chapters) out << ',' << dvd_time(chapter);
    return out.str();
}

std::string dvd_audio_format(AudioCodec codec) {
    return codec == AudioCodec::Lpcm ? "pcm" : "ac3";
}

struct DvdAudioElementary {
    fs::path path;
    int channels = 2;
    AudioCodec codec = AudioCodec::Ac3;
    int sample_rate_hz = 48000;
    int bit_depth = 16;
};

fs::path dvd_audio_file(const fs::path& dir, AudioCodec codec) {
    // mplex expects headerless, big-endian LPCM samples rather than a WAV file.
    return dir / (codec == AudioCodec::Lpcm ? "audio.lpcm" : "audio.ac3");
}

std::string dvd_audio_cache_extension(AudioCodec codec) {
    return codec == AudioCodec::Lpcm ? ".lpcm" : ".ac3";
}

std::string dvd_audio_cache_key(const MediaFingerprint& fp, const EncodingProfile& e, int source_ordinal, int output_channels) {
    std::ostringstream k;
    k << "bdmvauthor-dvd-audio-cache-v4\n" << fp.canonical()
      << "\ncodec=" << audio_codec_name(e.audio_codec)
      << "\nsourceAudioOrdinal=" << source_ordinal << "\noutputChannels=" << output_channels
      << "\nac3Bitrate=" << e.ac3_bitrate_kbps << "\nac3Advanced=" << e.ac3_advanced_options
      << "\nlpcmAdvanced=" << e.lpcm_advanced_options << "\nlpcmSampleRate=" << e.lpcm_sample_rate_hz
      << "\nlpcmBitDepth=" << e.lpcm_bit_depth << "\n"
      << "dvdLpcm=raw-big-endian-for-mplex-v2\n";
    return sha256_hex(k.str());
}

DvdAudioElementary encode_dvd_audio_elementary(const ToolPaths& tools, const fs::path& source, const fs::path& dir,
                                                 const EncodingProfile& encoding, bool loop, double seconds, int ordinal,
                                                 int requested_channels = 0) {
    fs::create_directories(dir);
    const int source_channels = audio_channels(tools, source, dir, ordinal);
    const int channels = resolved_audio_output_channels(source_channels, encoding, DiscTarget::DvdVideo480p,
                                                        requested_channels, "DVD audio stream");
    std::ostringstream command;
    command << shq_s(tools.ffmpeg) << " -hide_banner -loglevel error -y ";
    if (loop) command << "-stream_loop -1 ";
    command << "-i " << shq(source) << " -map 0:a:" << ordinal << ' ';
    if (loop) command << "-t " << std::fixed << std::setprecision(3) << seconds << ' ';
    else if (seconds > 0.0) command << "-af apad -t " << std::fixed << std::setprecision(3) << seconds << ' ';
    if (encoding.audio_codec == AudioCodec::Lpcm) {
        // mplex -f 8 needs raw signed big-endian LPCM plus an explicit -L format.
        const bool depth24 = encoding.lpcm_bit_depth == 24;
        command << "-c:a " << (depth24 ? "pcm_s24be" : "pcm_s16be")
                << " -ar " << encoding.lpcm_sample_rate_hz << " -ac " << channels
                << ffmpeg_private_options(encoding.lpcm_advanced_options)
                << " -f " << (depth24 ? "s24be " : "s16be ") << shq(dvd_audio_file(dir, encoding.audio_codec));
    } else {
        command << "-c:a ac3 -b:a " << encoding.ac3_bitrate_kbps << "k -ar 48000 -ac "
                << channels << ffmpeg_private_options(encoding.ac3_advanced_options)
                << " -f ac3 " << shq(dvd_audio_file(dir, encoding.audio_codec));
    }
    run(command.str());
    return {dvd_audio_file(dir, encoding.audio_codec), channels, encoding.audio_codec,
            encoding.audio_codec == AudioCodec::Lpcm ? encoding.lpcm_sample_rate_hz : 48000,
            encoding.audio_codec == AudioCodec::Lpcm ? encoding.lpcm_bit_depth : 16};
}

DvdAudioElementary encode_dvd_silence(const ToolPaths& tools, const fs::path& dir, const EncodingProfile& encoding,
                                       double duration_seconds) {
    fs::create_directories(dir);
    std::ostringstream command;
    command << shq_s(tools.ffmpeg) << " -hide_banner -loglevel error -y -f lavfi -i "
            << shq_s("anullsrc=r=48000:cl=stereo") << " -t " << std::fixed << std::setprecision(3)
            << std::max(0.1, duration_seconds) << ' ';
    if (encoding.audio_codec == AudioCodec::Lpcm) {
        const bool depth24 = encoding.lpcm_bit_depth == 24;
        command << "-c:a " << (depth24 ? "pcm_s24be" : "pcm_s16be") << " -ar " << encoding.lpcm_sample_rate_hz
                << " -ac 2" << ffmpeg_private_options(encoding.lpcm_advanced_options)
                << " -f " << (depth24 ? "s24be " : "s16be ") << shq(dvd_audio_file(dir, encoding.audio_codec));
    }
    else
        command << "-c:a ac3 -b:a " << encoding.ac3_bitrate_kbps << "k -ar 48000 -ac 2"
                << ffmpeg_private_options(encoding.ac3_advanced_options) << " -f ac3 "
                << shq(dvd_audio_file(dir, encoding.audio_codec));
    run(command.str());
    return {dvd_audio_file(dir, encoding.audio_codec), 2, encoding.audio_codec,
            encoding.audio_codec == AudioCodec::Lpcm ? encoding.lpcm_sample_rate_hz : 48000,
            encoding.audio_codec == AudioCodec::Lpcm ? encoding.lpcm_bit_depth : 16};
}

void mux_dvd_program_stream(const ToolPaths& tools, const fs::path& video,
                            const std::vector<DvdAudioElementary>& audio,
                            const fs::path& output, int mux_bitrate_kbps) {
    // dvdauthor requires a DVD program stream with an empty NAV sector at every
    // VOBU/GOP boundary so it can fill the DSI/PCI navigation data.  FFmpeg's
    // stream-copy DVD muxer only places an initial NAV pair for this elementary-
    // stream workflow, which collapses a whole title into one VOBU and prevents
    // normal time seeking. mjpegtools mplex -f 8 is dvdauthor's documented muxer
    // path for producing the required recurring NAV sectors.
    std::error_code ec;
    fs::remove(output, ec);
    std::ostringstream command;
    command << shq_s(tools.mplex) << " -f 8 -r " << std::max(1, mux_bitrate_kbps) << " -o " << shq(output) << ' ';
    const bool has_lpcm = std::any_of(audio.begin(), audio.end(), [](const DvdAudioElementary& a) { return a.codec == AudioCodec::Lpcm; });
    if (has_lpcm) {
        command << "-L ";
        bool first = true;
        for (const auto& a : audio) {
            if (a.codec != AudioCodec::Lpcm) continue;
            if (!first) command << ',';
            first = false;
            command << a.sample_rate_hz << ':' << a.channels << ':' << a.bit_depth;
        }
        command << ' ';
    }
    command << shq(video);
    for (const auto& a : audio) command << ' ' << shq(a.path);
    run(command.str());
}

fs::path extract_dvd_text_subtitle(const ToolPaths& tools, const fs::path& source, const fs::path& dir,
                                   const SourceTrackDescriptor& track) {
    if (track.codec_name == "hdmv_pgs_subtitle")
        throw std::runtime_error("DVD-Video output cannot convert PGS subtitles; use a text subtitle source for DVD rendering");
    fs::create_directories(dir);
    const auto out = dir / "subtitle.srt";
    const fs::path subtitle_source = track.external_source.empty() ? source : track.external_source;
    const int subtitle_ordinal = track.external_source.empty() ? track.source_ordinal : 0;
    run(shq_s(tools.ffmpeg) + " -hide_banner -loglevel error -y -i " + shq(subtitle_source) +
        " -map 0:s:" + std::to_string(subtitle_ordinal) + " -vn -an -dn -c:s srt -f srt " + shq(out));
    return out;
}

fs::path spumux_dvd_menu(const ToolPaths& tools, const fs::path& input, const fs::path& output,
                         const fs::path& xml, const Menu& menu, const DvdMenuButtonVisuals& visuals,
                         const VideoTimingMode& timing) {
    std::ostringstream spec;
    spec << "<subpictures format=\"" << dvd_spumux_format(timing) << "\"><stream><spu start=\"00:00:00.00\" highlight=\""
         << xml_escape(visuals.highlight.string()) << "\" select=\"" << xml_escape(visuals.select.string())
         << "\" force=\"yes\">";
    for (std::size_t i = 0; i < menu.buttons.size(); ++i) {
        const auto& b = menu.buttons[i];
        int y0 = std::clamp(b.bounds.y, 0, menu.height - 1) & ~1;
        int y1 = std::clamp(b.bounds.y + b.bounds.height, y0 + 2, menu.height);
        if (y1 & 1) --y1;
        if (y1 <= y0) y1 = std::min(menu.height, y0 + 2);
        const int x0 = std::clamp(b.bounds.x, 0, menu.width - 1);
        const int x1 = std::clamp(b.bounds.x + b.bounds.width, x0 + 1, menu.width);
        spec << "<button name=\"b" << (i + 1U) << "\" x0=\"" << x0 << "\" y0=\"" << y0
             << "\" x1=\"" << x1 << "\" y1=\"" << y1 << "\"/>";
    }
    spec << "</spu></stream></subpictures>\n";
    write_text(xml, spec.str());
    run(shq_s(tools.spumux) + " " + shq(xml) + " < " + shq(input) + " > " + shq(output));
    return output;
}

fs::path spumux_dvd_text_subtitle(const ToolPaths& tools, const fs::path& input, const fs::path& output,
                                  const fs::path& xml, const fs::path& subtitle, std::size_t stream_index,
                                  const VideoTimingMode& timing, const SubtitleStyle& style) {
    const auto geometry = target_geometry_for_timing(DiscTarget::DvdVideo480p, timing);
    const SubtitleStyle defaults{};const auto& st=style.override_style?style:defaults;
    // spumux 0.7.x built with FreeType but without Fontconfig treats its
    // font= attribute as a font *file*, not a family name. Resolve the family
    // here so DVD subtitle rendering behaves the same regardless of whether
    // the external spumux binary itself was compiled with Fontconfig.
    const auto dvd_font_file = detail::resolve_font_file(st.font_family, st.bold, st.italic);
    const int dvd_font_size = subtitle_units_to_pixels(st.font_size_px, geometry.graphics_height, 4);
    const int default_bottom_margin = subtitle_units_to_pixels(defaults.bottom_offset_px, geometry.graphics_height);
    std::ostringstream spec;
    spec << "<subpictures format=\"" << dvd_spumux_format(timing) << "\"><stream><textsub filename=\"" << xml_escape(subtitle.string())
         << "\" characterset=\"UTF-8\" fontsize=\"" << dvd_font_size
         << "\" font=\"" << xml_escape(dvd_font_file)
         << "\" fill-color=\"" << (style.override_style?subtitle_spumux_color(st.font_color):"white")
         << "\" outline-color=\"" << (style.override_style?subtitle_spumux_color(st.dvd_outline_color):"black")
         << "\" outline-thickness=\"" << (style.override_style?st.border_width:2.0)
         << "\" shadow-offset=\"" << (style.override_style?st.dvd_shadow_offset_x:0) << ", " << (style.override_style?st.dvd_shadow_offset_y:0)
         << "\" shadow-color=\"" << (style.override_style?subtitle_spumux_color(st.dvd_shadow_color):"black")
         << "\" horizontal-alignment=\"" << (style.override_style?st.dvd_horizontal_alignment:"center")
         << "\" vertical-alignment=\"" << (style.override_style?st.dvd_vertical_alignment:"bottom")
         << "\" left-margin=\"" << (style.override_style?st.dvd_left_margin_px:40) << "\" right-margin=\"" << (style.override_style?st.dvd_right_margin_px:40)
         << "\" top-margin=\"" << (style.override_style?st.dvd_top_margin_px:20) << "\" bottom-margin=\"" << (style.override_style?st.dvd_bottom_margin_px:default_bottom_margin)
         << "\" movie-width=\"" << geometry.video_width << "\" movie-height=\"" << geometry.video_height << "\" movie-fps=\"" << timing.tsmuxer_fps
         << "\" aspect=\"" << timing.aspect_ratio << "\"";
    if(style.override_style&&st.dvd_force_display)spec<<" force=\"yes\"";
    spec << "/></stream></subpictures>\n";
    write_text(xml, spec.str());
    run(shq_s(tools.spumux) + " -s " + std::to_string(stream_index) + " " + shq(xml) +
        " < " + shq(input) + " > " + shq(output));
    return output;
}

struct DvdContinuationNode { std::size_t id = 0; std::string commands; };

class DvdNavigationCompiler {
public:
    DvdNavigationCompiler(const std::vector<ResolvedMenu>& menus, std::size_t title_count)
        : menus_(menus), title_count_(title_count) {
        for (std::size_t i = 0; i < menus_.size(); ++i) menu_numbers_[menus_[i].menu.id] = i + 1U;
    }

    std::string compile(std::span<const NavigationAction> actions, std::size_t return_menu) {
        const std::vector<NavigationAction> source(actions.begin(), actions.end());
        const auto expanded = expand_navigation_repeat_groups(source);
        if (!expanded.has_infinite_loop()) return compile_from(expanded.actions, 0U, return_menu, 0U);

        // A forever group gets one VMGM continuation PGC that is also its loop
        // target. Titles inside it return through the normal dispatcher, and
        // reaching the end jumps back to this continuation without duplicating
        // any VOB or title asset.
        const std::size_t loop_index = continuations_.size();
        const std::size_t loop_id = loop_index + 1U;
        continuations_.push_back({loop_id,{}});
        std::vector<NavigationAction> body(expanded.actions.begin() + static_cast<std::ptrdiff_t>(expanded.infinite_loop_start),
                                           expanded.actions.end());
        continuations_[loop_index].commands = compile_from(body, 0U, return_menu, loop_id);
        std::vector<NavigationAction> prefix(expanded.actions.begin(),
                                             expanded.actions.begin() + static_cast<std::ptrdiff_t>(expanded.infinite_loop_start));
        return compile_from(prefix, 0U, return_menu, loop_id);
    }

    const std::vector<DvdContinuationNode>& continuations() const { return continuations_; }

private:
    std::string continuation_jump(std::size_t continuation) const {
        return "jump vmgm menu " + std::to_string(menus_.size() + continuation) + ";";
    }

    std::string compile_from(std::span<const NavigationAction> actions, std::size_t start,
                             std::size_t return_menu, std::size_t end_continuation) {
        std::ostringstream out;
        for (std::size_t i = start; i < actions.size(); ++i) {
            const auto& action = actions[i];
            switch (action.kind) {
                case NavigationActionKind::AudioTrack:
                    if (action.repeat_count == 0U) throw std::runtime_error("DVD stream-selection action cannot repeat forever");
                    for (std::uint16_t r = 0; r < std::max<std::uint16_t>(1U, action.repeat_count); ++r)
                        out << "g2 = " << action.target_stream << "; ";
                    break;
                case NavigationActionKind::SubtitleTrack:
                    if (action.repeat_count == 0U) throw std::runtime_error("DVD stream-selection action cannot repeat forever");
                    for (std::uint16_t r = 0; r < std::max<std::uint16_t>(1U, action.repeat_count); ++r)
                        out << "g3 = " << action.target_stream << "; ";
                    break;
                case NavigationActionKind::SubtitleOff:
                    if (action.repeat_count == 0U) throw std::runtime_error("DVD stream-selection action cannot repeat forever");
                    for (std::uint16_t r = 0; r < std::max<std::uint16_t>(1U, action.repeat_count); ++r) out << "g3 = 33; ";
                    break;
                case NavigationActionKind::Menu: {
                    if (action.repeat_count != 1U) throw std::runtime_error("DVD menu jump cannot be repeated");
                    const auto it = menu_numbers_.find(action.target_menu_id);
                    if (it == menu_numbers_.end()) throw std::runtime_error("DVD navigation targets unknown menu: " + action.target_menu_id);
                    out << "jump vmgm menu " << it->second << ";";
                    return out.str();
                }
                case NavigationActionKind::PlayTitle: {
                    if (action.target_title < 1U || action.target_title > title_count_)
                        throw std::runtime_error("DVD navigation has invalid title target");
                    const std::size_t continuation_index = continuations_.size();
                    const std::size_t continuation_id = continuation_index + 1U;
                    continuations_.push_back({continuation_id,{}});
                    auto launch = [&](std::size_t continuation) {
                        std::ostringstream c; c << "g1 = 1; g0 = " << continuation << "; ";
                        if (action.target_chapter > 1U) c << "g4 = " << action.target_chapter << "; jump titleset " << action.target_title << " menu;";
                        else c << "g4 = 0; jump title " << action.target_title << ";";
                        return c.str();
                    };
                    if (action.repeat_count == 0U) {
                        continuations_[continuation_index].commands = launch(continuation_id);
                    } else if (action.repeat_count > 1U) {
                        std::vector<NavigationAction> remaining;
                        remaining.reserve(static_cast<std::size_t>(action.repeat_count - 1U) + actions.size() - i - 1U);
                        for (std::uint16_t r = 1; r < action.repeat_count; ++r) { auto copy = action; copy.repeat_count = 1U; remaining.push_back(std::move(copy)); }
                        remaining.insert(remaining.end(), actions.begin() + static_cast<std::ptrdiff_t>(i + 1U), actions.end());
                        continuations_[continuation_index].commands = compile_from(remaining, 0U, return_menu, end_continuation);
                    } else {
                        continuations_[continuation_index].commands = compile_from(actions, i + 1U, return_menu, end_continuation);
                    }
                    out << launch(continuation_id);
                    return out.str();
                }
                case NavigationActionKind::RepeatBegin:
                case NavigationActionKind::RepeatEnd:
                    throw std::runtime_error("repeat-group marker survived DVD navigation expansion");
            }
        }
        if (end_continuation != 0U) out << continuation_jump(end_continuation);
        else if (return_menu != 0U) out << "jump vmgm menu " << return_menu << ";";
        else out << "exit;";
        return out.str();
    }

    const std::vector<ResolvedMenu>& menus_;
    std::size_t title_count_ = 0;
    std::unordered_map<std::string,std::size_t> menu_numbers_;
    std::vector<DvdContinuationNode> continuations_;
};

std::string dvd_chapter_dispatch_pre_commands(std::size_t chapter_count, const std::string& menu_button_commands) {
    std::ostringstream out;
    for (std::size_t chapter = 2; chapter <= chapter_count; ++chapter) out << "if (g4 == " << chapter << ") { g4 = 0; jump title 1 chapter " << chapter << "; } ";
    out << "g4 = 0; " << menu_button_commands;
    return out.str();
}

std::string dvd_title_stream_pre_commands(std::size_t audio_count, std::size_t subtitle_count,
                                          int default_audio_stream = 0, int default_subtitle_stream = -1) {
    std::ostringstream out;
    // SetSTN is legal in the VTS title domain, not in VMGM. g2/g3 carry an
    // explicit menu selection when one exists. If they are still zero, apply
    // this title's authored defaults directly without converting those defaults
    // into a disc-global override that would leak into another title.
    if (default_audio_stream > 0)
        out << "if (g2 == 0) audio = " << (default_audio_stream - 1) << "; ";
    if (default_subtitle_stream == 0)
        out << "if (g3 == 0) subtitle = 62; ";
    else if (default_subtitle_stream > 0)
        out << "if (g3 == 0) subtitle = " << (64 + default_subtitle_stream - 1) << "; ";
    for (std::size_t i = 0; i < audio_count; ++i)
        out << "if (g2 == " << (i + 1U) << ") audio = " << i << "; ";
    for (std::size_t i = 0; i < subtitle_count; ++i)
        out << "if (g3 == " << (i + 1U) << ") subtitle = " << (64U + i) << "; ";
    // dvdauthor's value 62 disables all subpictures. Zero merely selects
    // stream 0 with only forced subpictures visible.
    out << "if (g3 == 33) subtitle = 62;";
    return out.str();
}

std::string dvd_dispatch_shard_commands(std::size_t visible_menu_count,
                                        const std::vector<DvdContinuationNode>& continuations,
                                        std::size_t begin, std::size_t end) {
    std::ostringstream out;
    for (std::size_t i = begin; i < end; ++i) {
        const auto& node = continuations[i];
        out << "if (g0 == " << node.id << ") jump vmgm menu " << (visible_menu_count + node.id) << "; ";
    }
    if (visible_menu_count != 0U) out << "jump vmgm menu 1;";
    else out << "exit;";
    return out.str();
}

std::string dvd_dispatch_root_commands(std::size_t visible_menu_count, std::size_t continuation_count,
                                       std::size_t first_shard_menu, std::size_t shard_size) {
    std::ostringstream out;
    if (visible_menu_count != 0U) out << "if (g1 == 0) jump vmgm menu 1; g1 = 0; ";
    else out << "if (g1 == 0) exit; g1 = 0; ";
    const std::size_t shard_count = continuation_count == 0U ? 0U : (continuation_count + shard_size - 1U) / shard_size;
    for (std::size_t shard = 0; shard < shard_count; ++shard) {
        const auto upper = std::min(continuation_count, (shard + 1U) * shard_size);
        out << "if (g0 &lt;= " << upper << ") jump vmgm menu " << (first_shard_menu + shard) << "; ";
    }
    if (visible_menu_count != 0U) out << "jump vmgm menu 1;";
    else out << "exit;";
    return out.str();
}

std::string meta_header(unsigned playlist_offset, const std::vector<double>& chapters = {},
                        DiscTarget target = DiscTarget::BluRay1080, unsigned max_transport_bitrate_kbps = 0U,
                        unsigned min_transport_bitrate_kbps = 0U,
                        std::optional<unsigned> m2ts_offset = std::nullopt, bool final_still = false) {
    std::ostringstream s;
    // Keep the muxer's Blu-ray timeline explicit and in lock-step with the
    // absolute timeout clocks generated in hdmv.cpp. Numeric --start-time is
    // expressed in 45 kHz units by tsMuxer.
    s << "MUXOPT " << (target_is_uhd(target) ? "--blu-ray-v3" : "--blu-ray")
      << " --new-audio-pes --start-time=" << (hdmv::kBluRayMuxStartPts90k / 2U);
    // Always use Blu-ray's extended audio PES form for AC-3/TrueHD/DTS.  The
    // bundled tsMuxer documentation says BD mode enables this automatically,
    // but its parser does not; leaving TrueHD in generic 0xbd private-stream
    // PES packets causes poor interoperability on hardware players.
    //
    // Also give every authored clip a finite transport pacing ceiling and the
    // 950 ms mux/smoothing-buffer lead.  Standard BD uses its 48 Mb/s ceiling.
    // UHD previously left title/movie transport unconstrained (menus happened
    // to get a ceiling only when the interactive floor was enabled), allowing
    // large lossless-audio/video access units to be emitted in bursts.  Such
    // bursts can make hardware players lose/reacquire the audio clock, which is
    // especially visible as LPCM/TrueHD pops, speed changes or apparent seeks.
    // The UHD application validator already caps configured combined A/V at
    // 86 Mb/s for the current 25/50 GB recordable-media profile, so use the same ceiling for the V3 transport pacing model.
    if (max_transport_bitrate_kbps == 0U)
        max_transport_bitrate_kbps = static_cast<unsigned>(target_max_combined_av_bitrate_kbps(target));
    s << " --maxbitrate=" << max_transport_bitrate_kbps << " --vbv-len=950";
    // Extremely compressible menu video (for example, a solid-color H.264
    // background) can otherwise produce an entire 30-second M2TS of only a few
    // hundred KiB.  VLC can read that whole clip ahead through libbluray; arrow
    // selection still works in the IG processor, but an activated button's HDMV
    // navigation command is not serviced until the next read call, making the
    // activation appear to wait for the remaining menu duration.  A menu-only
    // transport floor keeps reads flowing without changing the encoded picture.
    // tsMuxer implements --minbitrate by inserting NULL transport packets.
    if (min_transport_bitrate_kbps != 0U) s << " --minbitrate=" << min_transport_bitrate_kbps;
    const unsigned clip_offset = m2ts_offset.value_or(playlist_offset);
    s << " --mplsOffset=" << playlist_offset << " --m2tsOffset=" << clip_offset;
    if (final_still) s << " --final-still";
    if (!chapters.empty()) {
        // tsMuxer does not inject a title-start mark when --custom-chapters is present.
        s << " --custom-chapters=" << chapter_time(0.0);
        for (double chapter : chapters) s << ";" << chapter_time(chapter);
    }
    s << "\n";
    return s.str();
}


void resolve_menu_node(const Menu& source, const Menu* parent, const std::string& parent_id,
                       std::vector<ResolvedMenu>& out) {
    Menu effective = source;
    if (parent) {
        if (source.inherit_background_image) { effective.background_image = parent->background_image; effective.background_video = parent->background_video; }
        if (source.inherit_background_color) effective.background_color = parent->background_color;
        if (source.inherit_button_style) effective.default_button_style = parent->default_button_style;
        if (source.inherit_encoding) { effective.encoding = parent->encoding; effective.uhd_encoding = parent->uhd_encoding; effective.dvd_encoding = parent->dvd_encoding; }
        if (source.inherit_audio) effective.audio_source = parent->audio_source;
        if (source.inherit_duration) effective.duration_seconds = parent->duration_seconds;
    }
    effective.width = kProjectDesignWidth;
    effective.height = kProjectDesignHeight;
    effective.submenus.clear();
    if (parent && source.auto_back_button) {
        MenuButton back = source.back_button;
        if (back.label.empty()) back.label = "Back";
        if (back.bounds.width <= 0 || back.bounds.height <= 0)
            back.bounds = {3280,1920,440,140};
        back.target_kind = MenuButtonTargetKind::Menu;
        back.target_menu_id = parent_id;
        NavigationAction back_action; back_action.kind = NavigationActionKind::Menu; back_action.target_menu_id = parent_id;
        set_single_button_action(back, std::move(back_action));
        effective.buttons.push_back(std::move(back));
    }
    out.push_back({effective, parent_id});
    for (const auto& child : source.submenus)
        resolve_menu_node(child, &effective, source.id, out);
}

std::vector<ResolvedMenu> resolve_menu_tree(const Menu& root) {
    std::vector<ResolvedMenu> menus;
    if (root.id.empty()) return menus;
    resolve_menu_node(root, nullptr, {}, menus);
    return menus;
}

std::string rgb_hex(const Rgba& c) {
    std::ostringstream o;
    o << "0x" << std::hex << std::setfill('0') << std::setw(2) << static_cast<unsigned>(c.r)
      << std::setw(2) << static_cast<unsigned>(c.g) << std::setw(2) << static_cast<unsigned>(c.b);
    return o.str();
}

std::uint32_t be32(const std::vector<unsigned char>& b, std::size_t p) {
    return (std::uint32_t(b[p]) << 24) | (std::uint32_t(b[p + 1]) << 16) | (std::uint32_t(b[p + 2]) << 8) | b[p + 3];
}
std::uint64_t bluray_uop_mask(std::uint64_t semantic){std::uint64_t out=0;auto map=[&](UserOperation op,unsigned index){if(user_operation_prohibited(semantic,op))out|=std::uint64_t{1}<<index;};map(UserOperation::MenuCall,0);map(UserOperation::TitleSearch,1);map(UserOperation::ChapterSearch,2);map(UserOperation::TimeSearch,3);map(UserOperation::SkipNext,4);map(UserOperation::SkipPrevious,5);map(UserOperation::Stop,7);map(UserOperation::Pause,8);map(UserOperation::StillOff,10);map(UserOperation::ForwardPlay,11);map(UserOperation::BackwardPlay,12);map(UserOperation::Resume,13);map(UserOperation::MoveUp,14);map(UserOperation::MoveDown,15);map(UserOperation::MoveLeft,16);map(UserOperation::MoveRight,17);map(UserOperation::SelectButton,18);map(UserOperation::ActivateButton,19);map(UserOperation::SelectAndActivate,20);map(UserOperation::PrimaryAudioChange,21);map(UserOperation::AngleChange,23);map(UserOperation::PopupOn,24);map(UserOperation::PopupOff,25);map(UserOperation::SubtitleEnableDisable,26);map(UserOperation::SubtitleChange,27);map(UserOperation::SecondaryVideoEnableDisable,28);map(UserOperation::SecondaryVideoChange,29);map(UserOperation::SecondaryAudioEnableDisable,30);map(UserOperation::SecondaryAudioChange,31);map(UserOperation::PipSubtitleChange,33);return out;}
std::uint32_t dvd_uop_mask(std::uint64_t semantic){std::uint32_t out=0;auto set=[&](unsigned bit){out|=std::uint32_t{1}<<bit;};auto map=[&](UserOperation op,unsigned bit){if(user_operation_prohibited(semantic,op))set(bit);};map(UserOperation::TitleOrTimePlay,0);map(UserOperation::ChapterSearchOrPlay,1);map(UserOperation::TitleSearch,2);map(UserOperation::Stop,3);map(UserOperation::GoUp,4);if(user_operation_prohibited(semantic,UserOperation::TimeSearch)){set(0);set(5);}if(user_operation_prohibited(semantic,UserOperation::ChapterSearch)){set(1);set(5);}map(UserOperation::SkipPrevious,6);map(UserOperation::SkipNext,7);map(UserOperation::ForwardPlay,8);map(UserOperation::BackwardPlay,9);map(UserOperation::TitleMenuCall,10);if(user_operation_prohibited(semantic,UserOperation::MenuCall))set(11);map(UserOperation::SubtitleMenuCall,12);map(UserOperation::AudioMenuCall,13);map(UserOperation::AngleMenuCall,14);map(UserOperation::ChapterMenuCall,15);map(UserOperation::Resume,16);if(user_operation_prohibited(semantic,UserOperation::SelectButton)||user_operation_prohibited(semantic,UserOperation::ActivateButton)||user_operation_prohibited(semantic,UserOperation::SelectAndActivate)||user_operation_prohibited(semantic,UserOperation::MoveUp)||user_operation_prohibited(semantic,UserOperation::MoveDown)||user_operation_prohibited(semantic,UserOperation::MoveLeft)||user_operation_prohibited(semantic,UserOperation::MoveRight))set(17);map(UserOperation::StillOff,18);map(UserOperation::Pause,19);map(UserOperation::PrimaryAudioChange,20);if(user_operation_prohibited(semantic,UserOperation::SubtitleEnableDisable)||user_operation_prohibited(semantic,UserOperation::SubtitleChange))set(21);map(UserOperation::AngleChange,22);map(UserOperation::KaraokeAudioMixChange,23);map(UserOperation::VideoPresentationModeChange,24);return out;}
void patch_dvd_uop_file(const fs::path& path,std::uint32_t add_mask){if(add_mask==0)return;std::ifstream in(path,std::ios::binary);if(!in)throw std::runtime_error("cannot patch DVD UOP mask: "+path.string());std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)),{});if(bytes.size()<0xd0U||std::string(reinterpret_cast<const char*>(bytes.data()),12)!="DVDVIDEO-VTS")throw std::runtime_error("invalid DVD VTS IFO while patching UOP mask: "+path.string());const auto pgcit_sector=be32(bytes,0xccU);const std::size_t table=static_cast<std::size_t>(pgcit_sector)*2048U;if(table+16U>bytes.size())throw std::runtime_error("DVD VTS PGC table is out of range while patching UOP mask: "+path.string());const auto pgc_count=static_cast<unsigned>((bytes[table]<<8)|bytes[table+1U]);if(pgc_count==0U)throw std::runtime_error("DVD VTS has no title PGC while patching UOP mask: "+path.string());const std::size_t pgc=table+static_cast<std::size_t>(be32(bytes,table+12U));if(pgc+12U>bytes.size())throw std::runtime_error("DVD title PGC is out of range while patching UOP mask: "+path.string());const std::uint32_t existing=be32(bytes,pgc+8U),combined=existing|add_mask;bytes[pgc+8U]=static_cast<unsigned char>(combined>>24);bytes[pgc+9U]=static_cast<unsigned char>(combined>>16);bytes[pgc+10U]=static_cast<unsigned char>(combined>>8);bytes[pgc+11U]=static_cast<unsigned char>(combined);write_binary(path,bytes);}
void patch_dvd_title_uops(const fs::path& dvd_root,std::size_t title_index,std::uint32_t mask){if(mask==0)return;std::ostringstream n;n<<"VTS_"<<std::setw(2)<<std::setfill('0')<<(title_index+1U)<<"_0";patch_dvd_uop_file(dvd_root/"VIDEO_TS"/(n.str()+".IFO"),mask);patch_dvd_uop_file(dvd_root/"VIDEO_TS"/(n.str()+".BUP"),mask);}
} // namespace

void AuthorEngine::validate_encoding_profile_for_target(const EncodingProfile& encoding, DiscTarget target, const std::string& where,
                                                       bool allow_exceeding_format_limits) {
    validate_encoding_for_target(encoding, target, where, allow_exceeding_format_limits);
}

AuthorEngine::AuthorEngine(ToolPaths t, AuthoringLimits limits) : tools_(std::move(t)), limits_(limits) {
#if defined(_WIN32)
    // spumux is a child process and must inherit the private, relocatable
    // Fontconfig configuration before it performs text-subtitle rendering.
    detail::configure_fontconfig_runtime_environment();
    tools_.ffmpeg=resolve_sibling_default_tool(std::move(tools_.ffmpeg),"ffmpeg");
    tools_.ffprobe=resolve_sibling_default_tool(std::move(tools_.ffprobe),"ffprobe");
    tools_.x264=resolve_sibling_default_tool(std::move(tools_.x264),"x264");
    tools_.x265=resolve_sibling_default_tool(std::move(tools_.x265),"x265");
    tools_.dvdauthor=resolve_sibling_default_tool(std::move(tools_.dvdauthor),"dvdauthor");
    tools_.spumux=resolve_sibling_default_tool(std::move(tools_.spumux),"spumux");
    tools_.mplex=resolve_sibling_default_tool(std::move(tools_.mplex),"mplex");
#endif
    // mkisofs is a BDMV Author private sibling on every packaged platform.
    // Explicit paths remain authoritative because resolve_sibling_default_tool()
    // only substitutes the bare default command name.
    tools_.mkisofs=resolve_sibling_default_tool(std::move(tools_.mkisofs),"mkisofs");
}


void AuthorEngine::validate_project(const Project& p, bool allow_exceeding_format_limits) {
    if (p.titles.empty()) throw std::runtime_error("add at least one title");
    if (p.titles.size() > 999) throw std::runtime_error("too many titles");
    if (!frame_rate_override_valid(p.target, p.frame_rate))
        throw std::runtime_error("frame-rate override '" + p.frame_rate + "' is not legal for the selected output target");
    if (project_has_menu(p) &&
        !static_video_mode_selection_valid(p.target, menu_resolution_for_target(p,p.target), menu_aspect_ratio_for_target(p,p.target), p.frame_rate, true, encoding_for_target(p.menu,p.target).video_codec))
        throw std::runtime_error("menu resolution/aspect/frame-rate selection is not legal for the selected output target");
    std::optional<bool> explicit_dvd_pal;
    auto merge_explicit_dvd_family = [&](std::optional<bool> family) {
        if (!family) return;
        if (explicit_dvd_pal && *explicit_dvd_pal != *family)
            throw std::runtime_error("DVD-Video cannot mix PAL titles/resolutions with Film/NTSC titles/resolutions on the same disc");
        explicit_dvd_pal = family;
    };
    if (target_is_dvd(p.target)) {
        merge_explicit_dvd_family(explicit_dvd_pal_family(p.frame_rate));
        if (project_has_menu(p)) merge_explicit_dvd_family(dvd_family_from_resolution(menu_resolution_for_target(p,p.target)));
    }
    auto validate_font_family = [](const std::string& family, bool bold, bool italic, const std::string& where) {
        // Empty is the automatic-font sentinel. Resolve it only when a font is
        // actually needed so projects without text rendering remain usable in
        // deliberately minimal builds that omit Fontconfig.
        if (family.empty()) return;
        try {
            const auto concrete = detail::resolve_font_family(family, bold, italic);
            if (concrete.empty() || concrete.find_first_of("\r\n\",") != std::string::npos)
                throw std::runtime_error("resolved font name contains a character not supported by authoring metadata");
        } catch (const std::exception& e) {
            throw std::runtime_error(where + ": " + e.what());
        }
    };
    for (const auto& t : p.titles) {
        const auto& per_title_rate = frame_rate_for_target(t, p.target);
        if (!title_frame_rate_override_valid(p.target, per_title_rate))
            throw std::runtime_error("title frame-rate override '" + per_title_rate + "' is not legal for the selected output target");
        const auto effective_rate = normalize_frame_rate_for_target(p.target, per_title_rate) == "inherit"
            ? p.frame_rate : per_title_rate;
        if (!static_video_mode_selection_valid(p.target, resolution_for_target(t,p.target),
                                                aspect_ratio_for_target(t,p.target), effective_rate, false, encoding_for_target(t,p.target).video_codec))
            throw std::runtime_error("title resolution/aspect/frame-rate selection is not legal for the selected output target");
        if (target_is_dvd(p.target)) {
            merge_explicit_dvd_family(explicit_dvd_pal_family(per_title_rate));
            merge_explicit_dvd_family(dvd_family_from_resolution(resolution_for_target(t,p.target)));
        }
    }
    for (const auto& t : p.titles) {
        if (!fs::is_regular_file(t.source)) throw std::runtime_error("missing title source: " + t.source.string());
        if (t.chapter_mode == ChapterMode::Manual) {
            double prev = -1.0;
            for (double ch : t.chapters_seconds) {
                if (!std::isfinite(ch) || ch < 0 || ch <= prev)
                    throw std::runtime_error("manual chapter times must be finite, non-negative and strictly increasing");
                prev = ch;
            }
        }
        if (t.chapter_mode == ChapterMode::Interval &&
            (!std::isfinite(t.chapter_interval_seconds) || t.chapter_interval_seconds <= 0.0))
            throw std::runtime_error("chapter interval must be greater than zero seconds");
        if (t.audio_language.size() != 3) throw std::runtime_error("audio language must contain exactly three letters");
        auto valid_language_override = [](const std::string& language) {
            return language.empty() || (language.size() == 3U && std::all_of(language.begin(), language.end(), [](unsigned char c) { return std::isalpha(c) != 0; }));
        };
        auto validate_subtitle_style = [&](const SubtitleStyle& st, const std::string& where) {
            validate_font_family(st.font_family, st.bold, st.italic, where + " font");
            if (st.font_size_px < 4 || st.font_size_px > 256) throw std::runtime_error(where + " font size must be 4-256 subtitle units");
            if (st.bottom_offset_px < 0 || st.bottom_offset_px > 2160) throw std::runtime_error(where + " bottom offset must be between 0 and 2160 subtitle units");
            if (!st.override_style) return;
            if (!std::isfinite(st.line_spacing) || st.line_spacing <= 0.0 || st.line_spacing > 10.0) throw std::runtime_error(where + " line spacing must be greater than zero and at most 10");
            if (!std::isfinite(st.border_width) || st.border_width < 0.0 || st.border_width > 32.0) throw std::runtime_error(where + " border width must be between 0 and 32 pixels");
            if (!std::isfinite(st.fade_in_ms) || !std::isfinite(st.fade_out_ms) || st.fade_in_ms < 0.0 || st.fade_out_ms < 0.0 || st.fade_in_ms > 10000.0 || st.fade_out_ms > 10000.0)
                throw std::runtime_error(where + " fade durations must be between 0 and 10000 ms");
            const std::array<std::string,4> ha={"default","left","center","right"};
            const std::array<std::string,3> va={"top","center","bottom"};
            if (std::find(ha.begin(),ha.end(),st.dvd_horizontal_alignment)==ha.end()) throw std::runtime_error(where + " has invalid DVD horizontal alignment");
            if (std::find(va.begin(),va.end(),st.dvd_vertical_alignment)==va.end()) throw std::runtime_error(where + " has invalid DVD vertical alignment");
            for (int m : {st.dvd_left_margin_px, st.dvd_right_margin_px, st.dvd_top_margin_px, st.dvd_bottom_margin_px})
                if (m < 0 || m > 720) throw std::runtime_error(where + " DVD margins must be between 0 and 720 pixels");
            if (st.dvd_shadow_offset_x < -100 || st.dvd_shadow_offset_x > 100 || st.dvd_shadow_offset_y < -100 || st.dvd_shadow_offset_y > 100)
                throw std::runtime_error(where + " DVD shadow offsets must be between -100 and 100 pixels");
        };
        validate_subtitle_style(t.subtitle_default_style, "title subtitle defaults");
        std::vector<int> audio_ordinals, subtitle_ordinals;
        for (const auto& stream : t.audio_stream_settings) {
            if (stream.source_ordinal < 0) throw std::runtime_error("audio stream override has a negative source ordinal");
            if (!valid_language_override(stream.language)) throw std::runtime_error("audio stream language override must be empty or exactly three letters");
            if (std::find(audio_ordinals.begin(), audio_ordinals.end(), stream.source_ordinal) != audio_ordinals.end())
                throw std::runtime_error("duplicate audio stream override for source stream " + std::to_string(stream.source_ordinal + 1));
            audio_ordinals.push_back(stream.source_ordinal);
            if (stream.output_channels != 0 && stream.output_channels != 1 && stream.output_channels != 2 && stream.output_channels != 4 && stream.output_channels != 6 && stream.output_channels != 8)
                throw std::runtime_error("audio stream output channel count must be source, mono, stereo, quad, 5.1, or 7.1");
            if (stream.override_encoding) {
                validate_encoding_for_target(stream.encoding, DiscTarget::BluRay1080, "audio stream standard Blu-ray override", allow_exceeding_format_limits);
                validate_encoding_for_target(stream.uhd_encoding, DiscTarget::UltraHdBluRay2160, "audio stream UHD override", allow_exceeding_format_limits);
                validate_encoding_for_target(stream.dvd_encoding, DiscTarget::DvdVideo480p, "audio stream DVD override", allow_exceeding_format_limits);
            }
        }
        for (const auto& stream : t.subtitle_stream_settings) {
            if (stream.source_ordinal < 0) throw std::runtime_error("subtitle stream override has a negative source ordinal");
            if (!valid_language_override(stream.language)) throw std::runtime_error("subtitle stream language override must be empty or exactly three letters");
            validate_subtitle_style(stream.style, "subtitle stream " + std::to_string(stream.source_ordinal + 1));
            if (std::find(subtitle_ordinals.begin(), subtitle_ordinals.end(), stream.source_ordinal) != subtitle_ordinals.end())
                throw std::runtime_error("duplicate subtitle stream override for source stream " + std::to_string(stream.source_ordinal + 1));
            subtitle_ordinals.push_back(stream.source_ordinal);
        }
        for (const auto& sub : t.external_subtitles) {
            if (!fs::is_regular_file(sub.source)) throw std::runtime_error("missing attached subtitle file: " + sub.source.string());
            if (!valid_language_override(sub.language)) throw std::runtime_error("attached subtitle language must be empty or exactly three letters");
            validate_subtitle_style(sub.style, "attached subtitle '" + sub.source.string() + "'");
        }
        const std::string title_where = t.name.empty() ? "title" : ("title '" + t.name + "'");
        validate_encoding_for_target(t.encoding, DiscTarget::BluRay1080, title_where + " standard Blu-ray fallback", allow_exceeding_format_limits);
        validate_encoding_for_target(t.uhd_encoding, DiscTarget::UltraHdBluRay2160, title_where + " UHD fallback", allow_exceeding_format_limits);
        validate_encoding_for_target(t.dvd_encoding, DiscTarget::DvdVideo480p, title_where + " DVD fallback", allow_exceeding_format_limits);
    }

    auto validate_button_style = [&](const ButtonStyle& s, const std::string& where) {
        validate_font_family(s.font_family, s.bold, s.italic, where + " button font");
        if (s.font_size_px < 6 || s.font_size_px > 512)
            throw std::runtime_error(where + " button font size must be 6..512 pixels");
        if (s.font_family.size() > 255)
            throw std::runtime_error(where + " button font family is too long");
        if (s.corner_radius < 0 || s.corner_radius > 512)
            throw std::runtime_error(where + " button corner radius is out of range");
        for (const auto* v : {&s.normal, &s.active, &s.selected, &s.activated})
            if (v->border_width < 0 || v->border_width > 128)
                throw std::runtime_error(where + " button border width is out of range");
    };

    const auto menus = resolve_menu_tree(p.menu);
    if (menus.size() + p.titles.size() > 999)
        throw std::runtime_error("combined menu/title count exceeds the current 999-playlist limit");

    std::unordered_set<std::string> menu_ids;
    for (std::size_t mi = 0; mi < menus.size(); ++mi) {
        const auto& menu = menus[mi].menu;
        const std::string where = menu.name.empty() ? ("menu " + std::to_string(mi + 1)) : ("menu '" + menu.name + "'");
        if (menu.id.empty()) throw std::runtime_error(where + " has an empty id");
        if (!menu_ids.insert(menu.id).second) throw std::runtime_error("duplicate menu id: " + menu.id);
        if (menu.width != kProjectDesignWidth || menu.height != kProjectDesignHeight) throw std::runtime_error(where + " is not in the 3840x2160 project design space");
        if (menu.background_video.empty() && menu.audio_source.empty() && menu.duration_seconds < 10.0)
            throw std::runtime_error(where + " duration must be at least 10 seconds when using a still background without menu audio");
        if (!menu.background_image.empty() && !menu.background_video.empty())
            throw std::runtime_error(where + " cannot use a still image and video background at the same time");
        if (!menu.background_image.empty() && !fs::is_regular_file(menu.background_image))
            throw std::runtime_error("missing background image for " + where + ": " + menu.background_image.string());
        if (!menu.background_video.empty() && !fs::is_regular_file(menu.background_video))
            throw std::runtime_error("missing background video for " + where + ": " + menu.background_video.string());
        if (!menu.audio_source.empty() && !fs::is_regular_file(menu.audio_source))
            throw std::runtime_error("missing audio for " + where + ": " + menu.audio_source.string());
        validate_encoding_for_target(menu.encoding, DiscTarget::BluRay1080, where + " standard Blu-ray fallback", allow_exceeding_format_limits);
        validate_encoding_for_target(menu.uhd_encoding, DiscTarget::UltraHdBluRay2160, where + " UHD fallback", allow_exceeding_format_limits);
        validate_encoding_for_target(menu.dvd_encoding, DiscTarget::DvdVideo480p, where + " DVD fallback", allow_exceeding_format_limits);
        validate_button_style(menu.default_button_style, where + " default");
        if (menu.back_button.use_custom_style) validate_button_style(menu.back_button.style, where + " Back button custom");
        if (menu.buttons.empty()) throw std::runtime_error(where + " needs at least one button");
        if (menu.buttons.size() > 255) throw std::runtime_error(where + " supports at most 255 buttons");
        for (const auto& b : menu.buttons) {
            if (b.use_custom_style) validate_button_style(b.style, where + " custom");
            const auto actions = button_action_sequence(b);
            if (actions.empty()) throw std::runtime_error(where + " has a button with no actions");
            ExpandedNavigationSequence expanded_actions;
            try { expanded_actions = expand_navigation_repeat_groups(actions); }
            catch (const std::exception& e) { throw std::runtime_error(where + " has an invalid repeat group: " + e.what()); }
            for (std::size_t ai = 0; ai < expanded_actions.actions.size(); ++ai) {
                const auto& action = expanded_actions.actions[ai];
                if (action.repeat_count > 1000U)
                    throw std::runtime_error(where + " has an action repeat count above 1000");
                if (action.repeat_count == 0U && (action.kind != NavigationActionKind::PlayTitle || ai + 1U != expanded_actions.actions.size()))
                    throw std::runtime_error(where + " may repeat forever only on its final Play Title action");
                if (action.kind == NavigationActionKind::Menu && action.repeat_count != 1U)
                    throw std::runtime_error(where + " cannot repeat a menu jump");
                if (action.kind == NavigationActionKind::Menu && ai + 1U != expanded_actions.actions.size())
                    throw std::runtime_error(where + " has a menu jump before the end of a button action sequence");
                if (navigation_action_is_title_scoped(action.kind)) {
                    if (action.target_title < 1 || action.target_title > p.titles.size())
                        throw std::runtime_error(where + " has a button action with an invalid title target");
                    if ((action.kind == NavigationActionKind::AudioTrack ||
                         action.kind == NavigationActionKind::SubtitleTrack) &&
                        (action.target_stream < 1 || action.target_stream > 4095))
                        throw std::runtime_error(where + " has a stream-selection action with an invalid stream number");
                }
            }
            if (b.bounds.width <= 0 || b.bounds.height <= 0 || b.bounds.x < 0 || b.bounds.y < 0 ||
                b.bounds.x + b.bounds.width > menu.width || b.bounds.y + b.bounds.height > menu.height)
                throw std::runtime_error(where + " has a button outside the 3840x2160 design canvas");
            if (b.kind == MenuButtonKind::Image) {
                if (b.normal_image.empty() || !fs::is_regular_file(b.normal_image))
                    throw std::runtime_error(where + " image-only button is missing its unselected image");
                if (!b.selected_image.empty() && !fs::is_regular_file(b.selected_image))
                    throw std::runtime_error(where + " selected image does not exist: " + b.selected_image.string());
            }
        }
        for (const auto& o : menu.overlays) {
            if (o.bounds.width <= 0 || o.bounds.height <= 0 || o.bounds.x < 0 || o.bounds.y < 0 ||
                o.bounds.x + o.bounds.width > menu.width || o.bounds.y + o.bounds.height > menu.height)
                throw std::runtime_error(where + " has a label outside the 3840x2160 design canvas");
            if (o.kind == MenuOverlayKind::Text) {
                if (o.text.empty()) throw std::runtime_error(where + " has an empty text label");
                validate_font_family(o.font_family, o.bold, o.italic, where + " text-label font");
                if (o.font_size_px < 6 || o.font_size_px > 1024)
                    throw std::runtime_error(where + " text-label font size must be 6..1024 pixels");
            } else if (o.image.empty() || !fs::is_regular_file(o.image)) {
                throw std::runtime_error(where + " image label is missing its image file");
            }
        }
    }
    auto validate_menu_actions = [&](std::span<const NavigationAction> actions, const std::string& where) {
        const std::vector<NavigationAction> source(actions.begin(), actions.end());
        ExpandedNavigationSequence expanded;
        try { expanded = expand_navigation_repeat_groups(source); }
        catch (const std::exception& e) { throw std::runtime_error(where + " has an invalid repeat group: " + e.what()); }
        for (std::size_t ai = 0; ai < expanded.actions.size(); ++ai) {
            const auto& action = expanded.actions[ai];
            if (action.repeat_count > 1000U) throw std::runtime_error(where + " has an action repeat count above 1000");
            if (action.repeat_count == 0U && (action.kind != NavigationActionKind::PlayTitle || ai + 1U != expanded.actions.size()))
                throw std::runtime_error(where + " may repeat forever only on its final Play Title action");
            if (action.kind == NavigationActionKind::Menu) {
                if (action.repeat_count != 1U) throw std::runtime_error(where + " cannot repeat a menu jump");
                if (ai + 1U != expanded.actions.size()) throw std::runtime_error(where + " has a menu jump before the end of its action sequence");
                if (!menu_ids.contains(action.target_menu_id)) throw std::runtime_error(where + " targets unknown menu id: " + action.target_menu_id);
            } else if (navigation_action_is_title_scoped(action.kind)) {
                if (action.target_title < 1 || action.target_title > p.titles.size()) throw std::runtime_error(where + " has an invalid title target");
                if ((action.kind == NavigationActionKind::AudioTrack || action.kind == NavigationActionKind::SubtitleTrack) &&
                    (action.target_stream < 1 || action.target_stream > 4095)) throw std::runtime_error(where + " has an invalid stream number");
            }
        }
    };
    for (const auto& rm : menus) for (const auto& b : rm.menu.buttons) validate_menu_actions(button_action_sequence(b), "menu button");
    validate_menu_actions(p.first_play_actions, "First Playback sequence");
    for (std::size_t i=0;i<p.titles.size();++i) if(!p.titles[i].menu_button_actions.empty()) validate_menu_actions(p.titles[i].menu_button_actions,"title "+std::to_string(i+1U)+" Menu-button sequence");
    if (p.output_image.empty()) throw std::runtime_error("output image path is empty");
}

bool AuthorEngine::validate_java_free_bdmv(const fs::path& root, std::string* err) {
    auto fail = [&](std::string s) {
        if (err) *err = std::move(s);
        return false;
    };
    for (auto d : {"BDMV/JAR", "BDMV/BDJO"})
        if (fs::exists(root / d)) return fail(std::string("Java content directory exists: ") + d);
    auto p = root / "BDMV/index.bdmv";
    std::ifstream f(p, std::ios::binary);
    if (!f) return fail("index.bdmv missing");
    std::vector<unsigned char> b(std::istreambuf_iterator<char>(f), {});
    if (b.size() < 90 || std::string(reinterpret_cast<char*>(b.data()), 8).rfind("INDX", 0) != 0)
        return fail("invalid index.bdmv");
    auto off = be32(b, 8);
    if (off + 30 > b.size()) return fail("truncated index.bdmv");
    std::size_t q = off + 4;
    auto check = [&](std::size_t e) {
        if (e + 12 > b.size()) return false;
        return ((b[e] >> 6) & 3) != 2;
    };
    if (!check(q) || !check(q + 12)) return fail("BD-J First Playback/Top Menu entry found");
    q += 24;
    if (q + 2 > b.size()) return fail("truncated title table");
    auto n = (unsigned(b[q]) << 8) | b[q + 1];
    q += 2;
    for (unsigned i = 0; i < n; ++i, q += 12)
        if (!check(q)) return fail("BD-J title entry found");
    return true;
}

void AuthorEngine::author(const Project& p, ProgressCallback cb, CancellationCallback cancelled) {
    CancellationRegistration cancellation_registration(cancelled);
    authoring_cancellation_point();
    validate_project(p, limits_.allow_exceeding_format_limits);
    const int combined_transport_limit_kbps = limits_.combined_transport_bitrate_kbps(p.target);
    if (!target_is_dvd(p.target) && combined_transport_limit_kbps < 1000)
        throw std::runtime_error("combined transport bitrate limit must be at least 1000 kb/s");

    struct ProgressReporter {
        ProgressCallback callback;
        double last_percent = 0.0;
        void emit(double percent, const std::string& message) {
            percent = std::clamp(percent, last_percent, 100.0);
            last_percent = percent;
            if (callback) callback(percent, message);
        }
    } reporter{std::move(cb)};

    fs::path work = p.work_directory;
    if (work.empty()) {
        auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        work = fs::temp_directory_path() / ("bdmvauthor-" + std::to_string(stamp));
    }
    fs::remove_all(work);
    fs::create_directories(work);

    const auto output_stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path output_parent = p.output_image.parent_path().empty() ? fs::path(".") : p.output_image.parent_path();
    fs::create_directories(output_parent);
    const fs::path temporary_output_image = output_parent /
        ("." + p.output_image.filename().string() + ".rendering-" + std::to_string(output_stamp));
    { std::error_code ec; fs::remove(temporary_output_image, ec); }
    RenderTempCleanup render_cleanup{work, temporary_output_image};

    const fs::path shared_cache_root = (p.use_encode_cache || p.use_compliance_cache) ? default_encode_cache_root() : fs::path{};
    const fs::path encode_cache_root = p.use_encode_cache ? shared_cache_root : fs::path{};
    const fs::path compliance_cache_root = p.use_compliance_cache ? shared_cache_root : fs::path{};
    if ((p.use_encode_cache || p.use_compliance_cache) && shared_cache_root.empty())
        reporter.emit(0.5, "Cache unavailable because the home directory could not be determined; continuing without persistent cache");
    std::unordered_map<std::string, MediaFingerprint> source_fingerprints;
    auto source_fingerprint = [&](const fs::path& source) -> const MediaFingerprint& {
        const auto key = fs::absolute(source).lexically_normal().string();
        auto it = source_fingerprints.find(key);
        if (it == source_fingerprints.end())
            it = source_fingerprints.emplace(key, fingerprint_media_file(source)).first;
        return it->second;
    };

    reporter.emit(1, "Analyzing video durations");
    const auto probe_dir = work / "probe";
    fs::create_directories(probe_dir);
    std::vector<double> title_durations;
    std::vector<SourceTrackCatalog> title_tracks;
    std::vector<std::vector<double>> resolved_title_chapters;
    title_durations.reserve(p.titles.size());
    title_tracks.reserve(p.titles.size());
    resolved_title_chapters.reserve(p.titles.size());
    for (std::size_t i = 0; i < p.titles.size(); ++i) {
        const double duration = probe_duration_seconds(
            tools_, p.titles[i].source, probe_dir / ("duration-" + std::to_string(i + 1) + ".txt"));
        title_durations.push_back(duration);
        title_tracks.push_back(probe_source_track_catalog(
            tools_, p.titles[i].source, probe_dir / ("tracks-" + std::to_string(i + 1) + ".txt")));
        for (const auto& attached : p.titles[i].external_subtitles) {
            auto ext = attached.source.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            const bool pgs = ext == ".sup" || ext == ".pgs";
            const auto style = attached.style.override_style ? attached.style : p.titles[i].subtitle_default_style;
            title_tracks.back().subtitles.push_back({-1, pgs ? "hdmv_pgs_subtitle" : "external_text",
                                                     normalized_language(attached.language), attached.source, style});
        }
        for (auto& subtitle_track : title_tracks.back().subtitles) {
            if (subtitle_track.source_ordinal < 0) continue;
            subtitle_track.subtitle_style = p.titles[i].subtitle_default_style;
            if (const auto* settings = find_subtitle_stream_settings(p.titles[i], subtitle_track.source_ordinal))
                if (settings->style.override_style) subtitle_track.subtitle_style = settings->style;
        }
        resolved_title_chapters.push_back(resolve_title_chapters(
            p.titles[i], duration, probe_source_chapter_starts(
                tools_, p.titles[i].source, probe_dir / ("chapters-" + std::to_string(i + 1) + ".txt"))));
    }

    for (std::size_t i = 0; i < p.titles.size(); ++i) {
        if(p.titles[i].default_audio_stream<0||p.titles[i].default_audio_stream>static_cast<int>(title_tracks[i].audio.size()))
            throw std::runtime_error("title "+std::to_string(i+1U)+" default audio selection is unavailable");
        if(p.titles[i].default_subtitle_stream<-1||p.titles[i].default_subtitle_stream>static_cast<int>(title_tracks[i].subtitles.size()))
            throw std::runtime_error("title "+std::to_string(i+1U)+" default subtitle selection is unavailable");
        for (const auto& stream : p.titles[i].audio_stream_settings)
            if (stream.source_ordinal >= static_cast<int>(title_tracks[i].audio.size()))
                throw std::runtime_error("title " + std::to_string(i + 1U) + " audio stream override refers to missing source stream " +
                                         std::to_string(stream.source_ordinal + 1));
        for (const auto& stream : p.titles[i].subtitle_stream_settings) {
            const bool found = std::any_of(title_tracks[i].subtitles.begin(), title_tracks[i].subtitles.end(),
                [&](const SourceTrackDescriptor& track) { return track.source_ordinal == stream.source_ordinal; });
            if (!found) throw std::runtime_error("title " + std::to_string(i + 1U) +
                " subtitle language override refers to a missing or unsupported source subtitle stream " +
                std::to_string(stream.source_ordinal + 1));
        }
    }

    auto validate_chapter_action = [&](const NavigationAction& action, const std::string& where) {
        if (action.kind != NavigationActionKind::PlayTitle || action.target_chapter == 0U) return;
        const auto ti = static_cast<std::size_t>(action.target_title - 1U);
        if (ti >= resolved_title_chapters.size()) return;
        const std::size_t chapter_count = resolved_title_chapters[ti].size() + 1U;
        if (action.target_chapter > chapter_count)
            throw std::runtime_error(where + " targets chapter " + std::to_string(action.target_chapter) +
                                     " of title " + std::to_string(action.target_title) +
                                     ", which has only " + std::to_string(chapter_count) + " chapter(s)");
    };

    auto menus = resolve_menu_tree(p.menu);
    // Menu timing is source-driven whenever motion video or menu audio is
    // present.  Explicit menu audio wins over audio embedded in the background
    // video.  With no explicit audio, use the video's first audio stream when
    // one exists. When loop_media is enabled the shorter source repeats to
    // fill the cycle; otherwise shorter audio ends naturally and shorter video
    // holds its final frame until the longer source completes.
    for (std::size_t mi = 0; mi < menus.size(); ++mi) {
        auto& menu = menus[mi].menu;
        const auto menu_probe = probe_dir / ("menu-" + std::to_string(mi + 1U));
        double video_duration = 0.0;
        double audio_duration = 0.0;
        if (!menu.background_video.empty()) {
            const auto video_present_file = menu_probe / "video-present.txt";
            if (!probe_stream_present(tools_, menu.background_video, "v:0", video_present_file))
                throw std::runtime_error("menu '" + menu.name + "' background video has no video stream: " + menu.background_video.string());
            video_duration = probe_stream_duration_seconds(tools_, menu.background_video, "v:0", menu_probe / "video-duration.txt");
            if (!(video_duration > 0.0))
                throw std::runtime_error("could not determine background-video duration for menu '" + menu.name + "': " + menu.background_video.string());
        }

        fs::path chosen_audio = menu.audio_source;
        if (!chosen_audio.empty()) {
            if (!probe_stream_present(tools_, chosen_audio, "a:0", menu_probe / "audio-present.txt"))
                throw std::runtime_error("menu '" + menu.name + "' audio source has no audio stream: " + chosen_audio.string());
            audio_duration = probe_stream_duration_seconds(tools_, chosen_audio, "a:0", menu_probe / "audio-duration.txt");
            if (!(audio_duration > 0.0))
                throw std::runtime_error("could not determine audio duration for menu '" + menu.name + "': " + chosen_audio.string());
        } else if (!menu.background_video.empty() &&
                   probe_stream_present(tools_, menu.background_video, "a:0", menu_probe / "embedded-audio-present.txt")) {
            chosen_audio = menu.background_video;
            audio_duration = probe_stream_duration_seconds(tools_, chosen_audio, "a:0", menu_probe / "embedded-audio-duration.txt");
            if (!(audio_duration > 0.0)) audio_duration = video_duration;
        }

        if (!menu.background_video.empty() || !menu.audio_source.empty()) {
            menu.duration_seconds = std::max(video_duration, audio_duration);
            if (!(menu.duration_seconds > 0.0))
                throw std::runtime_error("could not determine automatic duration for menu '" + menu.name + "'");
            std::ostringstream timing_message;
            timing_message << "Menu " << (mi + 1U) << " (" << menu.name << ") duration automatically set to "
                           << std::fixed << std::setprecision(3) << menu.duration_seconds << " s";
            reporter.emit(1.0, timing_message.str());
        }
        if (!menu.audio_source.empty() && !menu.background_video.empty())
            reporter.emit(1.0, "Using explicit menu audio for " + menu.name + "; background-video audio is ignored");
        else if (menu.audio_source.empty() && chosen_audio == menu.background_video && !chosen_audio.empty())
            reporter.emit(1.0, "Using audio embedded in the background video for menu " + menu.name);
        menu.audio_source = std::move(chosen_audio);
    }
    // Capacity planning happens after source/menu durations are known but before any
    // long-running encode.  The model deliberately reserves explicit filesystem,
    // muxing, subtitle/menu-authoring, and safety overhead so automatic mode errs
    // on the side of producing an image that is smaller than the selected medium.
    struct CapacityVideoSegment {
        double seconds = 0.0;
        int configured_kbps = 0;
        int minimum_kbps = 0;
        int maximum_kbps = 0;
        std::string label;
    };
    struct CapacityPlan {
        std::vector<int> menu_video_kbps;
        std::vector<int> title_video_kbps;
        std::uint64_t fixed_overhead_bytes = 0;
        std::uint64_t safety_bytes = 0;
        double mux_factor = 1.0;
        double audio_kbit_seconds = 0.0;
        double subtitle_kbit_seconds = 0.0;
        double video_kbit_seconds = 0.0;
        int waterline_kbps = 0;
    } capacity_plan;
    capacity_plan.menu_video_kbps.resize(menus.size());
    capacity_plan.title_video_kbps.resize(p.titles.size());

    auto estimate_encoded_audio_kbps = [&](const fs::path& source, const EncodingProfile& configured,
                                           int source_ordinal, int requested_channels, double duration,
                                           const fs::path& scratch, const std::string& where) {
        EncodingProfile resolved = configured;
        int source_channels = 2;
        if (!source.empty()) source_channels = audio_channels(tools_, source, scratch, source_ordinal);
        if (lossless_audio_codec(resolved.audio_codec) && !source.empty()) {
            const auto info = probe_audio_stream(tools_, source, scratch / "format", source_ordinal, duration);
            resolved.lpcm_sample_rate_hz = resolved_lossless_sample_rate_hz(
                p.target, configured.lpcm_sample_rate_hz, info.sample_rate);
            resolved.lpcm_bit_depth = resolved_lossless_bit_depth(
                p.target, configured.lpcm_bit_depth, info.bits_per_sample);
            if (info.channels > 0) source_channels = info.channels;
        } else if (resolved.audio_codec == AudioCodec::Lpcm) {
            resolved.lpcm_sample_rate_hz = resolved_lossless_sample_rate_hz(
                p.target, configured.lpcm_sample_rate_hz, 48000);
            resolved.lpcm_bit_depth = resolved_lossless_bit_depth(
                p.target, configured.lpcm_bit_depth, 16);
        }
        const int channels = resolved_audio_output_channels(source_channels, resolved, p.target,
                                                             requested_channels, where);
        if (resolved.audio_codec == AudioCodec::TrueHdAc3) {
            // A raw-PCM-plus-AC-3-core upper bound is conservative for the
            // variable-rate TrueHD payload and avoids optimistic disc fitting.
            return lpcm_bitrate_kbps(resolved.lpcm_sample_rate_hz, resolved.lpcm_bit_depth, channels) +
                   resolved.ac3_bitrate_kbps;
        }
        return configured_audio_rate_kbps(resolved, p.target, channels);
    };

    std::vector<int> menu_audio_kbps(menus.size(), 0);
    std::vector<int> title_audio_kbps(p.titles.size(), 0);
    const bool dvd_any_menu_audio = target_is_dvd(p.target) &&
        std::any_of(menus.begin(), menus.end(), [](const auto& rm) { return !rm.menu.audio_source.empty(); });
    for (std::size_t mi = 0; mi < menus.size(); ++mi) {
        const auto& menu = menus[mi].menu;
        const auto menu_encoding = encoding_for_target(menu, p.target);
        if (!menu.audio_source.empty() || dvd_any_menu_audio) {
            menu_audio_kbps[mi] = estimate_encoded_audio_kbps(
                menu.audio_source, menu_encoding, 0, 0, menu.duration_seconds,
                probe_dir / ("capacity-menu-audio-" + std::to_string(mi + 1U)),
                "menu '" + menu.name + "' audio");
        }
        capacity_plan.audio_kbit_seconds += static_cast<double>(menu_audio_kbps[mi]) * menu.duration_seconds;
    }
    for (std::size_t ti = 0; ti < p.titles.size(); ++ti) {
        const auto& title = p.titles[ti];
        int audio_total = 0;
        for (const auto& track : title_tracks[ti].audio) {
            const auto stream_encoding = effective_audio_encoding_for_stream(title, p.target, track.source_ordinal);
            const auto* settings = find_audio_stream_settings(title, track.source_ordinal);
            audio_total += estimate_encoded_audio_kbps(
                title.source, stream_encoding, track.source_ordinal,
                settings ? settings->output_channels : 0, title_durations[ti],
                probe_dir / ("capacity-title-" + std::to_string(ti + 1U) + "-audio-" + std::to_string(track.source_ordinal + 1)),
                "title " + std::to_string(ti + 1U) + " audio stream " + std::to_string(track.source_ordinal + 1));
        }
        title_audio_kbps[ti] = audio_total;
        capacity_plan.audio_kbit_seconds += static_cast<double>(audio_total) * title_durations[ti];
        // PGS/text subtitle sizes are content-dependent. Reserve a conservative
        // 256 kb/s average per authored subtitle stream in addition to the fixed
        // navigation/filesystem reserve below.
        constexpr int kSubtitleReserveKbps = 256;
        capacity_plan.subtitle_kbit_seconds += static_cast<double>(kSubtitleReserveKbps) *
            static_cast<double>(title_tracks[ti].subtitles.size() + p.titles[ti].external_subtitles.size()) * title_durations[ti];
    }

    std::vector<CapacityVideoSegment> segments;
    segments.reserve(menus.size() + p.titles.size());
    auto maximum_average_video_kbps = [&](DiscTarget target, const EncodingProfile& encoding, int audio_kbps) {
        if (limits_.allow_exceeding_format_limits)
            return std::max(1, encoding.video_max_bitrate_kbps);
        const int codec_max = std::min(video_codec_max_bitrate_kbps(target, encoding.video_codec),
                                       encoding.video_max_bitrate_kbps);
        if (target_is_dvd(target))
            return std::max(0, std::min(codec_max,
                combined_transport_limit_kbps - std::max(0, audio_kbps)));
        return std::max(0, std::min(codec_max,
            combined_transport_limit_kbps - std::max(0, audio_kbps)));
    };
    for (std::size_t mi = 0; mi < menus.size(); ++mi) {
        const auto& e = encoding_for_target(menus[mi].menu, p.target);
        segments.push_back({menus[mi].menu.duration_seconds, e.video_bitrate_kbps, e.video_min_bitrate_kbps,
                            maximum_average_video_kbps(p.target, e, menu_audio_kbps[mi]),
                            "menu " + std::to_string(mi + 1U)});
    }
    for (std::size_t ti = 0; ti < p.titles.size(); ++ti) {
        const auto& e = encoding_for_target(p.titles[ti], p.target);
        segments.push_back({title_durations[ti], e.video_bitrate_kbps, e.video_min_bitrate_kbps,
                            maximum_average_video_kbps(p.target, e, title_audio_kbps[ti]),
                            "title " + std::to_string(ti + 1U)});
    }
    for (const auto& segment : segments) {
        if (segment.maximum_kbps < segment.minimum_kbps)
            throw std::runtime_error(segment.label + " video minrate of " + std::to_string(segment.minimum_kbps) +
                " kb/s exceeds its available maxrate of " + std::to_string(segment.maximum_kbps) + " kb/s");
        if (!p.automatic_video_bitrate && segment.configured_kbps > segment.maximum_kbps)
            throw std::runtime_error(segment.label + " manual video bitrate of " + std::to_string(segment.configured_kbps) +
                " kb/s exceeds the maximum " + std::to_string(segment.maximum_kbps) +
                " kb/s available after reserving audio/transport bandwidth for the selected format");
    }

    constexpr std::uint64_t MiB = 1024ULL * 1024ULL;
    capacity_plan.fixed_overhead_bytes = target_is_dvd(p.target) ? 64ULL * MiB : 128ULL * MiB;
    // Menus, control files, thumbnails/interactive graphics, directory padding,
    // and similar small authored objects grow with project complexity.
    capacity_plan.fixed_overhead_bytes += static_cast<std::uint64_t>(menus.size()) * 4ULL * MiB;
    capacity_plan.fixed_overhead_bytes += static_cast<std::uint64_t>(p.titles.size()) * 2ULL * MiB;
    capacity_plan.mux_factor = 1.04;
    if (!disc_capacity_is_unlimited(p.disc_capacity_bytes))
        capacity_plan.safety_bytes = p.disc_capacity_bytes / 50ULL; // 2% final-image safety margin

    auto video_cost_at_waterline = [&](int waterline_kbps) {
        double cost = 0.0;
        for (const auto& segment : segments)
            cost += static_cast<double>(std::clamp(waterline_kbps, segment.minimum_kbps, segment.maximum_kbps)) * segment.seconds;
        return cost;
    };

    double available_video_kbit_seconds = std::numeric_limits<double>::infinity();
    if (!disc_capacity_is_unlimited(p.disc_capacity_bytes)) {
        const std::uint64_t reserved_bytes = capacity_plan.fixed_overhead_bytes + capacity_plan.safety_bytes;
        if (reserved_bytes >= p.disc_capacity_bytes)
            throw std::runtime_error("selected disc capacity is too small for filesystem/authoring overhead");
        const double elementary_kbit_budget =
            (static_cast<double>(p.disc_capacity_bytes - reserved_bytes) * 8.0 / capacity_plan.mux_factor) / 1000.0;
        available_video_kbit_seconds = elementary_kbit_budget -
            capacity_plan.audio_kbit_seconds - capacity_plan.subtitle_kbit_seconds;
        if (!(available_video_kbit_seconds > 0.0))
            throw std::runtime_error("selected disc capacity is too small for the configured audio, subtitles, muxing, filesystem overhead, and safety margin");
        if (video_cost_at_waterline(0) > available_video_kbit_seconds + 0.5)
            throw std::runtime_error("selected disc capacity cannot satisfy the configured video minrate values after reserving audio and authoring overhead");
    }

    int maximum_segment_bitrate = 0;
    for (const auto& segment : segments) maximum_segment_bitrate = std::max(maximum_segment_bitrate, segment.maximum_kbps);
    if (disc_capacity_is_unlimited(p.disc_capacity_bytes)) {
        capacity_plan.waterline_kbps = maximum_segment_bitrate;
    } else {
        int low = 0, high = maximum_segment_bitrate;
        while (low < high) {
            const int mid = low + (high - low + 1) / 2;
            if (video_cost_at_waterline(mid) <= available_video_kbit_seconds) low = mid;
            else high = mid - 1;
        }
        capacity_plan.waterline_kbps = low;
    }

    std::size_t segment_index = 0;
    for (std::size_t mi = 0; mi < menus.size(); ++mi, ++segment_index) {
        const auto& segment = segments[segment_index];
        capacity_plan.menu_video_kbps[mi] = p.automatic_video_bitrate
            ? std::clamp(capacity_plan.waterline_kbps, segment.minimum_kbps, segment.maximum_kbps) : segment.configured_kbps;
        capacity_plan.video_kbit_seconds += static_cast<double>(capacity_plan.menu_video_kbps[mi]) * segment.seconds;
    }
    for (std::size_t ti = 0; ti < p.titles.size(); ++ti, ++segment_index) {
        const auto& segment = segments[segment_index];
        capacity_plan.title_video_kbps[ti] = p.automatic_video_bitrate
            ? std::clamp(capacity_plan.waterline_kbps, segment.minimum_kbps, segment.maximum_kbps) : segment.configured_kbps;
        capacity_plan.video_kbit_seconds += static_cast<double>(capacity_plan.title_video_kbps[ti]) * segment.seconds;
    }

    if (p.automatic_video_bitrate) {
        if (capacity_plan.waterline_kbps < 1)
            throw std::runtime_error("selected disc capacity leaves no usable video bitrate after reserving audio and authoring overhead");
        std::ostringstream message;
        message << "Automatic video bitrate target: " << capacity_plan.waterline_kbps << " kb/s";
        if (disc_capacity_is_unlimited(p.disc_capacity_bytes)) message << " (Unlimited size; format/transport maximum)";
        else message << " for " << std::fixed << std::setprecision(1)
                     << (static_cast<double>(p.disc_capacity_bytes) / 1000000000.0) << " GB target";
        reporter.emit(1.0, message.str());
    } else if (!disc_capacity_is_unlimited(p.disc_capacity_bytes)) {
        const double configured_video_cost = capacity_plan.video_kbit_seconds;
        if (configured_video_cost > available_video_kbit_seconds + 0.5) {
            const double estimated_bytes = static_cast<double>(capacity_plan.fixed_overhead_bytes) +
                (capacity_plan.audio_kbit_seconds + capacity_plan.subtitle_kbit_seconds + configured_video_cost) *
                    1000.0 / 8.0 * capacity_plan.mux_factor;
            std::ostringstream error;
            error << "configured manual video bitrate exceeds the selected disc capacity. "
                  << "The project is estimated to require " << std::fixed << std::setprecision(2)
                  << (estimated_bytes / 1000000000.0) << " GB before the 2% safety margin, but the target is "
                  << (static_cast<double>(p.disc_capacity_bytes) / 1000000000.0) << " GB. "
                  << "The maximum common video bitrate that fits this project is about "
                  << capacity_plan.waterline_kbps << " kb/s. Reduce the manual bitrate, enable automatic bitrate, "
                  << "choose a larger size target, or select Unlimited.";
            throw std::runtime_error(error.str());
        }
    }

    const auto default_startup = (menus.empty() && p.first_play_actions.empty())
        ? default_menuless_startup_actions(p.titles.size()) : std::vector<NavigationAction>{};
    const std::span<const NavigationAction> startup_actions = default_startup.empty()
        ? std::span<const NavigationAction>(p.first_play_actions) : std::span<const NavigationAction>(default_startup);
    for (const auto& resolved : menus) {
        for (const auto& button : resolved.menu.buttons) {
            for (const auto& action : button_action_sequence(button)) {
                if (!navigation_action_is_title_scoped(action.kind)) continue;
                const auto title_index = static_cast<std::size_t>(action.target_title - 1U);
                if (title_index >= title_tracks.size()) continue; // validate_project already reports this case
                if (action.kind == NavigationActionKind::AudioTrack &&
                    action.target_stream > title_tracks[title_index].audio.size())
                    throw std::runtime_error("menu button selects audio stream " + std::to_string(action.target_stream) +
                                             " but title " + std::to_string(action.target_title) + " has only " +
                                             std::to_string(title_tracks[title_index].audio.size()) + " audio stream(s)");
                if (action.kind == NavigationActionKind::SubtitleTrack &&
                    action.target_stream > title_tracks[title_index].subtitles.size())
                    throw std::runtime_error("menu button selects subtitle stream " + std::to_string(action.target_stream) +
                                             " but title " + std::to_string(action.target_title) + " has only " +
                                             std::to_string(title_tracks[title_index].subtitles.size()) +
                                             " Blu-ray-compatible subtitle stream(s)");
                validate_chapter_action(action, "menu button");
            }
        }
    }
    for (const auto& action : startup_actions) {
        if (!navigation_action_is_title_scoped(action.kind)) continue;
        const auto title_index = static_cast<std::size_t>(action.target_title - 1U);
        if (title_index >= title_tracks.size()) continue;
        if (action.kind == NavigationActionKind::AudioTrack && action.target_stream > title_tracks[title_index].audio.size())
            throw std::runtime_error("First Playback selects unavailable audio stream " + std::to_string(action.target_stream));
        if (action.kind == NavigationActionKind::SubtitleTrack && action.target_stream > title_tracks[title_index].subtitles.size())
            throw std::runtime_error("First Playback selects unavailable subtitle stream " + std::to_string(action.target_stream));
        validate_chapter_action(action, "First Playback");
    }
    for(std::size_t owner=0;owner<p.titles.size();++owner){for(const auto& action:p.titles[owner].menu_button_actions){if(!navigation_action_is_title_scoped(action.kind))continue;const auto title_index=static_cast<std::size_t>(action.target_title-1U);if(title_index>=title_tracks.size())continue;if(action.kind==NavigationActionKind::AudioTrack&&action.target_stream>title_tracks[title_index].audio.size())throw std::runtime_error("title Menu-button action selects unavailable audio stream "+std::to_string(action.target_stream));if(action.kind==NavigationActionKind::SubtitleTrack&&action.target_stream>title_tracks[title_index].subtitles.size())throw std::runtime_error("title Menu-button action selects unavailable subtitle stream "+std::to_string(action.target_stream));validate_chapter_action(action,"title Menu-button action");}}

    if (target_is_dvd(p.target)) {
        if (p.titles.size() > 99U) throw std::runtime_error("DVD-Video supports at most 99 titles");
        if (menus.size() > 99U) throw std::runtime_error("DVD-Video backend currently supports at most 99 visible menus");
        for (std::size_t i = 0; i < title_tracks.size(); ++i) {
            if (resolved_title_chapters[i].size() + 1U > 99U)
                throw std::runtime_error("DVD-Video supports at most 99 chapters per title");
            if (title_tracks[i].audio.size() > 8U)
                throw std::runtime_error("DVD-Video supports at most 8 audio streams per title");
            if (title_tracks[i].subtitles.size() > 32U)
                throw std::runtime_error("DVD-Video supports at most 32 subtitle streams per title");
            for (const auto& sub : title_tracks[i].subtitles)
                if (sub.codec_name == "hdmv_pgs_subtitle")
                    throw std::runtime_error("DVD-Video output cannot convert PGS subtitle track " +
                                             std::to_string(sub.source_ordinal + 1) + " in title " + std::to_string(i + 1) +
                                             "; use a text subtitle source for DVD rendering");
        }
        const bool menu_four_three = normalize_aspect_token(menu_aspect_ratio_for_target(p,p.target)) == "4:3";
        for (const auto& rm : menus) {
            // DVD permits 36 buttons in a 4:3 PGC. Widescreen menus need
            // parallel display-mode button groups, limiting each group to 18.
            const std::size_t button_limit = menu_four_three ? 36U : 18U;
            if (rm.menu.buttons.size() > button_limit)
                throw std::runtime_error("DVD-Video " + std::string(menu_four_three ? "4:3" : "16:9") +
                    " menus support at most " + std::to_string(button_limit) + " buttons per menu");
        }

        const bool any_menu_audio = std::any_of(menus.begin(), menus.end(), [](const ResolvedMenu& rm) {
            return !rm.menu.audio_source.empty();
        });
        AudioCodec menu_audio_codec = AudioCodec::Ac3;
        if (any_menu_audio) {
            bool chosen = false;
            for (const auto& rm : menus) {
                if (rm.menu.audio_source.empty()) continue;
                const auto codec = encoding_for_target(rm.menu, p.target).audio_codec;
                if (!chosen) { menu_audio_codec = codec; chosen = true; }
                else if (codec != menu_audio_codec)
                    throw std::runtime_error("all DVD VMGM menus with audio must use the same audio codec (AC-3 or LPCM)");
            }
        }

        // DVD-Video may mix Film (23.976p) and NTSC (29.97i) titles because
        // both belong to the 525/60 family. PAL uses the separate 625/50
        // family. Resolution selections participate in the same family check.
        std::optional<bool> dvd_pal_family;
        auto merge_family = [&](std::optional<bool> family) {
            if (!family) return;
            if (dvd_pal_family && *dvd_pal_family != *family)
                throw std::runtime_error("DVD-Video cannot mix PAL titles/resolutions with Film/NTSC titles/resolutions on the same disc");
            dvd_pal_family = family;
        };
        merge_family(explicit_dvd_pal_family(p.frame_rate));
        if (!menus.empty()) merge_family(dvd_family_from_resolution(menu_resolution_for_target(p,p.target)));
        for (const auto& title : p.titles) {
            merge_family(explicit_dvd_pal_family(frame_rate_for_target(title, p.target)));
            merge_family(dvd_family_from_resolution(resolution_for_target(title, p.target)));
        }
        if (!dvd_pal_family) {
            const auto first_source = probe_video_timing_metadata(tools_, p.titles.front().source, work / "dvd-disc-timing.txt");
            if (first_source.height == 576 || first_source.height == 288) dvd_pal_family = true;
            else if (first_source.height == 480 || first_source.height == 240) dvd_pal_family = false;
            else {
                const auto first_timing = resolve_video_timing_for_rate(DiscTarget::DvdVideo480p, "auto",
                    first_source.frame_rate, timing_source_interlaced(first_source));
                dvd_pal_family = dvd_timing_is_pal(first_timing);
            }
        }
        const bool dvd_disc_pal = *dvd_pal_family;
        const auto project_dvd_rate = normalize_frame_rate_for_target(DiscTarget::DvdVideo480p, p.frame_rate);
        const std::string dvd_menu_rate = (project_dvd_rate.empty() || project_dvd_rate == "auto")
            ? (dvd_disc_pal ? "pal-dvd" : "ntsc-dvd") : p.frame_rate;
        VideoStreamInfo dvd_menu_source;
        dvd_menu_source.frame_rate = dvd_disc_pal ? 25.0 : 30000.0/1001.0;
        dvd_menu_source.field_order = "tt";
        const auto dvd_timing = menus.empty()
            ? resolve_output_mode(DiscTarget::DvdVideo480p, dvd_disc_pal ? "720x576" : "720x480", "16:9",
                                  dvd_menu_rate, dvd_menu_source, true, VideoCodec::Mpeg2, dvd_disc_pal)
            : resolve_output_mode(DiscTarget::DvdVideo480p, menu_resolution_for_target(p,p.target), menu_aspect_ratio_for_target(p,p.target),
                                  dvd_menu_rate, dvd_menu_source, true, VideoCodec::Mpeg2, dvd_disc_pal);
        if (!menus.empty() && dvd_timing_is_pal(dvd_timing) != dvd_disc_pal)
            throw std::runtime_error("DVD-Video menu/default mode conflicts with the selected title video family");
        const auto geometry = target_geometry_for_timing(p.target, dvd_timing);
        const std::string dvd_format = dvd_video_format(dvd_timing);
        reporter.emit(2.0, "Authoring DVD-Video MPEG-2/VOB assets — " +
            std::string(dvd_disc_pal ? "PAL 625/50 family" : "NTSC/Film 525/60 family"));

        const auto pass_count = [](const EncodingProfile& e) { return e.two_pass ? 2.0 : 1.0; };
        const auto work_duration = [](double seconds) { return seconds > 0.0 ? seconds : 1.0; };
        double total_dvd_video_work = 0.0;
        for (const auto& rm : menus)
            total_dvd_video_work += work_duration(rm.menu.duration_seconds) * pass_count(encoding_for_target(rm.menu, p.target));
        for (std::size_t i = 0; i < p.titles.size(); ++i)
            total_dvd_video_work += work_duration(title_durations[i]) * pass_count(encoding_for_target(p.titles[i], p.target));
        double completed_dvd_video_work = 0.0;
        auto dvd_video_percent = [&](double work_done) {
            if (total_dvd_video_work <= 0.0) return 3.0;
            return 3.0 + 75.0 * std::clamp(work_done / total_dvd_video_work, 0.0, 1.0);
        };
        auto dvd_progress_for_video = [&](double units, std::string label) -> VideoProgressCallback {
            const double before = completed_dvd_video_work;
            return [&, before, units, label = std::move(label)](double fraction, int pass, int passes) {
                const double pass_fraction = std::clamp(
                    fraction * static_cast<double>(passes) - static_cast<double>(pass - 1), 0.0, 1.0);
                std::ostringstream msg;
                msg << label;
                if (passes > 1) msg << " — pass " << pass << "/" << passes;
                msg << " — " << std::fixed << std::setprecision(1) << pass_fraction * 100.0 << "%";
                reporter.emit(dvd_video_percent(before + units * std::clamp(fraction, 0.0, 1.0)), msg.str());
            };
        };
        std::vector<fs::path> dvd_menu_vobs;
        dvd_menu_vobs.reserve(menus.size());
        for (std::size_t mi = 0; mi < menus.size(); ++mi) {
            const auto& source_menu = menus[mi].menu;
            auto menu = scale_menu_from_design(source_menu, geometry.video_width, geometry.video_height, dvd_timing.aspect_ratio);
            auto encoding = encoding_for_target(source_menu, p.target);
            if (p.automatic_video_bitrate) encoding.video_bitrate_kbps = capacity_plan.menu_video_kbps.at(mi);
            auto md = work / ("dvd-menu-" + std::to_string(mi + 1U));
            fs::create_directories(md);

            EncodingProfile menu_audio_encoding = encoding;
            menu_audio_encoding.audio_codec = menu_audio_codec;
            if (!limits_.allow_exceeding_format_limits && menu_audio_codec == AudioCodec::Ac3)
                menu_audio_encoding.ac3_bitrate_kbps = std::min(menu_audio_encoding.ac3_bitrate_kbps, 448);
            int menu_audio_channels = 0;
            int menu_audio_rate_kbps = 0;
            if (any_menu_audio) {
                const int source_channels = menu.audio_source.empty()
                    ? 2 : audio_channels(tools_, menu.audio_source, md / "menu-audio-channels", 0);
                menu_audio_channels = resolved_audio_output_channels(source_channels, menu_audio_encoding,
                    DiscTarget::DvdVideo480p, 0, "DVD menu audio stream");
                menu_audio_rate_kbps = configured_audio_rate_kbps(menu_audio_encoding,
                    DiscTarget::DvdVideo480p, menu_audio_channels);
            }
            const int dvd_video_peak_kbps = dvd_mpeg2_vbr_peak_kbps(encoding.video_bitrate_kbps, encoding.video_max_bitrate_kbps,
                menu_audio_rate_kbps, "DVD menu " + std::to_string(mi + 1U), limits_.allow_exceeding_format_limits);
            const double units = work_duration(menu.duration_seconds) * pass_count(encoding);
            const std::string encode_label = "Encoding DVD menu " + std::to_string(mi + 1U) + " of " + std::to_string(menus.size());
            reporter.emit(dvd_video_percent(completed_dvd_video_work),
                          encode_label + (encoding.two_pass ? " — pass 1/2 — 0%" : " — 0%"));

            const auto visuals = prepare_dvd_menu_button_visuals(tools_, menu, md);
            const bool motion = !menu.background_video.empty();
            const auto background = motion ? menu.background_video : prepare_menu_background(tools_, menu, md);
            const auto label_overlay = motion ? prepare_menu_overlay(tools_, menu, md) : fs::path{};
            const auto combined_overlay = combine_rgba_overlays(tools_, label_overlay, visuals.normal_overlay,
                                                                 geometry.video_width, geometry.video_height, md);
            VideoStreamInfo timing_source;
            if (motion) timing_source = probe_video_timing_metadata(tools_, background, md / "timing.txt");
            else { timing_source.frame_rate = 30000.0 / 1001.0; timing_source.field_order = "progressive"; }
            const auto& timing = dvd_timing;
            reporter.emit(dvd_video_percent(completed_dvd_video_work), encode_label + " — output timing " + timing.description);
            fs::path video;
            fs::path menu_video_cache_file;
            if (!encode_cache_root.empty()) {
                menu_video_cache_file = encode_cache_root / "menu-video" /
                    (menu_video_cache_key(background, combined_overlay, encoding, p, timing, tools_,
                                          timing_source_interlaced(timing_source), !motion, menu.duration_seconds,
                                          menu.background_color, menu.loop_media, dvd_video_peak_kbps) + video_extension(encoding));
            }
            const auto menu_video_metadata = video_cache_metadata(
                "menu-video", motion ? source_menu.background_video : source_menu.background_image,
                p.target, encoding, timing, tools_);
            if (!menu_video_cache_file.empty() && !p.force_reencode && !p.refresh_encode_cache && cache_file_usable(menu_video_cache_file)) {
                video = menu_video_cache_file;
                touch_cache_metadata(menu_video_cache_file, menu_video_metadata);
                reporter.emit(dvd_video_percent(completed_dvd_video_work + units),
                              "Using cached encoded video for DVD menu " + std::to_string(mi + 1U));
            } else if (!menu_video_cache_file.empty()) {
                const auto cache_tmp_dir = cache_encode_temp_dir(menu_video_cache_file);
                CacheTempDirCleanup cache_tmp_cleanup{cache_tmp_dir};
                const auto cache_tmp_base = cache_tmp_dir / "video";
                (void)encode_video(tools_, background, cache_tmp_base, encoding, p, timing,
                                   timing_source_interlaced(timing_source), !motion, menu.duration_seconds,
                                   menu.duration_seconds, dvd_progress_for_video(units, encode_label),
                                   menu.background_color, motion && menu.loop_media, combined_overlay, motion && !menu.loop_media,
                                   dvd_video_peak_kbps);
                const fs::path cache_tmp_output = cache_tmp_base.string() + video_extension(encoding);
                finalize_encoded_cache_file(cache_tmp_output, menu_video_cache_file, p.force_reencode || p.refresh_encode_cache, menu_video_metadata);
                video = menu_video_cache_file;
            } else {
                (void)encode_video(tools_, background, md / "video", encoding, p, timing,
                                   timing_source_interlaced(timing_source), !motion, menu.duration_seconds,
                                   menu.duration_seconds, dvd_progress_for_video(units, encode_label),
                                   menu.background_color, motion && menu.loop_media, combined_overlay, motion && !menu.loop_media,
                                   dvd_video_peak_kbps);
                video = md / "video.m2v";
            }
            completed_dvd_video_work += units;

            std::vector<DvdAudioElementary> audio;
            if (any_menu_audio) {
                auto ad = md / "audio";
                if (menu.audio_source.empty()) {
                    audio.push_back(encode_dvd_silence(tools_, ad, menu_audio_encoding, menu.duration_seconds));
                } else if (!encode_cache_root.empty()) {
                    const auto& audio_fp = source_fingerprint(menu.audio_source);
                    const auto audio_cache_file = encode_cache_root / "menu-audio" /
                        (menu_audio_cache_key(audio_fp, menu_audio_encoding, p.target, menu.duration_seconds, true, menu.loop_media) +
                         dvd_audio_cache_extension(menu_audio_encoding.audio_codec));
                    const int channels = menu_audio_channels;
                    const auto menu_audio_metadata = audio_cache_metadata(
                        "menu-audio", menu.audio_source, p.target, menu_audio_encoding, channels, 0);
                    if (!p.force_reencode && !p.refresh_encode_cache && cache_file_usable(audio_cache_file)) {
                        touch_cache_metadata(audio_cache_file, menu_audio_metadata);
                        reporter.emit(dvd_video_percent(completed_dvd_video_work),
                                      "Using cached encoded audio for DVD menu " + std::to_string(mi + 1U));
                        audio.push_back({audio_cache_file, channels, menu_audio_encoding.audio_codec,
                                         menu_audio_encoding.audio_codec == AudioCodec::Lpcm ? menu_audio_encoding.lpcm_sample_rate_hz : 48000,
                                         menu_audio_encoding.audio_codec == AudioCodec::Lpcm ? menu_audio_encoding.lpcm_bit_depth : 16});
                    } else {
                        const auto cache_tmp_dir = cache_encode_temp_dir(audio_cache_file);
                        CacheTempDirCleanup cache_tmp_cleanup{cache_tmp_dir};
                        const auto encoded = encode_dvd_audio_elementary(tools_, menu.audio_source, cache_tmp_dir,
                                                                         menu_audio_encoding, menu.loop_media, menu.duration_seconds, 0, 0);
                        finalize_encoded_cache_file(encoded.path, audio_cache_file, p.force_reencode || p.refresh_encode_cache, menu_audio_metadata);
                        audio.push_back({audio_cache_file, encoded.channels, encoded.codec, encoded.sample_rate_hz, encoded.bit_depth});
                    }
                } else {
                    audio.push_back(encode_dvd_audio_elementary(tools_, menu.audio_source, ad, menu_audio_encoding,
                                                                 menu.loop_media, menu.duration_seconds, 0, 0));
                }
            }
            const auto base_vob = md / "menu-base.mpg";
            mux_dvd_program_stream(tools_, video, audio, base_vob, combined_transport_limit_kbps);
            const auto menu_vob = md / "menu.mpg";
            spumux_dvd_menu(tools_, base_vob, menu_vob, md / "spumux-menu.xml", menu, visuals, dvd_timing);
            dvd_menu_vobs.push_back(menu_vob);
        }

        std::vector<fs::path> dvd_title_vobs;
        std::vector<VideoTimingMode> dvd_title_timings;
        dvd_title_vobs.reserve(p.titles.size());
        dvd_title_timings.reserve(p.titles.size());
        for (std::size_t i = 0; i < p.titles.size(); ++i) {
            const auto& title = p.titles[i];
            auto encoding = encoding_for_target(title, p.target);
            if (p.automatic_video_bitrate) encoding.video_bitrate_kbps = capacity_plan.title_video_kbps.at(i);
            auto td = work / ("dvd-title-" + std::to_string(i + 1U));
            fs::create_directories(td);
            const double units = work_duration(title_durations[i]) * pass_count(encoding);
            const std::string encode_label = "Encoding DVD title " + std::to_string(i + 1U) + " of " + std::to_string(p.titles.size());
            reporter.emit(dvd_video_percent(completed_dvd_video_work),
                          encode_label + (encoding.two_pass ? " — pass 1/2 — 0%" : " — 0%"));

            const auto timing_source = probe_video_timing_metadata(tools_, title.source, td / "timing.txt");
            const auto timing = resolve_output_mode(DiscTarget::DvdVideo480p,
                resolution_for_target(title,p.target), aspect_ratio_for_target(title,p.target),
                effective_title_frame_rate(p,title), timing_source, false, VideoCodec::Mpeg2, dvd_disc_pal);
            dvd_title_timings.push_back(timing);
            reporter.emit(dvd_video_percent(completed_dvd_video_work), encode_label + " — output timing " + timing.description);
            const bool force_reencode = p.force_reencode || title.force_reencode;
            std::optional<MediaFingerprint> fp;
            if (!encode_cache_root.empty()) fp = source_fingerprint(title.source);

            std::vector<int> planned_audio_channels;
            planned_audio_channels.reserve(title_tracks[i].audio.size());
            int configured_audio_total_kbps = 0;
            for (const auto& track : title_tracks[i].audio) {
                const auto stream_encoding = effective_audio_encoding_for_stream(title, p.target, track.source_ordinal);
                validate_encoding_for_target(stream_encoding, p.target,
                    "DVD title " + std::to_string(i + 1U) + " audio stream " + std::to_string(track.source_ordinal + 1),
                    limits_.allow_exceeding_format_limits);
                const auto ad = td / ("audio-" + std::to_string(track.source_ordinal + 1));
                fs::create_directories(ad);
                const int source_channels = audio_channels(tools_, title.source, ad, track.source_ordinal);
                const auto* stream_settings = find_audio_stream_settings(title, track.source_ordinal);
                const int channels = resolved_audio_output_channels(source_channels, stream_encoding, p.target,
                    stream_settings ? stream_settings->output_channels : 0,
                    "DVD title " + std::to_string(i + 1U) + " audio stream " + std::to_string(track.source_ordinal + 1));
                planned_audio_channels.push_back(channels);
                configured_audio_total_kbps += configured_audio_rate_kbps(stream_encoding, p.target, channels);
            }
            const int dvd_video_peak_kbps = dvd_mpeg2_vbr_peak_kbps(encoding.video_bitrate_kbps, encoding.video_max_bitrate_kbps,
                configured_audio_total_kbps, "DVD title " + std::to_string(i + 1U), limits_.allow_exceeding_format_limits);

            fs::path video;
            fs::path video_cache;
            if (fp) video_cache = encode_cache_root / "video" /
                (video_cache_key(*fp, encoding, p, timing, tools_, dvd_video_peak_kbps) + video_extension(encoding));
            const auto title_video_metadata = video_cache_metadata("title-video", title.source, p.target, encoding, timing, tools_);
            if (fp && !force_reencode && !p.refresh_encode_cache && cache_file_usable(video_cache)) {
                video = video_cache;
                touch_cache_metadata(video_cache, title_video_metadata);
                reporter.emit(dvd_video_percent(completed_dvd_video_work + units), "Using cached encoded video for DVD title " + std::to_string(i + 1U));
            } else if (fp) {
                const auto temp_dir = cache_encode_temp_dir(video_cache);
                CacheTempDirCleanup cleanup{temp_dir};
                const auto base = temp_dir / "video";
                (void)encode_video(tools_, title.source, base, encoding, p, timing,
                                   timing_source_interlaced(timing_source), false, 0, title_durations[i],
                                   dvd_progress_for_video(units, encode_label), {0,0,0,255}, false, {}, false,
                                   dvd_video_peak_kbps);
                const fs::path temp_output = base.string() + video_extension(encoding);
                finalize_encoded_cache_file(temp_output, video_cache, force_reencode || p.refresh_encode_cache, title_video_metadata);
                video = video_cache;
            } else {
                const auto base = td / "video";
                (void)encode_video(tools_, title.source, base, encoding, p, timing,
                                   timing_source_interlaced(timing_source), false, 0, title_durations[i],
                                   dvd_progress_for_video(units, encode_label), {0,0,0,255}, false, {}, false,
                                   dvd_video_peak_kbps);
                video = base.string() + video_extension(encoding);
            }

            completed_dvd_video_work += units;

            std::vector<DvdAudioElementary> audio;
            audio.reserve(title_tracks[i].audio.size());
            std::size_t audio_plan_index = 0;
            for (const auto& track : title_tracks[i].audio) {
                const auto stream_encoding = effective_audio_encoding_for_stream(title, p.target, track.source_ordinal);
                const auto ad = td / ("audio-" + std::to_string(track.source_ordinal + 1));
                const auto* stream_settings = find_audio_stream_settings(title, track.source_ordinal);
                const int channels = planned_audio_channels.at(audio_plan_index++);
                fs::path cached;
                if (fp) cached = encode_cache_root / "audio" /
                    (dvd_audio_cache_key(*fp, stream_encoding, track.source_ordinal, stream_settings ? stream_settings->output_channels : 0) + dvd_audio_cache_extension(stream_encoding.audio_codec));
                const auto title_audio_metadata = audio_cache_metadata(
                    "title-audio", title.source, p.target, stream_encoding, channels, track.source_ordinal);
                if (fp && !force_reencode && !p.refresh_encode_cache && cache_file_usable(cached)) {
                    touch_cache_metadata(cached, title_audio_metadata);
                    audio.push_back({cached, channels, stream_encoding.audio_codec,
                        stream_encoding.audio_codec == AudioCodec::Lpcm ? stream_encoding.lpcm_sample_rate_hz : 48000,
                        stream_encoding.audio_codec == AudioCodec::Lpcm ? stream_encoding.lpcm_bit_depth : 16});
                } else if (fp) {
                    const auto temp_dir = cache_encode_temp_dir(cached);
                    CacheTempDirCleanup cleanup{temp_dir};
                    const auto encoded = encode_dvd_audio_elementary(tools_, title.source, temp_dir, stream_encoding, false, 0, track.source_ordinal, stream_settings ? stream_settings->output_channels : 0);
                    finalize_encoded_cache_file(encoded.path, cached, force_reencode || p.refresh_encode_cache, title_audio_metadata);
                    audio.push_back({cached, encoded.channels, encoded.codec, encoded.sample_rate_hz, encoded.bit_depth});
                } else {
                    audio.push_back(encode_dvd_audio_elementary(tools_, title.source, ad, stream_encoding, false, 0, track.source_ordinal, stream_settings ? stream_settings->output_channels : 0));
                }
            }

            auto current = td / "title-base.mpg";
            mux_dvd_program_stream(tools_, video, audio, current, combined_transport_limit_kbps);
            for (std::size_t si = 0; si < title_tracks[i].subtitles.size(); ++si) {
                const auto sd = td / ("subtitle-" + std::to_string(si + 1U));
                const auto srt = extract_dvd_text_subtitle(tools_, title.source, sd, title_tracks[i].subtitles[si]);
                const auto next = td / ("title-sub-" + std::to_string(si + 1U) + ".mpg");
                spumux_dvd_text_subtitle(tools_, current, next, sd / "spumux.xml", srt, si, timing, title_tracks[i].subtitles[si].subtitle_style);
                current = next;
            }
            dvd_title_vobs.push_back(current);
        }

        DvdNavigationCompiler nav(menus, p.titles.size());
        std::vector<std::vector<std::string>> button_commands(menus.size());
        for (std::size_t mi = 0; mi < menus.size(); ++mi) {
            for (const auto& button : menus[mi].menu.buttons)
                button_commands[mi].push_back(nav.compile(button_action_sequence(button), mi + 1U));
        }
        const std::size_t return_menu = menus.empty() ? 0U : 1U;
        std::string first_play = menus.empty()
            ? ("g0 = 0; g1 = 0; " + nav.compile(startup_actions, 0U))
            : "g0 = 0; g1 = 0; jump vmgm menu 1;";
        if (!menus.empty() && !startup_actions.empty()) first_play = "g0 = 0; g1 = 0; " + nav.compile(startup_actions, return_menu);
        std::vector<std::string> title_menu_commands;title_menu_commands.reserve(p.titles.size());for(const auto& title:p.titles)title_menu_commands.push_back(nav.compile(title.menu_button_actions,return_menu));

        const auto& continuations = nav.continuations();
        constexpr std::size_t DispatchShardSize = 96U;
        const std::size_t shard_count = continuations.empty() ? 0U :
            (continuations.size() + DispatchShardSize - 1U) / DispatchShardSize;
        const std::size_t first_shard_menu = menus.size() + continuations.size() + 1U;
        const std::size_t dispatcher_menu = first_shard_menu + shard_count;

        std::ostringstream xml;
        xml << "<dvdauthor format=\"" << dvd_format << "\">\n<vmgm>\n<fpc>" << first_play << "</fpc>\n"
            << "<menus lang=\"en\">\n<video format=\"" << dvd_format << "\" aspect=\"" << dvd_timing.aspect_ratio
            << "\" resolution=\"" << geometry.video_width << "x" << geometry.video_height << "\""
            << (dvd_timing.aspect_ratio == "16:9" ? " widescreen=\"nopanscan\"" : "") << "/>\n";
        if (any_menu_audio)
            xml << "<audio format=\"" << dvd_audio_format(menu_audio_codec)
                << "\" channels=\"2\" samplerate=\"48khz\" lang=\"en\"/>\n";
        for (std::size_t mi = 0; mi < menus.size(); ++mi) {
            const bool has_menu_media = !menus[mi].menu.background_video.empty() || !menus[mi].menu.audio_source.empty();
            const bool loop_menu_media = has_menu_media && menus[mi].menu.loop_media;
            xml << "<pgc" << (mi == 0U ? " entry=\"title\"" : "") << ">\n"
                << "<pre>g0 = 0; g1 = 0;</pre>\n"
                << "<vob file=\"" << xml_escape(dvd_menu_vobs[mi].string()) << "\""
                << (!loop_menu_media ? " pause=\"inf\"" : "") << "/>\n";
            for (std::size_t bi = 0; bi < button_commands[mi].size(); ++bi)
                xml << "<button name=\"b" << (bi + 1U) << "\">" << button_commands[mi][bi] << "</button>\n";
            if (loop_menu_media) xml << "<post>jump cell 1;</post>\n";
            xml << "</pgc>\n";
        }
        for (const auto& node : continuations)
            xml << "<pgc><pre>" << node.commands << "</pre></pgc>\n";
        for (std::size_t shard = 0; shard < shard_count; ++shard) {
            const auto begin = shard * DispatchShardSize;
            const auto end = std::min(continuations.size(), begin + DispatchShardSize);
            xml << "<pgc><pre>" << dvd_dispatch_shard_commands(menus.size(), continuations, begin, end)
                << "</pre></pgc>\n";
        }
        xml << "<pgc><pre>" << dvd_dispatch_root_commands(menus.size(), continuations.size(), first_shard_menu, DispatchShardSize)
            << "</pre></pgc>\n</menus>\n</vmgm>\n";

        for (std::size_t i = 0; i < p.titles.size(); ++i) {
            const auto& authored_title = p.titles[i];
            const auto title_geometry = target_geometry_for_timing(p.target, dvd_title_timings[i]);
            const auto title_format = dvd_video_format(dvd_title_timings[i]);
            xml << "<titleset>\n<menus><pgc entry=\"root\"><pre>"
                << dvd_chapter_dispatch_pre_commands(resolved_title_chapters[i].size() + 1U,title_menu_commands[i])
                << "</pre></pgc></menus>\n<titles>\n<video format=\"" << title_format << "\" aspect=\""
                << dvd_title_timings[i].aspect_ratio << "\" resolution=\""
                << title_geometry.video_width << "x" << title_geometry.video_height << "\""
                << (dvd_title_timings[i].aspect_ratio == "16:9" ? " widescreen=\"nopanscan\"" : "") << "/>\n";
            for (std::size_t ai = 0; ai < title_tracks[i].audio.size(); ++ai) {
                const auto& track = title_tracks[i].audio[ai];
                const auto stream_encoding = effective_audio_encoding_for_stream(authored_title, p.target, track.source_ordinal);
                xml << "<audio format=\"" << dvd_audio_format(stream_encoding.audio_codec) << "\" samplerate=\""
                    << (stream_encoding.audio_codec == AudioCodec::Lpcm ? stream_encoding.lpcm_sample_rate_hz / 1000 : 48)
                    << "khz\" lang=\"" << dvd_language(effective_audio_language(authored_title, track, ai))
                    << "\"/>\n";
            }
            for (const auto& sub : title_tracks[i].subtitles)
                xml << "<subpicture lang=\"" << dvd_language(effective_subtitle_language(authored_title, sub)) << "\"/>\n";
            xml << "<pgc><pre>"
                << dvd_title_stream_pre_commands(title_tracks[i].audio.size(), title_tracks[i].subtitles.size(),
                                                  authored_title.default_audio_stream, authored_title.default_subtitle_stream)
                << "</pre><vob file=\"" << xml_escape(dvd_title_vobs[i].string()) << "\" chapters=\""
                << dvd_chapters(resolved_title_chapters[i]) << "\"/>\n"
                << "<post>call vmgm menu " << dispatcher_menu << ";</post></pgc>\n</titles>\n</titleset>\n";
        }
        xml << "</dvdauthor>\n";

        reporter.emit(78.0, "DVD video encoding complete");

        const auto dvd_xml = work / "dvdauthor.xml";
        write_text(dvd_xml, xml.str());
        const auto dvd_root = work / "dvd-root";
        reporter.emit(82.0, "Building DVD-Video VIDEO_TS structure with dvdauthor");
        run(shq_s(tools_.dvdauthor) + " -o " + shq(dvd_root) + " -x " + shq(dvd_xml));
        fs::create_directories(dvd_root / "AUDIO_TS");
        if (!fs::is_directory(dvd_root / "VIDEO_TS")) throw std::runtime_error("dvdauthor completed without creating VIDEO_TS");
        for(std::size_t i=0;i<p.titles.size();++i)patch_dvd_title_uops(dvd_root,i,dvd_uop_mask(p.titles[i].prohibited_user_operations));

        reporter.emit(92.0, "Building DVD-Video UDF 1.02/ISO image with mkisofs -dvd-video");
        run(shq_s(tools_.mkisofs) + " -dvd-video -V " + shq_s(p.volume_label) + " -o " + shq(temporary_output_image) + " " + shq(dvd_root));
        if (!cache_file_usable(temporary_output_image)) throw std::runtime_error("mkisofs did not create a usable DVD image");
        if (!disc_capacity_is_unlimited(p.disc_capacity_bytes)) {
            const auto image_bytes = fs::file_size(temporary_output_image);
            if (image_bytes > p.disc_capacity_bytes)
                throw std::runtime_error("completed DVD image exceeds the selected size target despite preflight budgeting; image is " +
                    std::to_string(image_bytes) + " bytes but target is " + std::to_string(p.disc_capacity_bytes) + " bytes");
        }
        authoring_cancellation_point();
        publish_rendered_image(temporary_output_image, p.output_image);
        reporter.emit(100.0, "DVD-Video image complete");
        if (!p.keep_work_directory) fs::remove_all(work);
        return;
    }

    auto disc = work / "disc";
    fs::create_directories(disc / "BDMV/STREAM");
    fs::create_directories(disc / "BDMV/CLIPINF");
    fs::create_directories(disc / "BDMV/PLAYLIST");
    fs::create_directories(disc / "CERTIFICATE/BACKUP");

    hdmv::MenuObjectMap menu_objects;
    for (std::size_t i = 0; i < menus.size(); ++i)
        menu_objects.emplace(menus[i].menu.id, static_cast<std::uint16_t>(i));

    // Only action sequences that genuinely need a returning Play_PL run in
    // synthetic MovieObjects. Ordinary menu/title/stream-selection buttons are
    // compiled directly into the IG button command list so activation can
    // interrupt the currently playing menu clip immediately.
    std::vector<hdmv::MovieObject> navigation_objects;
    std::vector<std::vector<std::vector<hdmv::NavCommand>>> button_commands(menus.size());
    const auto menu_count_for_nav = static_cast<std::uint16_t>(menus.size());
    const auto hdmv_menu_object_count = menus.empty() ? 1U : menus.size();
    const auto base_navigation_object = hdmv_menu_object_count + p.titles.size();
    auto add_navigation_object = [&](std::span<const NavigationAction> actions, std::uint16_t return_menu) {const auto object_number = base_navigation_object + navigation_objects.size();if (object_number > 65534U) throw std::runtime_error("too many HDMV navigation objects");navigation_objects.push_back({false,false,false,hdmv::make_navigation_sequence_commands(actions,menu_objects,menu_count_for_nav,return_menu)});return static_cast<std::uint16_t>(object_number);};
    constexpr std::uint16_t NoNavigationObject=0xffffU;std::vector<std::uint16_t> title_menu_call_objects(p.titles.size(),NoNavigationObject);const bool custom_title_menu_calls=std::any_of(p.titles.begin(),p.titles.end(),[](const Title& t){return !t.menu_button_actions.empty();});std::uint16_t menu_call_default_object=NoNavigationObject;if(menus.empty()&&custom_title_menu_calls){const auto object_number=base_navigation_object+navigation_objects.size();if(object_number>65534U)throw std::runtime_error("too many HDMV navigation objects");menu_call_default_object=static_cast<std::uint16_t>(object_number);navigation_objects.push_back({false,false,false,{}});}for(std::size_t i=0;i<p.titles.size();++i)if(!p.titles[i].menu_button_actions.empty())title_menu_call_objects[i]=add_navigation_object(p.titles[i].menu_button_actions,0);
    for (std::size_t mi = 0; mi < menus.size(); ++mi) {
        auto& compiled = button_commands[mi];
        compiled.reserve(menus[mi].menu.buttons.size());
        for (const auto& button : menus[mi].menu.buttons) {
            const auto actions = button_action_sequence(button);
            if (auto immediate = hdmv::make_immediate_navigation_sequence_commands(actions, menu_objects,
                                                                                     static_cast<std::uint16_t>(mi),&menus[mi].menu)) {
                compiled.push_back(std::move(*immediate));
            } else {
                compiled.push_back({hdmv::jump_object(add_navigation_object(actions, static_cast<std::uint16_t>(mi)))});
            }
        }
    }
    // First Playback initializes independent per-title audio/subtitle state.
    // Keep this in its own MovieObject so any absolute GoTo commands inside a
    // user First Playback sequence retain their original command numbering.
    std::uint16_t startup_object = 0xffffU;
    if (!startup_actions.empty()) startup_object = add_navigation_object(startup_actions, 0);
    std::vector<hdmv::TitleStreamDefaults> stream_defaults;stream_defaults.reserve(p.titles.size());
    for(const auto& title:p.titles)stream_defaults.push_back({title.default_audio_stream,title.default_subtitle_stream});
    std::uint16_t first_play_object=0;
    if(!stream_defaults.empty()){
        const auto object_number=base_navigation_object+navigation_objects.size();if(object_number>65534U)throw std::runtime_error("too many HDMV navigation objects");
        auto commands=hdmv::make_title_stream_initialization_commands(stream_defaults);
        commands.push_back(hdmv::jump_object(startup_object==0xffffU?0U:startup_object));
        navigation_objects.push_back({false,false,false,std::move(commands)});first_play_object=static_cast<std::uint16_t>(object_number);
    }else if(startup_object!=0xffffU)first_play_object=startup_object;

    constexpr int VideoProgressStart = 2;
    constexpr int VideoProgressEnd = 82;
    const auto pass_count = [](const EncodingProfile& e) { return e.two_pass ? 2.0 : 1.0; };
    auto work_duration = [](double seconds) { return seconds > 0.0 ? seconds : 1.0; };
    double total_video_work = 0.0;
    for (const auto& rm : menus)
        total_video_work += work_duration(rm.menu.duration_seconds) * pass_count(encoding_for_target(rm.menu, p.target));
    for (std::size_t i = 0; i < p.titles.size(); ++i)
        total_video_work += work_duration(title_durations[i]) * pass_count(encoding_for_target(p.titles[i], p.target));
    double completed_video_work = 0.0;

    auto video_percent = [&](double work_done) {
        if (total_video_work <= 0.0) return static_cast<double>(VideoProgressStart);
        const double fraction = std::clamp(work_done / total_video_work, 0.0, 1.0);
        return static_cast<double>(VideoProgressStart) +
               fraction * static_cast<double>(VideoProgressEnd - VideoProgressStart);
    };
    auto progress_for_video = [&](double units, std::string label) -> VideoProgressCallback {
        const double before = completed_video_work;
        return [&, before, units, label = std::move(label)](double fraction, int pass, int passes) {
            const double pass_fraction = std::clamp(
                fraction * static_cast<double>(passes) - static_cast<double>(pass - 1), 0.0, 1.0);
            const double local = pass_fraction * 100.0;
            std::ostringstream msg;
            msg << label;
            if (passes > 1) msg << " — pass " << pass << "/" << passes;
            msg << " — " << std::fixed << std::setprecision(1) << local << "%";
            reporter.emit(video_percent(before + units * std::clamp(fraction, 0.0, 1.0)), msg.str());
        };
    };

    // Author one ordinary Blu-ray playlist/clip per menu node. MovieObject ids
    // and playlist numbers are deliberately identical for menus, so a button's
    // Jump_Object target is stable regardless of title count. Menu resolution
    // and aspect are project-wide, while Auto timing may follow each motion
    // background independently because every menu is its own playlist/clip.
    for (std::size_t mi = 0; mi < menus.size(); ++mi) {
        const auto& menu = menus[mi].menu;
        auto menu_encoding = encoding_for_target(menu, p.target);
        if (p.automatic_video_bitrate) menu_encoding.video_bitrate_kbps = capacity_plan.menu_video_kbps.at(mi);
        auto md = work / ("menu-" + std::to_string(mi));
        fs::create_directories(md);
        const bool motion_background = !menu.background_video.empty();
        VideoStreamInfo menu_timing_source;
        if (motion_background)
            menu_timing_source = probe_video_timing_metadata(tools_, menu.background_video, md / "menu-timing.txt");
        else {
            menu_timing_source.frame_rate = target_is_uhd(p.target) ? 24000.0/1001.0 : 24000.0/1001.0;
            menu_timing_source.field_order = "progressive";
            menu_timing_source.width = target_is_uhd(p.target) ? 3840 : 1920;
            menu_timing_source.height = target_is_uhd(p.target) ? 2160 : 1080;
            menu_timing_source.sample_aspect_ratio = "1:1";
        }
        const auto menu_timing = resolve_output_mode(p.target, menu_resolution_for_target(p,p.target), menu_aspect_ratio_for_target(p,p.target),
                                                     p.frame_rate, menu_timing_source, true, menu_encoding.video_codec);
        validate_codec_for_output_mode(menu_encoding,p.target,menu_timing,"menu '" + menu.name + "'");
        const auto geometry = target_geometry_for_timing(p.target,menu_timing);
        const auto video_menu = scale_menu_from_design(menu, geometry.video_width, geometry.video_height,
                                                       menu_timing.aspect_ratio);
        const auto graphics_menu = scale_menu_from_design(menu, geometry.graphics_width, geometry.graphics_height,
                                                          menu_timing.aspect_ratio);
        const bool has_image_buttons = std::any_of(graphics_menu.buttons.begin(), graphics_menu.buttons.end(),
                                                   [](const MenuButton& b){ return b.kind == MenuButtonKind::Image; });
        if (has_image_buttons)
            reporter.emit(video_percent(completed_video_work), "Preparing image buttons for " + menu.name);
        const auto authored_menu = prepare_menu_button_images(tools_, graphics_menu, md);
        const double units = work_duration(menu.duration_seconds) * pass_count(menu_encoding);
        const std::string label = "Encoding menu " + std::to_string(mi + 1) + " of " +
                                  std::to_string(menus.size()) + " (" + menu.name + ")";
        reporter.emit(video_percent(completed_video_work),
                      label + (menu_encoding.two_pass ? " — pass 1/2 — 0%" : " — 0%"));
        if (!menu.overlays.empty())
            reporter.emit(video_percent(completed_video_work), "Compositing labels for " + menu.name);
        const auto menu_background = motion_background ? video_menu.background_video : prepare_menu_background(tools_, video_menu, md);
        const auto menu_overlay = motion_background ? prepare_menu_overlay(tools_, video_menu, md) : fs::path{};
        reporter.emit(video_percent(completed_video_work), label + " — output mode " + menu_timing.description);

        // Prepare menu audio before video so VBR lossless audio can reserve its
        // measured peak transport budget before x264/x265 chooses a VBV ceiling.
        std::string audioMeta;
        int menu_audio_peak_kbps = 0;
        if (!menu.audio_source.empty()) {
            auto ad = md / "audio";
            fs::create_directories(ad);
            const int source_channels = audio_channels(tools_, menu.audio_source, md / "menu-audio-cache-channels", 0);
            const int output_channels = resolved_audio_output_channels(source_channels, menu_encoding, p.target, 0, "menu audio");
            const int fixed_peak = fixed_encoded_audio_peak_kbps(menu_encoding, p.target, output_channels);
            if (!encode_cache_root.empty()) {
                const auto& audio_fp = source_fingerprint(menu.audio_source);
                const auto audio_cache_file = encode_cache_root / "menu-audio" /
                    (menu_audio_cache_key(audio_fp, menu_encoding, p.target, menu.duration_seconds, false, menu.loop_media) + audio_extension(menu_encoding));
                int cached_peak = fixed_peak > 0 ? fixed_peak : cache_peak_bitrate_kbps(audio_cache_file);
                const bool audio_cache_hit = !p.force_reencode && !p.refresh_encode_cache &&
                    cache_file_usable(audio_cache_file) && cached_peak > 0;
                if (audio_cache_hit) {
                    const auto menu_audio_metadata = audio_cache_metadata(
                        "menu-audio", menu.audio_source, p.target, menu_encoding, output_channels, 0, cached_peak);
                    touch_cache_metadata(audio_cache_file, menu_audio_metadata);
                    reporter.emit(video_percent(completed_video_work), "Using cached encoded audio for menu " + menu.name);
                    audioMeta = audio_meta_for(audio_cache_file, menu_encoding, "eng");
                    menu_audio_peak_kbps = cached_peak;
                } else {
                    reporter.emit(video_percent(completed_video_work), "Encoding menu audio before video for " + menu.name);
                    const auto cache_tmp_dir = cache_encode_temp_dir(audio_cache_file);
                    CacheTempDirCleanup cache_tmp_cleanup{cache_tmp_dir};
                    const auto encoded = encode_audio(tools_, menu.audio_source, cache_tmp_dir, menu_encoding, p.target,
                                                      "eng", menu.loop_media, menu.duration_seconds);
                    menu_audio_peak_kbps = encoded.peak_bitrate_kbps;
                    const auto menu_audio_metadata = audio_cache_metadata(
                        "menu-audio", menu.audio_source, p.target, menu_encoding, output_channels, 0, menu_audio_peak_kbps);
                    finalize_encoded_cache_file(encoded.path, audio_cache_file, p.force_reencode || p.refresh_encode_cache, menu_audio_metadata);
                    audioMeta = audio_meta_for(audio_cache_file, menu_encoding, "eng");
                }
            } else {
                reporter.emit(video_percent(completed_video_work), "Encoding menu audio before video for " + menu.name);
                const auto encoded = encode_audio(tools_, menu.audio_source, ad, menu_encoding, p.target, "eng", menu.loop_media, menu.duration_seconds);
                audioMeta = encoded.meta;
                menu_audio_peak_kbps = encoded.peak_bitrate_kbps;
            }
        }
        const int menu_video_peak_limit_kbps = derived_video_peak_limit_kbps(p.target, menu_encoding.video_codec, menu_audio_peak_kbps, combined_transport_limit_kbps,
            menu_encoding.video_max_bitrate_kbps, limits_.allow_exceeding_format_limits);
        if (!limits_.allow_exceeding_format_limits)
            validate_video_peak_budget(menu_encoding, p.target, menu_audio_peak_kbps, menu_video_peak_limit_kbps, combined_transport_limit_kbps, "menu '" + menu.name + "'");
        reporter.emit(video_percent(completed_video_work), "Menu " + menu.name + " video peak budget: " +
                      std::to_string(menu_video_peak_limit_kbps) + " kb/s" +
                      (limits_.allow_exceeding_format_limits ? " (debug maxrate; audio-derived ceiling disabled)" :
                       " after reserving " + std::to_string(menu_audio_peak_kbps) + " kb/s for audio"));

        std::string videoMeta;
        fs::path menu_video_cache_file;
        if (!encode_cache_root.empty()) {
            menu_video_cache_file = encode_cache_root / "menu-video" /
                (menu_video_cache_key(menu_background, menu_overlay, menu_encoding, p, menu_timing, tools_,
                                      timing_source_interlaced(menu_timing_source), !motion_background,
                                      menu.duration_seconds, menu.background_color, menu.loop_media, menu_video_peak_limit_kbps) + video_extension(menu_encoding));
        }
        const auto menu_video_metadata = video_cache_metadata(
            "menu-video", motion_background ? menu.background_video : menu.background_image,
            p.target, menu_encoding, menu_timing, tools_, menu_video_peak_limit_kbps);
        if (!menu_video_cache_file.empty() && !p.force_reencode && !p.refresh_encode_cache && cache_file_usable(menu_video_cache_file)) {
            touch_cache_metadata(menu_video_cache_file, menu_video_metadata);
            reporter.emit(video_percent(completed_video_work + units), "Using cached encoded video for menu " + menu.name);
            videoMeta = video_meta_for(menu_video_cache_file, menu_encoding, menu_timing);
        } else if (!menu_video_cache_file.empty()) {
            const auto cache_tmp_dir = cache_encode_temp_dir(menu_video_cache_file);
            CacheTempDirCleanup cache_tmp_cleanup{cache_tmp_dir};
            const auto cache_tmp_base = cache_tmp_dir / "video";
            (void)encode_video(tools_, menu_background, cache_tmp_base,
                               menu_encoding, p, menu_timing,
                               timing_source_interlaced(menu_timing_source), !motion_background, menu.duration_seconds,
                               menu.duration_seconds, progress_for_video(units, label),
                               menu.background_color, motion_background && menu.loop_media, menu_overlay, motion_background && !menu.loop_media, menu_video_peak_limit_kbps);
            const fs::path cache_tmp_output = cache_tmp_base.string() + video_extension(menu_encoding);
            finalize_encoded_cache_file(cache_tmp_output, menu_video_cache_file, p.force_reencode || p.refresh_encode_cache, menu_video_metadata);
            videoMeta = video_meta_for(menu_video_cache_file, menu_encoding, menu_timing);
        } else {
            videoMeta = encode_video(tools_, menu_background, md / "video",
                                     menu_encoding, p, menu_timing,
                                     timing_source_interlaced(menu_timing_source), !motion_background, menu.duration_seconds,
                                     menu.duration_seconds, progress_for_video(units, label),
                                     menu.background_color, motion_background && menu.loop_media, menu_overlay, motion_background && !menu.loop_media, menu_video_peak_limit_kbps);
        }
        completed_video_work += units;

        auto segs = hdmv::make_menu_display_set(authored_menu, menu_objects, button_commands[mi], hdmv_frame_rate_code(menu_timing), hdmv::menu_selection_state_gpr(static_cast<std::uint16_t>(mi)));
        auto igs = hdmv::serialize_igs_file(segs);
        auto igsPath = md / "menu.igs";
        write_binary(igsPath, igs);

        auto metaPath = md / "menu.meta";
        constexpr unsigned kInteractiveMenuTransportFloorKbps = 4000U;
        // Apply the read-ahead floor to both standard Blu-ray and UHD menus.
        // UHD titles keep their independent transport model; only the tiny
        // interactive menu M2TS is padded with NULL TS packets.
        const unsigned menu_transport_floor = kInteractiveMenuTransportFloorKbps;
        // Keep Interactive Graphics resident independently of the main-path menu
        // media.  Looping menus use exactly one PlayItem; when it completes, the
        // menu MovieObject immediately replays that playlist.  Repeating the same
        // clip as hundreds of consecutive PlayItems makes each clip-local PTS
        // restart part of one long playlist timeline.  libbluray/VLC can then
        // accumulate a bad timestamp epoch after repeated UHD menu cycles.  A
        // real Play_PL restart gives the player a clean transport-clock epoch.
        // The preloaded IGS and per-menu PSR10/GPR handoff preserve the visible
        // menu and selected button across that restart as closely as HDMV allows.
        const bool has_menu_media = !menu.background_video.empty() || !menu.audio_source.empty();
        const bool final_menu_still = has_menu_media && !menu.loop_media;
        const unsigned menu_main_clip = static_cast<unsigned>(mi) * 2U;
        write_text(metaPath, meta_header(static_cast<unsigned>(mi), {}, p.target, static_cast<unsigned>(combined_transport_limit_kbps), menu_transport_floor,
                                         menu_main_clip, final_menu_still) + videoMeta + audioMeta +
                                 "S_HDMV/IGS, " + shq(igsPath) +
                                 ", lang=eng, fps=" + menu_timing.tsmuxer_fps + ", video-width=" + std::to_string(geometry.graphics_width) +
                                 ", video-height=" + std::to_string(geometry.graphics_height) + ", subClip\n");
        auto bd = md / "bd";
        reporter.emit(video_percent(completed_video_work), "Muxing HDMV menu " + menu.name);
        tsmux(tools_, metaPath, bd);
        const unsigned menu_igs_clip = menu_main_clip + 1U;
        copy_playlist_clips(bd, disc, static_cast<unsigned>(mi), {menu_main_clip, menu_igs_clip});
        backup_stream_metadata(disc, static_cast<unsigned>(mi), {menu_main_clip, menu_igs_clip});
    }

    const auto menu_count = static_cast<unsigned>(menus.size());
    for (std::size_t i = 0; i < p.titles.size(); ++i) {
        const auto& title = p.titles[i];
        auto title_encoding = encoding_for_target(title, p.target);
        if (p.automatic_video_bitrate) title_encoding.video_bitrate_kbps = capacity_plan.title_video_kbps.at(i);
        auto td = work / ("title-" + std::to_string(i + 1));
        fs::create_directories(td);
        const double units = work_duration(title_durations[i]) * pass_count(title_encoding);
        const bool force_reencode = p.force_reencode || title.force_reencode;
        std::ostringstream title_label;
        title_label << "title " << (i + 1) << " of " << p.titles.size();

        std::optional<MediaFingerprint> title_fp;
        if (!encode_cache_root.empty() || !compliance_cache_root.empty()) {
            reporter.emit(video_percent(completed_video_work),
                          "Fingerprinting " + title_label.str() +
                          ((!encode_cache_root.empty() && !compliance_cache_root.empty()) ? " for compliance and encoded-media caches" :
                           !encode_cache_root.empty() ? " for encoded-media cache" : " for compliance cache"));
            title_fp = source_fingerprint(title.source);
        }

        std::optional<TitleStreamAnalysis> analysis;
        bool compliance_cache_hit = false;
        fs::path compliance_cache_file;
        if (!force_reencode) {
            CacheMetadata compliance_metadata;
            compliance_metadata.category = "compliance-analysis";
            compliance_metadata.source_name = cache_source_name(title.source);
            compliance_metadata.target = cache_target_name(p.target);
            compliance_metadata.codec = "probe";
            compliance_metadata.settings = "headers + bounded first/middle/last samples";
            if (title_fp && !compliance_cache_root.empty()) {
                compliance_cache_file = compliance_cache_root / "compliance" /
                    (compliance_cache_key(*title_fp, title_tracks[i], title_durations[i], p.target) + ".txt");
                if (!p.refresh_compliance_cache) {
                    TitleStreamAnalysis cached;
                    if (load_compliance_cache(compliance_cache_file, title_tracks[i], cached)) {
                        analysis = std::move(cached);
                        compliance_cache_hit = true;
                        touch_cache_metadata(compliance_cache_file, compliance_metadata);
                        reporter.emit(video_percent(completed_video_work),
                                      "Using cached Blu-ray compliance analysis for " + title_label.str());
                    }
                }
            }
            if (!analysis) {
                reporter.emit(video_percent(completed_video_work), "Checking Blu-ray stream compliance (headers + bounded samples) for " + title_label.str());
                analysis = analyze_title_streams(tools_, title.source, td / "compliance", title_tracks[i], title_durations[i], p.target);
                if (!compliance_cache_file.empty()) publish_compliance_cache(compliance_cache_file, *analysis, compliance_metadata);
            }
        }

        VideoStreamInfo title_timing_source;
        if (analysis) title_timing_source = analysis->video;
        else title_timing_source = probe_video_timing_metadata(tools_, title.source, td / "timing.txt");
        const auto title_timing = resolve_output_mode(p.target, resolution_for_target(title,p.target),
                                                      aspect_ratio_for_target(title,p.target),
                                                      effective_title_frame_rate(p,title), title_timing_source, false, title_encoding.video_codec);
        validate_codec_for_output_mode(title_encoding,p.target,title_timing,"title " + std::to_string(i + 1));
        const auto title_geometry = target_geometry_for_timing(p.target, title_timing);
        const bool source_matches_override = source_matches_output_mode(title_timing_source,title_timing);
        const bool source_matches_gop = source_matches_requested_keyframe_interval(title_timing_source,title_encoding,title_timing);
        bool video_passthrough = !p.automatic_video_bitrate && analysis && analysis->video_decision.compliant && source_matches_override && source_matches_gop;
        std::vector<bool> audio_passthrough(title_tracks[i].audio.size(), false);
        if (analysis) {
            for (std::size_t ai = 0; ai < analysis->audio.size(); ++ai) {
                const auto* stream_settings = find_audio_stream_settings(title, analysis->audio[ai].source.source_ordinal);
                audio_passthrough[ai] = !p.automatic_video_bitrate && !force_reencode && !(stream_settings && (stream_settings->override_encoding || stream_settings->output_channels > 0)) &&
                                        analysis->audio[ai].decision.compliant;
            }
        }
        if (force_reencode) {
            reporter.emit(video_percent(completed_video_work), "Forced re-encode enabled for " + title_label.str());
        } else if (!video_passthrough) {
            if (analysis && analysis->video_decision.compliant && !source_matches_override) {
                reporter.emit(video_percent(completed_video_work),
                              "Video will be re-encoded for " + title_label.str() +
                              " to satisfy title video-mode selection: " + title_timing.description);
            } else if (analysis && analysis->video_decision.compliant && !source_matches_gop) {
                reporter.emit(video_percent(completed_video_work),
                              "Video will be re-encoded for " + title_label.str() +
                              " to satisfy the requested GOP/keyframe interval");
            } else {
                reporter.emit(video_percent(completed_video_work),
                              "NONCOMPLIANT: video for " + title_label.str() + ": " + analysis->video_decision.reason +
                              (compliance_cache_hit ? " [cached analysis]" : ""));
            }
        } else {
            reporter.emit(video_percent(completed_video_work), "Video can be remuxed without re-encoding for " + title_label.str() + ": " + analysis->video_decision.reason);
        }
        if (analysis) {
            for (std::size_t ai = 0; ai < analysis->audio.size(); ++ai) {
                const auto track_number = ai + 1U;
                const auto& decision = analysis->audio[ai].decision;
                if (decision.compliant) {
                    reporter.emit(video_percent(completed_video_work),
                        "Audio track " + std::to_string(track_number) +
                        " can be remuxed without re-encoding for " + title_label.str() + ": " + decision.reason);
                } else {
                    reporter.emit(video_percent(completed_video_work),
                        "NONCOMPLIANT: audio track " + std::to_string(track_number) + " for " +
                        title_label.str() + ": " + decision.reason +
                        (compliance_cache_hit ? " [cached analysis]" : ""));
                }
            }
        }

        // Prepare/remux every authored audio stream before video. VBR audio
        // (especially TrueHD) is measured after encoding so its actual peak,
        // not a fixed worst-case placeholder or average bitrate, determines the
        // video VBV ceiling. Every authored track is reserved because all audio
        // PIDs consume transport bandwidth even when only one is selected.
        std::string audioMeta;
        int aggregate_audio_peak_kbps = 0;
        for (std::size_t ai = 0; ai < title_tracks[i].audio.size(); ++ai) {
            const auto& source_track = title_tracks[i].audio[ai];
            const auto track_dir = td / ("audio-" + std::to_string(ai + 1U));
            const auto language = effective_audio_language(title, source_track, ai);
            const auto* stream_settings = find_audio_stream_settings(title, source_track.source_ordinal);
            const auto stream_encoding = effective_audio_encoding_for_stream(title, p.target, source_track.source_ordinal);
            validate_encoding_for_target(stream_encoding, p.target, title_label.str() + " audio track " + std::to_string(ai + 1U),
                                         limits_.allow_exceeding_format_limits);
            fs::create_directories(track_dir);

            if (audio_passthrough[ai]) {
                reporter.emit(video_percent(completed_video_work), "Remuxing compliant audio track " + std::to_string(ai + 1U) + " before video for " + title_label.str());
                audioMeta += remux_compliant_audio(tools_, title.source, track_dir,
                                                   analysis->audio[ai].decision, language,
                                                   source_track.source_ordinal);
                const fs::path remuxed = track_dir / (std::string("audio") + analysis->audio[ai].decision.elementary_extension);
                const int peak = probe_elementary_audio_peak_kbps(tools_, remuxed, track_dir / "peak-bitrate.txt");
                aggregate_audio_peak_kbps += peak;
                reporter.emit(video_percent(completed_video_work), "Measured audio track " + std::to_string(ai + 1U) +
                              " peak: " + std::to_string(peak) + " kb/s");
                continue;
            }

            fs::path audio_cache_file;
            if (title_fp && !encode_cache_root.empty()) {
                audio_cache_file = encode_cache_root / "audio" /
                                   (audio_cache_key(*title_fp, stream_encoding, source_track.source_ordinal,
                                                    stream_settings ? stream_settings->output_channels : 0) + audio_extension(stream_encoding));
            }
            const int source_channels_for_metadata = analysis && ai < analysis->audio.size() && analysis->audio[ai].info.channels > 0
                ? analysis->audio[ai].info.channels : audio_channels(tools_, title.source, track_dir, source_track.source_ordinal);
            const int output_channels_for_metadata = resolved_audio_output_channels(
                source_channels_for_metadata, stream_encoding, p.target,
                stream_settings ? stream_settings->output_channels : 0,
                "title audio track " + std::to_string(ai + 1U));
            const int fixed_peak = fixed_encoded_audio_peak_kbps(stream_encoding, p.target, output_channels_for_metadata);
            const int cached_peak = !audio_cache_file.empty() ?
                (fixed_peak > 0 ? fixed_peak : cache_peak_bitrate_kbps(audio_cache_file)) : 0;
            const bool audio_cache_hit = !audio_cache_file.empty() && !force_reencode && !p.refresh_encode_cache &&
                                         cache_file_usable(audio_cache_file) && cached_peak > 0;
            if (audio_cache_hit) {
                const auto title_audio_metadata = audio_cache_metadata(
                    "title-audio", title.source, p.target, stream_encoding, output_channels_for_metadata,
                    source_track.source_ordinal, cached_peak);
                touch_cache_metadata(audio_cache_file, title_audio_metadata);
                reporter.emit(video_percent(completed_video_work), "Using cached encoded audio track " + std::to_string(ai + 1U) + " before video for " + title_label.str());
                audioMeta += audio_meta_for(audio_cache_file, stream_encoding, language);
                aggregate_audio_peak_kbps += cached_peak;
            } else {
                reporter.emit(video_percent(completed_video_work), "Encoding audio track " + std::to_string(ai + 1U) + " before video for " + title_label.str());
                EncodedAudioResult encoded;
                if (!audio_cache_file.empty()) {
                    const auto cache_tmp_dir = cache_encode_temp_dir(audio_cache_file);
                    CacheTempDirCleanup cache_tmp_cleanup{cache_tmp_dir};
                    encoded = encode_audio(tools_, title.source, cache_tmp_dir, stream_encoding, p.target, language, false, 0,
                                           source_track.source_ordinal, stream_settings ? stream_settings->output_channels : 0);
                    const auto title_audio_metadata = audio_cache_metadata(
                        "title-audio", title.source, p.target, stream_encoding, output_channels_for_metadata,
                        source_track.source_ordinal, encoded.peak_bitrate_kbps);
                    finalize_encoded_cache_file(encoded.path, audio_cache_file, force_reencode || p.refresh_encode_cache, title_audio_metadata);
                    audioMeta += audio_meta_for(audio_cache_file, stream_encoding, language);
                } else {
                    encoded = encode_audio(tools_, title.source, track_dir, stream_encoding, p.target, language, false, 0,
                                           source_track.source_ordinal, stream_settings ? stream_settings->output_channels : 0);
                    audioMeta += encoded.meta;
                }
                aggregate_audio_peak_kbps += encoded.peak_bitrate_kbps;
                reporter.emit(video_percent(completed_video_work), "Measured encoded audio track " + std::to_string(ai + 1U) +
                              " peak: " + std::to_string(encoded.peak_bitrate_kbps) + " kb/s");
            }
        }

        const int title_video_peak_limit_kbps = derived_video_peak_limit_kbps(
            p.target, title_encoding.video_codec, aggregate_audio_peak_kbps, combined_transport_limit_kbps,
            title_encoding.video_max_bitrate_kbps, limits_.allow_exceeding_format_limits);
        reporter.emit(video_percent(completed_video_work), title_label.str() + " video peak budget: " +
                      std::to_string(title_video_peak_limit_kbps) + " kb/s" +
                      (limits_.allow_exceeding_format_limits ? " (debug maxrate; audio-derived ceiling disabled)" :
                       " after reserving " + std::to_string(aggregate_audio_peak_kbps) + " kb/s for authored audio"));

        if (video_passthrough && !limits_.allow_exceeding_format_limits) {
            const double source_video_peak_kbps = passthrough_video_rate_kbps(analysis->video);
            const int transport_remainder_kbps = combined_transport_limit_kbps - aggregate_audio_peak_kbps;
            if (source_video_peak_kbps > static_cast<double>(transport_remainder_kbps) + 0.5) {
                video_passthrough = false;
                reporter.emit(video_percent(completed_video_work),
                              "Compliant source video will be re-encoded for " + title_label.str() +
                              " because its declared/sample peak of " +
                              std::to_string(static_cast<int>(std::ceil(source_video_peak_kbps))) +
                              " kb/s does not fit beside the measured audio peak under the " +
                              std::to_string(combined_transport_limit_kbps) + " kb/s transport limit");
            }
        }

        if (!video_passthrough && !limits_.allow_exceeding_format_limits)
            validate_video_peak_budget(title_encoding, p.target, aggregate_audio_peak_kbps,
                                       title_video_peak_limit_kbps, combined_transport_limit_kbps, title_label.str());

        std::string videoMeta;
        if (video_passthrough) {
            reporter.emit(video_percent(completed_video_work), "Remuxing compliant video for " + title_label.str());
            videoMeta = remux_compliant_video(tools_, title.source, td / "passthrough-video", analysis->video, analysis->video_decision);
            reporter.emit(video_percent(completed_video_work + units), "Remuxed compliant video for " + title_label.str());
        } else {
            fs::path video_cache_file;
            if (title_fp && !encode_cache_root.empty()) {
                video_cache_file = encode_cache_root / "video" /
                                   (video_cache_key(*title_fp, title_encoding, p, title_timing, tools_, title_video_peak_limit_kbps) + video_extension(title_encoding));
            }
            const auto title_video_metadata = video_cache_metadata("title-video", title.source, p.target, title_encoding, title_timing, tools_, title_video_peak_limit_kbps);
            const bool video_cache_hit = !video_cache_file.empty() && !force_reencode && !p.refresh_encode_cache && cache_file_usable(video_cache_file);
            if (video_cache_hit) {
                touch_cache_metadata(video_cache_file, title_video_metadata);
                reporter.emit(video_percent(completed_video_work + units), "Using cached encoded video for " + title_label.str());
                videoMeta = video_meta_for(video_cache_file, title_encoding, title_timing);
            } else {
                std::ostringstream encode_label;
                encode_label << "Encoding " << title_label.str() << " — " << title_timing.description
                             << " — peak limit " << title_video_peak_limit_kbps << " kb/s";
                reporter.emit(video_percent(completed_video_work),
                              encode_label.str() + (title_encoding.two_pass ? " — pass 1/2 — 0%" : " — 0%"));
                if (!video_cache_file.empty()) {
                    const auto cache_tmp_dir = cache_encode_temp_dir(video_cache_file);
                    CacheTempDirCleanup cache_tmp_cleanup{cache_tmp_dir};
                    const auto cache_tmp_base = cache_tmp_dir / "video";
                    (void)encode_video(tools_, title.source, cache_tmp_base, title_encoding, p, title_timing,
                                       timing_source_interlaced(title_timing_source), false, 0,
                                       title_durations[i], progress_for_video(units, encode_label.str()),
                                       {0,0,0,255}, false, {}, false, title_video_peak_limit_kbps);
                    const fs::path cache_tmp_output = cache_tmp_base.string() + video_extension(title_encoding);
                    finalize_encoded_cache_file(cache_tmp_output, video_cache_file, force_reencode || p.refresh_encode_cache, title_video_metadata);
                    videoMeta = video_meta_for(video_cache_file, title_encoding, title_timing);
                } else {
                    videoMeta = encode_video(tools_, title.source, td / "video", title_encoding, p, title_timing,
                                             timing_source_interlaced(title_timing_source), false, 0,
                                             title_durations[i], progress_for_video(units, encode_label.str()),
                                             {0,0,0,255}, false, {}, false, title_video_peak_limit_kbps);
                }
            }
        }
        completed_video_work += units;

        std::string subtitleMeta;
        const int subtitle_width = title_geometry.graphics_width;
        const int subtitle_height = title_geometry.graphics_height;
        const double subtitle_fps = video_passthrough ? analysis->video.frame_rate : title_timing.frame_rate_value;
        for (std::size_t si = 0; si < title_tracks[i].subtitles.size(); ++si) {
            reporter.emit(video_percent(completed_video_work), "Preparing subtitle track " + std::to_string(si + 1U) + " for " + title_label.str());
            auto subtitle_track = title_tracks[i].subtitles[si];
            subtitle_track.language = effective_subtitle_language(title, subtitle_track);
            subtitleMeta += prepare_subtitle(tools_, title.source,
                                             td / ("subtitle-" + std::to_string(si + 1U)),
                                             subtitle_track,
                                             subtitle_width, subtitle_height, subtitle_fps);
        }
        const auto playlist = menu_count + static_cast<unsigned>(i);
        const auto title_clip = 2U * menu_count + static_cast<unsigned>(i);
        std::ostringstream meta;
        meta << meta_header(playlist, resolved_title_chapters[i], p.target, static_cast<unsigned>(combined_transport_limit_kbps), 0U, title_clip) << videoMeta;
        meta << audioMeta << subtitleMeta;
        auto mf = td / "title.meta";
        write_text(mf, meta.str());
        auto bd = td / "bd";
        reporter.emit(video_percent(completed_video_work), "Muxing " + title_label.str());
        tsmux(tools_, mf, bd);
        copy_playlist_clips(bd, disc, playlist, {title_clip});
        {std::ostringstream playlist_name;playlist_name<<std::setw(5)<<std::setfill('0')<<playlist<<".mpls";hdmv::add_playlist_user_operation_mask(disc/"BDMV/PLAYLIST"/playlist_name.str(),bluray_uop_mask(title.prohibited_user_operations));}
        backup_stream_metadata(disc, playlist, {title_clip});
    }

    reporter.emit(VideoProgressEnd, "Writing HDMV navigation");
    std::vector<std::uint16_t> title_chapter_counts;
    title_chapter_counts.reserve(resolved_title_chapters.size());
    for (const auto& chapters : resolved_title_chapters)
        title_chapter_counts.push_back(static_cast<std::uint16_t>(chapters.size() + 1U));
    hdmv::write_control_files(disc / "BDMV", static_cast<std::uint16_t>(p.titles.size()),
                              static_cast<std::uint16_t>(menus.size()), target_is_uhd(p.target),
                              navigation_objects, first_play_object, title_chapter_counts,
                              title_menu_call_objects,menu_call_default_object,true);
    std::string why;
    if (!validate_java_free_bdmv(disc, &why)) throw std::runtime_error("Java-free validation failed: " + why);

    reporter.emit(88, "Building UDF 2.50 image");
    udf25::WriterOptions o;
    o.output = temporary_output_image;
    o.volume_label = p.volume_label;
    o.application_id = "*BDMVAuthor";
    o.implementation_id = "*BDMVAuthor";
    o.automatic_bdmv_correction = true;
    o.automatic_hddvd_correction = false;
    o.verify_after_write = true;
    o.cancel_requested = [] { return authoring_cancel_requested(); };
    o.grafts.push_back({disc / "BDMV", "BDMV"});
    o.grafts.push_back({disc / "CERTIFICATE", "CERTIFICATE"});
    udf25::ImageWriter w(std::move(o));
    try {
        w.build();
    } catch (...) {
        if (authoring_cancel_requested()) throw AuthorCancelled();
        throw;
    }
    if (!disc_capacity_is_unlimited(p.disc_capacity_bytes)) {
        const auto image_bytes = fs::file_size(temporary_output_image);
        if (image_bytes > p.disc_capacity_bytes)
            throw std::runtime_error("completed Blu-ray image exceeds the selected size target despite preflight budgeting; image is " +
                std::to_string(image_bytes) + " bytes but target is " + std::to_string(p.disc_capacity_bytes) + " bytes");
    }
    authoring_cancellation_point();
    publish_rendered_image(temporary_output_image, p.output_image);
    reporter.emit(100, "Blu-ray image complete");
    if (!p.keep_work_directory) fs::remove_all(work);
}

} // namespace bdmvauthor
