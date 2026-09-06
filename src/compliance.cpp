// SPDX-License-Identifier: Apache-2.0
#include "bdmvauthor/compliance.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <sstream>

namespace bdmvauthor {
namespace {

constexpr double kPrimaryVideoMaxBitrate = 40'000'000.0;
constexpr double kUhdPrimaryVideoMaxBitrate = 100'000'000.0;
constexpr double kUhdHevcMaxCpbBits = 100'000'000.0;
constexpr double kAvcMaxCpbBits = 30'000'000.0;
constexpr double kMpeg2MaxVbvBits = 9'781'248.0;

bool near(double a, double b, double epsilon = 0.03) {
    return std::isfinite(a) && std::abs(a - b) <= epsilon;
}
bool progressive(const VideoStreamInfo& i) {
    if (i.field_order == "progressive") return true;
    // H.264 frame_mbs_only_flag=1 proves frame-coded progressive video even
    // when a container/ffprobe reports field_order=unknown.
    return i.codec_name == "h264" && i.h264_frame_mbs_only_flag == 1;
}
bool interlaced(const VideoStreamInfo& i) {
    return i.field_order == "tt" || i.field_order == "bb" || i.field_order == "tb" || i.field_order == "bt";
}
bool fake_interlaced_avc(const VideoStreamInfo& i) {
    // x264 --fake-interlaced clears frame_mbs_only_flag so the sequence is
    // signaled as PAFF-capable, but leaves mb_adaptive_frame_field_flag clear
    // because every coded picture remains progressive.  This pair of SPS bits
    // distinguishes the Blu-ray 25p/29.97p technique from true MBAFF encoding.
    return i.codec_name == "h264" && i.h264_frame_mbs_only_flag == 0 &&
           i.h264_mb_adaptive_frame_field_flag == 0;
}
bool is_hd_mode(const VideoStreamInfo& i, bool allow_1440) {
    if (i.width == 1920 && i.height == 1080)
        return (progressive(i) && (near(i.frame_rate, 24000.0/1001.0) || near(i.frame_rate, 24.0))) ||
               (interlaced(i) && (near(i.frame_rate, 30000.0/1001.0) || near(i.frame_rate, 25.0))) ||
               (fake_interlaced_avc(i) && (near(i.frame_rate, 30000.0/1001.0) || near(i.frame_rate, 25.0)));
    if (allow_1440 && i.width == 1440 && i.height == 1080)
        return (progressive(i) && (near(i.frame_rate, 24000.0/1001.0) || near(i.frame_rate, 24.0))) ||
               (interlaced(i) && (near(i.frame_rate, 30000.0/1001.0) || near(i.frame_rate, 25.0))) ||
               (fake_interlaced_avc(i) && (near(i.frame_rate, 30000.0/1001.0) || near(i.frame_rate, 25.0)));
    if (i.width == 1280 && i.height == 720 && progressive(i))
        return near(i.frame_rate, 24000.0/1001.0) || near(i.frame_rate, 24.0) ||
               near(i.frame_rate, 60000.0/1001.0) || near(i.frame_rate, 50.0);
    return false;
}
bool is_sd_mode(const VideoStreamInfo& i) {
    return (i.width == 720 && i.height == 480 && interlaced(i) && near(i.frame_rate, 30000.0/1001.0)) ||
           (i.width == 720 && i.height == 576 && interlaced(i) && near(i.frame_rate, 25.0));
}
std::string mode_text(const VideoStreamInfo& i) {
    std::ostringstream s; s << i.width << 'x' << i.height << '@' << i.frame_rate << (progressive(i) ? "p" : "i"); return s.str();
}
BluRayStreamDecision reject(std::string why) { return {false, std::move(why), {}, {}}; }
BluRayStreamDecision accept(std::string why, std::string codec, std::string ext) {
    return {true, std::move(why), std::move(codec), std::move(ext)};
}
int h264_max_dpb_mbs(int level) {
    switch (level) {
        case 30: return 8100;
        case 31: return 18000;
        case 32: return 20480;
        case 40: return 32768;
        case 41: return 32768;
        default: return 0;
    }
}
int h264_max_frame_mbs(int level) {
    switch (level) {
        case 30: return 1620;
        case 31: return 3600;
        case 32: return 5120;
        case 40: return 8192;
        case 41: return 8192;
        default: return 0;
    }
}
int h264_max_mbps(int level) {
    switch (level) {
        case 30: return 40500;
        case 31: return 108000;
        case 32: return 216000;
        case 40: return 245760;
        case 41: return 245760;
        default: return 0;
    }
}
double h264_level_max_bitrate(int level, int profile_idc) {
    double base = 0.0;
    switch (level) {
        case 30: base = 10'000'000.0; break;
        case 31: base = 14'000'000.0; break;
        case 32: base = 20'000'000.0; break;
        case 40: base = 20'000'000.0; break;
        case 41: base = 50'000'000.0; break;
        default: return 0.0;
    }
    return profile_idc == 100 ? base * 1.25 : base;
}
double h264_level_max_cpb(int level, int profile_idc) {
    double base = 0.0;
    switch (level) {
        case 30: base = 10'000'000.0; break;
        case 31: base = 14'000'000.0; break;
        case 32: base = 20'000'000.0; break;
        case 40: base = 25'000'000.0; break;
        case 41: base = 62'500'000.0; break;
        default: return 0.0;
    }
    return profile_idc == 100 ? base * 1.25 : base;
}
bool sar_ok(const VideoStreamInfo& i) {
    if (i.sample_aspect_ratio.empty() || i.sample_aspect_ratio == "N/A") return false;
    if ((i.width == 3840 && i.height == 2160) || (i.width == 1920 && i.height == 1080) ||
        (i.width == 1280 && i.height == 720))
        return i.sample_aspect_ratio == "1:1";
    if (i.width == 1440 && i.height == 1080) return i.sample_aspect_ratio == "4:3";
    if (i.width == 720 && i.height == 480)
        return i.sample_aspect_ratio == "8:9" || i.sample_aspect_ratio == "32:27";
    if (i.width == 720 && i.height == 576)
        return i.sample_aspect_ratio == "16:15" || i.sample_aspect_ratio == "64:45";
    return false;
}
bool audio_rate_ok(int rate) { return rate == 48000 || rate == 96000 || rate == 192000; }
std::string mbps(double bps) {
    std::ostringstream s;
    s.setf(std::ios::fixed);
    s.precision(3);
    s << (bps / 1'000'000.0);
    return s.str();
}

bool color_unspecified(const std::string& value) {
    return value.empty() || value == "unknown" || value == "unspecified" || value == "reserved" || value == "N/A";
}
bool limited_or_unspecified(const VideoStreamInfo& i) {
    return color_unspecified(i.color_range) || i.color_range == "tv" || i.color_range == "mpeg";
}
bool color_bt709_or_unspecified(const VideoStreamInfo& i) {
    const auto bt709_or_unknown = [](const std::string& value) {
        return color_unspecified(value) || value == "bt709";
    };
    return bt709_or_unknown(i.color_primaries) && bt709_or_unknown(i.color_transfer) &&
           bt709_or_unknown(i.color_space);
}
bool color_bt709_needs_tagging(const VideoStreamInfo& i) {
    return color_bt709_or_unspecified(i) &&
           (color_unspecified(i.color_primaries) || color_unspecified(i.color_transfer) ||
            color_unspecified(i.color_space));
}
bool color_bt2020_sdr(const VideoStreamInfo& i) {
    return i.color_primaries == "bt2020" &&
           (i.color_transfer == "bt2020-10" || i.color_transfer == "bt2020_10") &&
           (i.color_space == "bt2020nc" || i.color_space == "bt2020_ncl");
}
bool color_bt2020_pq(const VideoStreamInfo& i) {
    return i.color_primaries == "bt2020" && i.color_transfer == "smpte2084" &&
           (i.color_space == "bt2020nc" || i.color_space == "bt2020_ncl");
}
bool any_bt2020_tag(const VideoStreamInfo& i) {
    return i.color_primaries == "bt2020" || i.color_transfer == "bt2020-10" ||
           i.color_transfer == "bt2020_10" || i.color_transfer == "smpte2084" ||
           i.color_space == "bt2020nc" || i.color_space == "bt2020_ncl" || i.color_space == "bt2020c";
}

} // namespace

namespace detail {
namespace {
std::optional<unsigned> dts_read_be_bits(const std::vector<std::uint8_t>& bytes,
                                         std::size_t& bit_pos, unsigned count) {
    if (count == 0 || count > 24 || bit_pos + count > bytes.size() * 8ULL) return std::nullopt;
    unsigned value = 0;
    for (unsigned i = 0; i < count; ++i) {
        const auto byte_index = bit_pos / 8;
        const auto bit_index = 7U - static_cast<unsigned>(bit_pos % 8);
        value = (value << 1U) | ((bytes[byte_index] >> bit_index) & 1U);
        ++bit_pos;
    }
    return value;
}
} // namespace

std::optional<double> dts_core_actual_bitrate_from_bytes(const std::vector<std::uint8_t>& input) {
    if (input.size() < 12) return std::nullopt;
    std::size_t sync = std::string::npos;
    bool little_endian = false;
    for (std::size_t i = 0; i + 4 <= input.size(); ++i) {
        if (input[i] == 0x7f && input[i + 1] == 0xfe && input[i + 2] == 0x80 && input[i + 3] == 0x01) {
            sync = i; break;
        }
        if (input[i] == 0xfe && input[i + 1] == 0x7f && input[i + 2] == 0x01 && input[i + 3] == 0x80) {
            sync = i; little_endian = true; break;
        }
    }
    if (sync == std::string::npos) return std::nullopt;
    std::vector<std::uint8_t> bytes(input.begin() + static_cast<std::ptrdiff_t>(sync), input.end());
    if (little_endian) {
        for (std::size_t i = 0; i + 1 < bytes.size(); i += 2) std::swap(bytes[i], bytes[i + 1]);
    }
    if (bytes.size() < 12 || bytes[0] != 0x7f || bytes[1] != 0xfe || bytes[2] != 0x80 || bytes[3] != 0x01)
        return std::nullopt;
    std::size_t bit = 32;
    if (!dts_read_be_bits(bytes, bit, 1) || !dts_read_be_bits(bytes, bit, 5) || !dts_read_be_bits(bytes, bit, 1)) return std::nullopt;
    const auto nblks = dts_read_be_bits(bytes, bit, 7);
    const auto fsize = dts_read_be_bits(bytes, bit, 14);
    if (!nblks || !fsize || !dts_read_be_bits(bytes, bit, 6)) return std::nullopt;
    const auto sfreq = dts_read_be_bits(bytes, bit, 4);
    if (!sfreq || !dts_read_be_bits(bytes, bit, 5)) return std::nullopt;
    static constexpr std::array<int,16> sample_rates = {
        0,8000,16000,32000,0,0,11025,22050,44100,0,0,12000,24000,48000,96000,192000
    };
    const int sample_rate = sample_rates[*sfreq];
    const unsigned pcm_samples = (*nblks + 1U) * 32U;
    const unsigned frame_bytes = *fsize + 1U;
    if (sample_rate <= 0 || pcm_samples == 0 || frame_bytes == 0) return std::nullopt;
    return static_cast<double>(frame_bytes) * 8.0 * static_cast<double>(sample_rate) /
           static_cast<double>(pcm_samples);
}
} // namespace detail

BluRayStreamDecision evaluate_blu_ray_video_headers(const VideoStreamInfo& i, DiscTarget target) {
    if (target == DiscTarget::DvdVideo480p)
        return reject("DVD-Video uses its dedicated MPEG-2 encode path rather than Blu-ray passthrough compliance");
    if (target == DiscTarget::UltraHdBluRay2160) {
        if (i.codec_name == "hevc") {
            const bool uhd_size = (i.width == 3840 && i.height == 2160) || (i.width == 1920 && i.height == 1080);
            if (!uhd_size || !progressive(i) ||
                !(near(i.frame_rate, 24000.0/1001.0) || near(i.frame_rate, 24.0) ||
                  near(i.frame_rate, 25.0) || near(i.frame_rate, 50.0) ||
                  near(i.frame_rate, 60000.0/1001.0) || near(i.frame_rate, 60.0)))
                return reject("video mode " + mode_text(i) + " is not a supported Ultra HD Blu-ray HEVC progressive mode");
            if (!sar_ok(i))
                return reject("Ultra HD Blu-ray video sample aspect ratio must be 1:1");
            if (!i.pixel_format.empty() && i.pixel_format.find("yuv420p10") != 0)
                return reject("Ultra HD Blu-ray HEVC primary video must use 10-bit 4:2:0 sampling");
            if (i.hevc_profile_idc != 2)
                return reject("Ultra HD Blu-ray HEVC must use Main 10 profile");
            if (i.hevc_tier_flag != 1)
                return reject("Ultra HD Blu-ray HEVC must use High Tier");
            if (i.hevc_level_idc != 150 && i.hevc_level_idc != 153)
                return reject("Ultra HD Blu-ray HEVC level must be 5.0 or 5.1");
            if (i.hevc_chroma_format_idc != 1 || i.hevc_bit_depth_luma_minus8 != 2 || i.hevc_bit_depth_chroma_minus8 != 2)
                return reject("Ultra HD Blu-ray HEVC SPS is not 10-bit 4:2:0");
            if (!limited_or_unspecified(i))
                return reject("Ultra HD Blu-ray primary video must use limited-range video levels");
            if (!color_bt709_or_unspecified(i) && !color_bt2020_sdr(i) && !color_bt2020_pq(i))
                return reject("Ultra HD Blu-ray color description must be BT.709 SDR, BT.2020 SDR, or BT.2020/ST 2084 HDR");
            if (!i.hevc_hrd_present)
                return reject("HEVC HRD parameters are absent, so Ultra HD Blu-ray decoder-buffer compliance cannot be verified");
            if (i.hevc_hrd_max_bitrate_bps <= 0.0 || i.hevc_hrd_max_bitrate_bps > kUhdPrimaryVideoMaxBitrate * 1.001)
                return reject("HEVC HRD bitrate " + mbps(i.hevc_hrd_max_bitrate_bps) + " Mb/s exceeds the Ultra HD Blu-ray 100 Mb/s primary-video limit");
            if (i.hevc_hrd_max_cpb_bits <= 0.0 || i.hevc_hrd_max_cpb_bits > kUhdHevcMaxCpbBits * 1.001)
                return reject("HEVC CPB/VBV buffer exceeds the Ultra HD Blu-ray decoder-buffer limit");
            return accept(color_bt709_needs_tagging(i)
                              ? "HEVC Main10 High-Tier metadata/SPS/HRD checks pass; missing BT.709 color tags are assumed and will be completed during remux; sampled random-access verification is still required"
                              : "HEVC Main10 High-Tier metadata/SPS/HRD/color checks pass; sampled random-access verification is still required",
                          "V_MPEGH/ISO/HEVC", ".265");
        }
        if (i.codec_name != "h264")
            return reject("Ultra HD Blu-ray primary video must use HEVC/H.265 or 1920x1080 AVC/H.264");
        if (i.width != 1920 || i.height != 1080 || !progressive(i) ||
            !(near(i.frame_rate, 24000.0/1001.0) || near(i.frame_rate, 24.0)))
            return reject("Ultra HD Blu-ray AVC/H.264 is limited to 1920x1080 at 23.976p or 24p");
        if (!sar_ok(i)) return reject("Ultra HD Blu-ray AVC/H.264 sample aspect ratio must be 1:1");
        if (!i.pixel_format.empty() && i.pixel_format != "yuv420p")
            return reject("Ultra HD Blu-ray AVC/H.264 must use 8-bit 4:2:0 sampling");
        if (!color_bt709_or_unspecified(i) || any_bt2020_tag(i))
            return reject("Ultra HD Blu-ray AVC/H.264 primary video is SDR BT.709 only");
        if (!limited_or_unspecified(i))
            return reject("Ultra HD Blu-ray AVC/H.264 must use limited-range video levels");
        // Continue through the shared Blu-ray H.264 profile/level/HRD checks.
    }
    if (i.codec_name != "h264" && i.codec_name != "mpeg2video" && i.codec_name != "vc1")
        return reject("video codec " + (i.codec_name.empty() ? std::string("is unknown") : i.codec_name) + " is not a BD-ROM primary-video codec");
    const bool allow_1440 = true;
    if (!is_hd_mode(i, allow_1440) && !is_sd_mode(i))
        return reject("video mode " + mode_text(i) + " is not a supported BD-ROM video mode");
    if (!sar_ok(i))
        return reject("video sample aspect ratio is missing or does not match a Blu-ray aspect-ratio mode");
    if (!i.pixel_format.empty() && i.pixel_format != "yuv420p")
        return reject("video uses " + i.pixel_format + "; Blu-ray primary video requires 8-bit 4:2:0 sampling");
    if (any_bt2020_tag(i))
        return reject("BT.2020/HDR source requires color conversion to BT.709 for standard Blu-ray output");
    if ((is_hd_mode(i, allow_1440)) && !color_bt709_or_unspecified(i))
        return reject("HD Blu-ray source has a non-BT.709 color description and requires conversion to BT.709");
    if ((is_hd_mode(i, allow_1440)) && !limited_or_unspecified(i))
        return reject("HD Blu-ray primary video must use limited-range video levels");
    if (i.codec_name == "vc1" && color_bt709_needs_tagging(i))
        return reject("VC-1 with missing BT.709 color tags must be re-encoded because the elementary stream cannot be safely color-tagged");

    if (i.codec_name == "h264") {
        if (i.h264_profile_idc != 77 && i.h264_profile_idc != 100)
            return reject("H.264 profile is not Blu-ray Main or High profile");
        if (i.h264_profile_idc == 100 && i.h264_level_idc != 40 && i.h264_level_idc != 41)
            return reject("H.264 High profile must use Blu-ray level 4.0 or 4.1");
        if (i.h264_profile_idc == 77 &&
            i.h264_level_idc != 30 && i.h264_level_idc != 31 && i.h264_level_idc != 32 &&
            i.h264_level_idc != 40 && i.h264_level_idc != 41)
            return reject("H.264 Main profile level is outside the Blu-ray set");
        // In Main Profile the chroma_format_idc and bit-depth syntax elements
        // are not present in the SPS; 4:2:0 8-bit is implicit. Treat missing
        // trace_headers fields as those mandated Main-profile defaults rather
        // than rejecting a valid Main-profile Blu-ray stream as "unknown".
        const int chroma_format_idc = (i.h264_profile_idc == 77 && i.h264_chroma_format_idc < 0)
            ? 1 : i.h264_chroma_format_idc;
        const int bit_depth_luma_minus8 = (i.h264_profile_idc == 77 && i.h264_bit_depth_luma_minus8 < 0)
            ? 0 : i.h264_bit_depth_luma_minus8;
        const int bit_depth_chroma_minus8 = (i.h264_profile_idc == 77 && i.h264_bit_depth_chroma_minus8 < 0)
            ? 0 : i.h264_bit_depth_chroma_minus8;
        if (chroma_format_idc != 1 || bit_depth_luma_minus8 != 0 || bit_depth_chroma_minus8 != 0)
            return reject("H.264 SPS is not 8-bit 4:2:0");
        const int max_dpb = h264_max_dpb_mbs(i.h264_level_idc);
        const int max_fs = h264_max_frame_mbs(i.h264_level_idc);
        const int max_mbps = h264_max_mbps(i.h264_level_idc);
        const int frame_mbs = ((i.width + 15) / 16) * ((i.height + 15) / 16);
        if (max_dpb <= 0 || max_fs <= 0 || max_mbps <= 0 || frame_mbs <= 0 || i.h264_max_num_ref_frames < 0)
            return reject("H.264 decoded-picture-buffer/reference-frame limits could not be verified");
        if (frame_mbs > max_fs || static_cast<double>(frame_mbs) * i.frame_rate > static_cast<double>(max_mbps) * 1.001)
            return reject("H.264 frame size or macroblock rate exceeds the declared level");
        const int max_refs = std::max(1, std::min(16, max_dpb / frame_mbs));
        if (i.h264_max_num_ref_frames > std::min(6, max_refs))
            return reject("H.264 reference-frame count exceeds the level/DPB limit for this resolution");
        if (!i.h264_hrd_present)
            return reject("H.264 HRD/VBV parameters are absent, so Blu-ray decoder-buffer compliance cannot be verified");
        if (i.h264_buffering_period_sei_present) {
            if (i.h264_initial_cpb_removal_delay_90k <= 0.0)
                return reject("H.264 buffering-period SEI has no usable initial CPB removal delay");
            const double hrd_initial_delay_limit = i.h264_hrd_max_bitrate_bps > 0.0
                ? 90000.0 * i.h264_hrd_max_cpb_bits / i.h264_hrd_max_bitrate_bps : 0.0;
            if (hrd_initial_delay_limit <= 0.0 ||
                i.h264_initial_cpb_removal_delay_90k > hrd_initial_delay_limit * 1.01)
                return reject("H.264 initial CPB removal delay is inconsistent with its HRD CPB/bitrate and can cause excessive title-start buffering");
        }
        const double level_bitrate = h264_level_max_bitrate(i.h264_level_idc, i.h264_profile_idc);
        const double level_cpb = h264_level_max_cpb(i.h264_level_idc, i.h264_profile_idc);
        if (i.h264_hrd_max_bitrate_bps <= 0.0 || level_bitrate <= 0.0 ||
            i.h264_hrd_max_bitrate_bps > std::min(kPrimaryVideoMaxBitrate, level_bitrate) * 1.001)
            return reject("H.264 HRD bitrate " + mbps(i.h264_hrd_max_bitrate_bps) + " Mb/s exceeds the Blu-ray or declared-level limit");
        if (i.h264_hrd_max_cpb_bits <= 0.0 || level_cpb <= 0.0 ||
            i.h264_hrd_max_cpb_bits > std::min(kAvcMaxCpbBits, level_cpb) * 1.001)
            return reject("H.264 CPB/VBV buffer exceeds the Blu-ray or declared-level limit");
        return accept(target == DiscTarget::UltraHdBluRay2160
                          ? "AVC/H.264 1920x1080 v3 metadata/SPS/HRD checks pass; sampled packet/GOP verification is still required"
                          : "H.264 metadata/SPS/HRD checks pass; sampled packet/GOP verification is still required",
                      "V_MPEG4/ISO/AVC", ".264");
    }

    if (i.codec_name == "mpeg2video") {
        const bool hd = is_hd_mode(i, true);
        if ((hd && i.mpeg2_profile_and_level_indication != 0x44) ||
            (!hd && i.mpeg2_profile_and_level_indication != 0x44 && i.mpeg2_profile_and_level_indication != 0x48))
            return reject("MPEG-2 profile/level is not Blu-ray Main Profile @ High/Main Level");
        if (i.mpeg2_header_bitrate_bps <= 0.0 || i.mpeg2_header_bitrate_bps > kPrimaryVideoMaxBitrate * 1.001)
            return reject("MPEG-2 sequence-header bitrate exceeds the 40 Mb/s Blu-ray limit");
        if (i.mpeg2_vbv_bits <= 0.0 || i.mpeg2_vbv_bits > kMpeg2MaxVbvBits)
            return reject("MPEG-2 VBV buffer exceeds the Main Profile @ High Level decoder-buffer limit");
        return accept("MPEG-2 metadata/sequence-header/VBV checks pass; sampled packet/GOP verification is still required",
                      "V_MPEG-2", ".m2v");
    }

    if (i.profile != "Advanced") return reject("VC-1 must use Advanced Profile for Blu-ray");
    if (i.level != 2 && i.level != 3) return reject("VC-1 must use Advanced Profile level 2 or 3 for Blu-ray");
    return accept("VC-1 metadata/profile/level checks pass; sampled packet/GOP verification is still required",
                  "V_MS/VFW/WVC1", ".vc1");
}

BluRayStreamDecision evaluate_blu_ray_video(const VideoStreamInfo& i, DiscTarget target) {
    const auto headers = evaluate_blu_ray_video_headers(i, target);
    if (!headers.compliant) return headers;
    if (i.sampled_average_bitrate_bps <= 0.0)
        return reject("video sampled elementary packets could not be verified");
    // H.264 and MPEG-2 already carry authoritative decoder-model bitrate
    // constraints in HRD/VBV or sequence-header syntax.  Access-unit sizes
    // grouped by DTS/PTS are decoder removal timing, not CPB/PES arrival
    // timing, so a short timestamp burst is not a valid second 40 Mb/s veto.
    // VC-1 lacks an equivalent check in this probe, so retain a conservative
    // sampled-average sanity check for that codec only.
    if (i.codec_name == "vc1" && i.sampled_average_bitrate_bps > kPrimaryVideoMaxBitrate * 1.01)
        return reject("VC-1 sampled average elementary bitrate " + mbps(i.sampled_average_bitrate_bps) + " Mb/s exceeds the 40 Mb/s Blu-ray primary-video limit");
    if (!i.first_packet_is_keyframe)
        return reject("video does not begin on a random-access/key frame");

    if (i.codec_name == "hevc") {
        if (i.max_keyframe_interval_seconds <= 0.0 || i.max_keyframe_interval_seconds > 1.05)
            return reject("HEVC random-access interval exceeds one second in a sampled window");
        return accept("HEVC Main10 High-Tier elementary stream passes Ultra HD Blu-ray header checks plus sampled first/middle/last random-access checks",
                      "V_MPEGH/ISO/HEVC", ".265");
    }

    if (i.codec_name == "h264") {
        if (i.max_consecutive_b_frames < 0)
            return reject("H.264 sampled B-frame structure could not be verified");
        if (i.max_consecutive_b_frames > 3)
            return reject("H.264 uses more than three consecutive B-frames in a sampled window, outside Blu-ray-compatible encoder limits");
        if (i.max_keyframe_interval_seconds <= 0.0 || i.max_keyframe_interval_seconds > 1.05)
            return reject("H.264 random-access interval exceeds one second in a sampled window");
        return accept(target == DiscTarget::UltraHdBluRay2160
                          ? "AVC/H.264 1920x1080 stream passes Ultra HD Blu-ray v3 header plus sampled packet/GOP checks"
                          : "H.264 elementary stream passes Blu-ray metadata/header checks plus sampled first/middle/last packet, bitrate, B-frame and random-access checks",
                      "V_MPEG4/ISO/AVC", ".264");
    }

    if (i.codec_name == "mpeg2video") {
        if (i.max_keyframe_interval_seconds <= 0.0 || i.max_keyframe_interval_seconds > 1.05)
            return reject("MPEG-2 GOP/random-access interval exceeds one second in a sampled window");
        return accept("MPEG-2 elementary stream passes Blu-ray metadata/header checks plus sampled first/middle/last bitrate and GOP checks",
                      "V_MPEG-2", ".m2v");
    }

    if (i.max_keyframe_interval_seconds <= 0.0 || i.max_keyframe_interval_seconds > 1.05)
        return reject("VC-1 random-access interval exceeds one second in a sampled window");
    return accept("VC-1 elementary stream passes Blu-ray metadata/profile checks plus sampled first/middle/last bitrate and random-access checks",
                  "V_MS/VFW/WVC1", ".vc1");
}

BluRayStreamDecision evaluate_blu_ray_audio(const AudioStreamInfo& i) {
    if (!i.present) return reject("title has no audio stream");
    if (i.codec_name == "ac3") {
        if (i.sample_rate != 48000) return reject("AC-3 sample rate is not the Blu-ray 48 kHz rate");
        if (i.channels < 1 || i.channels > 6) return reject("AC-3 has more than 5.1 channels");
        if (i.bitrate_bps <= 0.0 || i.bitrate_bps > 640000.0) return reject("AC-3 bitrate exceeds the Blu-ray 640 kb/s limit");
        return accept("AC-3 stream passes Blu-ray sample-rate, channel and bitrate checks", "A_AC3", ".ac3");
    }
    if (i.codec_name == "eac3") {
        if (i.sample_rate != 48000) return reject("Dolby Digital Plus sample rate is not 48 kHz");
        if (i.channels < 1 || i.channels > 8) return reject("Dolby Digital Plus has more than 7.1 channels");
        if (i.bitrate_bps <= 0.0 || i.bitrate_bps > 4'736'000.0) return reject("Dolby Digital Plus bitrate exceeds the Blu-ray 4.736 Mb/s limit");
        return accept("Dolby Digital Plus stream passes Blu-ray sample-rate, channel and bitrate checks", "A_AC3", ".eac3");
    }
    if (i.codec_name == "dts") {
        const bool hd = i.profile.find("DTS-HD") != std::string::npos;
        if (!hd) {
            if (i.sample_rate != 48000) return reject("DTS core sample rate is not 48 kHz");
            if (i.channels < 1 || i.channels > 6) return reject("DTS core has more than 5.1 channels");
            const double core_bitrate = i.dts_core_bitrate_bps > 0.0 ? i.dts_core_bitrate_bps : i.bitrate_bps;
            if (core_bitrate <= 0.0) return reject("DTS core frame-header bitrate could not be verified");
            if (core_bitrate > 1'524'000.0 * 1.001) return reject("DTS core frame-header actual bitrate " + mbps(core_bitrate) + " Mb/s exceeds the Blu-ray 1.524 Mb/s limit");
        } else {
            if (!audio_rate_ok(i.sample_rate)) return reject("DTS-HD sample rate is not a Blu-ray primary-audio rate");
            if (i.channels < 1 || i.channels > (i.sample_rate == 192000 ? 6 : 8)) return reject("DTS-HD channel count exceeds the Blu-ray limit");
            if (i.dts_core_bitrate_bps > 1'524'000.0 * 1.001) return reject("DTS-HD compatibility core frame-header actual bitrate " + mbps(i.dts_core_bitrate_bps) + " Mb/s exceeds the Blu-ray 1.524 Mb/s DTS-core limit");
            if (i.bitrate_bps <= 0.0 || i.bitrate_bps > 24'500'000.0 * 1.001) return reject("DTS-HD sampled raw elementary average bitrate " + mbps(i.bitrate_bps) + " Mb/s exceeds the Blu-ray 24.5 Mb/s limit");
        }
        return accept(hd ? "DTS-HD stream passes Blu-ray sample-rate, channel and bitrate checks" :
                           "DTS core stream passes Blu-ray sample-rate, channel and bitrate checks",
                      "A_DTS", ".dts");
    }
    if (i.codec_name == "pcm_s16le" || i.codec_name == "pcm_s24le") {
        const int bits = i.bits_per_sample > 0 ? i.bits_per_sample : (i.codec_name == "pcm_s16le" ? 16 : 24);
        if (!audio_rate_ok(i.sample_rate)) return reject("LPCM sample rate is not 48, 96, or 192 kHz");
        if (bits != 16 && bits != 20 && bits != 24) return reject("LPCM bit depth is not 16, 20, or 24 bits");
        if (i.channels < 1 || i.channels > (i.sample_rate == 192000 ? 6 : 8)) return reject("LPCM channel count exceeds the Blu-ray limit");
        const double bitrate = static_cast<double>(i.sample_rate) * i.channels * bits;
        if (bitrate > 27'648'000.0) return reject("LPCM bitrate exceeds the Blu-ray 27.648 Mb/s limit");
        return accept("LPCM stream passes Blu-ray sample-rate, bit-depth, channel and bitrate checks", "A_LPCM", ".wav");
    }
    if (i.codec_name == "truehd")
        return reject("TrueHD core pairing cannot be proven from the selected elementary stream; it will be rebuilt with an AC-3 compatibility core");
    return reject("audio codec " + (i.codec_name.empty() ? std::string("is unknown") : i.codec_name) + " is not a supported pass-through Blu-ray primary-audio codec");
}

} // namespace bdmvauthor
