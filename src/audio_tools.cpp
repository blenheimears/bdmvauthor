// SPDX-License-Identifier: Apache-2.0
#include "bdmvauthor/audio_tools.hpp"
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <algorithm>
#include <cmath>
#include <deque>
#include <stdexcept>
#include <utility>
#include <vector>

namespace bdmvauthor::detail {
namespace {
namespace fs = std::filesystem;

std::vector<unsigned char> read_binary(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) throw std::runtime_error("cannot read " + p.string());
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

void write_binary(const fs::path& p, const std::vector<unsigned char>& b) {
    std::ofstream f(p, std::ios::binary);
    if (!f) throw std::runtime_error("cannot write " + p.string());
    f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    if (!f) throw std::runtime_error("failed while writing " + p.string());
}
} // namespace

int merge_truehd_ac3_core(const fs::path& truehd_path, const fs::path& ac3_path, const fs::path& output) {
    const auto thd = read_binary(truehd_path);
    const auto ac3 = read_binary(ac3_path);
    constexpr std::uint64_t Ac3Samples = 1536;
    // AC-3 frmsizecod is two codes per nominal bitrate.  BDMV Author encodes
    // the compatibility core at 48 kHz, where one 1536-sample frame contains
    // exactly bitrate_kbps * 4 bytes.  Do not assume the 640 kb/s/2560-byte
    // case: the UI deliberately permits every legal Blu-ray AC-3 bitrate.
    constexpr unsigned Ac3BitratesKbps[] = {
        32, 40, 48, 56, 64, 80, 96, 112, 128, 160,
        192, 224, 256, 320, 384, 448, 512, 576, 640
    };

    if (ac3.empty())
        throw std::runtime_error("experimental TrueHD AC-3 core is empty");
    std::vector<std::pair<std::size_t, std::size_t>> ac3_frames;
    for (std::size_t pos = 0; pos < ac3.size();) {
        if (pos + 5U > ac3.size() || ac3[pos] != 0x0b || ac3[pos + 1U] != 0x77)
            throw std::runtime_error("experimental TrueHD AC-3 core contains an invalid AC-3 frame");
        const unsigned fscod = static_cast<unsigned>(ac3[pos + 4U] >> 6U);
        const unsigned frmsizecod = static_cast<unsigned>(ac3[pos + 4U] & 0x3fU);
        if (fscod != 0U)
            throw std::runtime_error("experimental TrueHD AC-3 core must use 48 kHz AC-3");
        const unsigned bitrate_index = frmsizecod >> 1U;
        if (bitrate_index >= std::size(Ac3BitratesKbps))
            throw std::runtime_error("experimental TrueHD AC-3 core has an invalid AC-3 frame-size code");
        const std::size_t frame_bytes = static_cast<std::size_t>(Ac3BitratesKbps[bitrate_index]) * 4U;
        if (pos + frame_bytes > ac3.size())
            throw std::runtime_error("experimental TrueHD AC-3 core ends in a truncated AC-3 frame");
        ac3_frames.emplace_back(pos, frame_bytes);
        pos += frame_bytes;
    }
    if (thd.size() < 9)
        throw std::runtime_error("experimental TrueHD encoder produced an empty or truncated stream");

    std::size_t pos = 0;
    std::uint64_t samples_per_truehd_frame = 0;
    std::vector<std::pair<std::size_t, std::size_t>> frames;
    while (pos < thd.size()) {
        if (pos + 8 > thd.size()) throw std::runtime_error("truncated TrueHD frame header");
        const std::size_t len = (static_cast<std::size_t>(thd[pos] & 0x0f) << 9) |
                                (static_cast<std::size_t>(thd[pos + 1]) << 1);
        if (len == 0 || pos + len > thd.size()) throw std::runtime_error("invalid TrueHD frame size");
        if (samples_per_truehd_frame == 0 && pos + 9 <= thd.size() &&
            thd[pos + 4] == 0xf8 && thd[pos + 5] == 0x72 && thd[pos + 6] == 0x6f && thd[pos + 7] == 0xba) {
            const unsigned ratebits = static_cast<unsigned>(thd[pos + 8] >> 4);
            if (ratebits > 2U)
                throw std::runtime_error("experimental TrueHD+AC-3 supports 48, 96, or 192 kHz TrueHD");
            samples_per_truehd_frame = 40U << ratebits;
        }
        frames.emplace_back(pos, len);
        pos += len;
    }
    if (samples_per_truehd_frame == 0)
        throw std::runtime_error("could not locate a TrueHD major sync frame");

    // TrueHD access units remain 1200 Hz: they contain 40/80/160 samples at
    // 48/96/192 kHz respectively. Measure the largest one-second window and add
    // the compatibility core's actual AC-3 rate. This is intentionally
    // conservative: the core is CBR and therefore consumes that transport
    // budget continuously while the TrueHD frame sizes vary with content.
    const std::size_t truehd_frames_per_second = 1200U;
    std::deque<std::size_t> truehd_window;
    std::uint64_t truehd_window_bytes = 0;
    std::uint64_t truehd_peak_window_bytes = 0;
    for (const auto& frame : frames) {
        truehd_window.push_back(frame.second);
        truehd_window_bytes += frame.second;
        if (truehd_window.size() > truehd_frames_per_second) {
            truehd_window_bytes -= truehd_window.front();
            truehd_window.pop_front();
        }
        truehd_peak_window_bytes = std::max(truehd_peak_window_bytes, truehd_window_bytes);
    }
    unsigned ac3_peak_kbps = 0;
    for (const auto& [p, frame_bytes] : ac3_frames) {
        (void)frame_bytes;
        const unsigned frmsizecod = static_cast<unsigned>(ac3[p + 4U] & 0x3fU);
        ac3_peak_kbps = std::max(ac3_peak_kbps, Ac3BitratesKbps[frmsizecod >> 1U]);
    }
    const auto truehd_peak_kbps = static_cast<int>(std::ceil(static_cast<double>(truehd_peak_window_bytes) * 8.0 / 1000.0));

    std::vector<unsigned char> merged;
    merged.reserve(thd.size() + ac3.size());
    std::size_t core_index = 0;
    auto append_core = [&]() {
        const auto [p, frame_bytes] = ac3_frames[core_index];
        merged.insert(merged.end(), ac3.begin() + static_cast<std::ptrdiff_t>(p),
                      ac3.begin() + static_cast<std::ptrdiff_t>(p + frame_bytes));
        ++core_index;
    };
    append_core();
    std::uint64_t decoded_samples = 0;
    const std::uint64_t rate_multiplier = samples_per_truehd_frame / 40U;
    std::uint64_t next_core_sample = Ac3Samples * rate_multiplier;
    for (const auto& [frame_pos, frame_len] : frames) {
        merged.insert(merged.end(), thd.begin() + static_cast<std::ptrdiff_t>(frame_pos),
                      thd.begin() + static_cast<std::ptrdiff_t>(frame_pos + frame_len));
        decoded_samples += samples_per_truehd_frame;
        while (decoded_samples >= next_core_sample && core_index < ac3_frames.size()) {
            append_core();
            next_core_sample += Ac3Samples * rate_multiplier;
        }
    }
    while (core_index < ac3_frames.size()) append_core();
    write_binary(output, merged);
    return truehd_peak_kbps + static_cast<int>(ac3_peak_kbps);
}

} // namespace bdmvauthor::detail
