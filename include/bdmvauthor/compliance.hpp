// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <string>
#include <optional>
#include <vector>
#include "bdmvauthor/model.hpp"

namespace bdmvauthor {

struct VideoStreamInfo {
    std::string codec_name;
    std::string profile;
    std::string pixel_format;
    std::string field_order;
    std::string sample_aspect_ratio;
    std::string color_primaries;
    std::string color_transfer;
    std::string color_space;
    std::string color_range;
    int level = -1;
    int width = 0;
    int height = 0;
    double frame_rate = 0.0;
    double sampled_average_bitrate_bps = 0.0;
    bool first_packet_is_keyframe = false;
    double max_keyframe_interval_seconds = 0.0;
    int max_consecutive_b_frames = -1;

    // H.264 SPS/HRD fields obtained from FFmpeg's trace_headers bitstream parser.
    int h264_profile_idc = -1;
    int h264_level_idc = -1;
    int h264_chroma_format_idc = -1;
    int h264_bit_depth_luma_minus8 = -1;
    int h264_bit_depth_chroma_minus8 = -1;
    int h264_max_num_ref_frames = -1;
    int h264_frame_mbs_only_flag = -1;
    int h264_mb_adaptive_frame_field_flag = -1;
    bool h264_hrd_present = false;
    bool h264_buffering_period_sei_present = false;
    bool h264_picture_timing_sei_present = false;
    double h264_initial_cpb_removal_delay_90k = 0.0;
    double h264_hrd_max_bitrate_bps = 0.0;
    double h264_hrd_max_cpb_bits = 0.0;

    // HEVC VPS/SPS/HRD fields used by Ultra HD Blu-ray.
    int hevc_profile_idc = -1;
    int hevc_tier_flag = -1;
    int hevc_level_idc = -1;
    int hevc_chroma_format_idc = -1;
    int hevc_bit_depth_luma_minus8 = -1;
    int hevc_bit_depth_chroma_minus8 = -1;
    bool hevc_hrd_present = false;
    double hevc_hrd_max_bitrate_bps = 0.0;
    double hevc_hrd_max_cpb_bits = 0.0;

    // MPEG-2 sequence header/extension fields.
    int mpeg2_profile_and_level_indication = -1;
    double mpeg2_header_bitrate_bps = 0.0;
    double mpeg2_vbv_bits = 0.0;
};

struct AudioStreamInfo {
    bool present = false;
    std::string codec_name;
    std::string profile;
    std::string sample_format;
    int sample_rate = 0;
    int channels = 0;
    int bits_per_sample = 0;
    double bitrate_bps = 0.0;
    double reported_bitrate_bps = 0.0;
    double dts_core_bitrate_bps = 0.0;
};

struct BluRayStreamDecision {
    bool compliant = false;
    std::string reason;
    std::string tsmuxer_codec;
    std::string elementary_extension;
};

namespace detail {
std::optional<double> dts_core_actual_bitrate_from_bytes(const std::vector<std::uint8_t>& bytes);
}

BluRayStreamDecision evaluate_blu_ray_video_headers(const VideoStreamInfo& info, DiscTarget target = DiscTarget::BluRay1080);
BluRayStreamDecision evaluate_blu_ray_video(const VideoStreamInfo& info, DiscTarget target = DiscTarget::BluRay1080);
BluRayStreamDecision evaluate_blu_ray_audio(const AudioStreamInfo& info);

} // namespace bdmvauthor
