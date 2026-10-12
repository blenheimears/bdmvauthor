#pragma once

#include <algorithm>
#include <cstdint>

namespace bdmvauthor {

// mjpegtools mplex estimates the program-stream rate as roughly 102.05% of
// the sum of the elementary-stream nominal rates.  Its AC-3 parser reports
// standard AC-3 rates in binary kbit/s (bit_rate * 1024), while BDMV Author's
// UI/settings use decimal kb/s.  Budget against the same conservative nominal
// rate mplex sees so a legal 10.08 Mb/s DVD program stream is not overfilled.
inline constexpr std::int64_t dvd_mplex_conservative_audio_nominal_bps(int aggregate_audio_kbps) {
    if (aggregate_audio_kbps <= 0) return 0;
    const std::int64_t binary_nominal_bps = static_cast<std::int64_t>(aggregate_audio_kbps) * 1024LL;
    // Round to the next whole decimal kb/s.  This exactly covers AC-3 and is
    // only slightly conservative for LPCM.
    return ((binary_nominal_bps + 999LL) / 1000LL) * 1000LL;
}

inline constexpr std::int64_t dvd_mplex_estimated_program_bitrate_bps(int video_kbps,
                                                                       int aggregate_audio_kbps) {
    const std::int64_t nominal_bps = static_cast<std::int64_t>(std::max(0, video_kbps)) * 1000LL +
        dvd_mplex_conservative_audio_nominal_bps(aggregate_audio_kbps);
    // Mirrors mplex's 1.0205 nominal-rate expansion and its 50-byte/8-bit
    // rounding path closely enough to make the preflight conservative.
    const std::int64_t expanded_bps = nominal_bps * 10205LL / 10000LL;
    const std::int64_t mux_bytes_per_second = ((expanded_bps / 50LL + 25LL) * 50LL) / 8LL;
    return mux_bytes_per_second * 8LL;
}

inline constexpr int dvd_mplex_safe_video_bitrate_kbps(int transport_limit_kbps,
                                                         int aggregate_audio_kbps,
                                                         int codec_video_limit_kbps) {
    // Leave a small margin beyond mplex's own estimator for packet/timestamp
    // rounding and VBR packing.  32 kb/s costs essentially no DVD quality but
    // prevents operating exactly on the edge that previously caused late DTS
    // delivery and "Too many frame drops" failures.
    constexpr int kMuxSafetyMarginKbps = 32;
    const int usable_transport_kbps = std::max(0, transport_limit_kbps - kMuxSafetyMarginKbps);
    int low = 0;
    int high = std::max(0, codec_video_limit_kbps);
    while (low < high) {
        const int mid = low + (high - low + 1) / 2;
        if (dvd_mplex_estimated_program_bitrate_bps(mid, aggregate_audio_kbps) <=
            static_cast<std::int64_t>(usable_transport_kbps) * 1000LL)
            low = mid;
        else
            high = mid - 1;
    }
    return low;
}

} // namespace bdmvauthor
