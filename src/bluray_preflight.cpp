// SPDX-License-Identifier: Apache-2.0
#include "bdmvauthor/bluray_preflight.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace bdmvauthor {
namespace {
constexpr unsigned kBluRayPcrPid = 0x1001;
constexpr std::uintmax_t kM2tsPacketBytes = 192;
constexpr std::uintmax_t kBdcmfSegmentBytes = 196608;

unsigned be16(const unsigned char* ptr) {
    return (static_cast<unsigned>(ptr[0]) << 8) | ptr[1];
}
std::uint32_t be32(const unsigned char* ptr) {
    return (static_cast<std::uint32_t>(ptr[0]) << 24) |
           (static_cast<std::uint32_t>(ptr[1]) << 16) |
           (static_cast<std::uint32_t>(ptr[2]) << 8) | ptr[3];
}
[[noreturn]] void fail(const std::filesystem::path& path, const std::string& why) {
    throw std::runtime_error("BDCMF PCR preflight: " + path.filename().string() + ": " + why);
}

unsigned clpi_pcr_pid(const std::filesystem::path& clpi) {
    std::ifstream stream(clpi, std::ios::binary);
    if (!stream) fail(clpi, "missing CLPI metadata");
    stream.seekg(0, std::ios::end);
    const auto length = stream.tellg();
    if (length < std::streamoff(64)) fail(clpi, "CLPI metadata is truncated");
    stream.seekg(0, std::ios::beg);
    std::array<unsigned char, 32> header{};
    if (!stream.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size())))
        fail(clpi, "unable to read CLPI header");
    if (header[0] != 'H' || header[1] != 'D' || header[2] != 'M' || header[3] != 'V')
        fail(clpi, "invalid CLPI signature");
    const std::uint32_t sequence_offset = be32(header.data() + 8);
    // SequenceInfo: 4-byte section length, reserved byte, ATC count,
    // SPN_ATC_start, STC count, STC offset, and PCR PID.
    if (sequence_offset < 32 || static_cast<std::uint64_t>(sequence_offset) + 14 >
                                    static_cast<std::uint64_t>(length))
        fail(clpi, "SequenceInfo offset outside CLPI");
    stream.seekg(static_cast<std::streamoff>(sequence_offset), std::ios::beg);
    std::array<unsigned char, 14> seq{};
    if (!stream.read(reinterpret_cast<char*>(seq.data()), static_cast<std::streamsize>(seq.size())))
        fail(clpi, "truncated SequenceInfo");
    const std::uint32_t section_length = be32(seq.data());
    if (section_length < 16 || static_cast<std::uint64_t>(sequence_offset) + 4 + section_length >
                                   static_cast<std::uint64_t>(length))
        fail(clpi, "invalid SequenceInfo length");
    if (seq[5] == 0 || seq[10] == 0) fail(clpi, "SequenceInfo has no STC/PCR entry");
    return be16(seq.data() + 12);
}
}  // namespace

namespace {
// MPEG-2 PSI sections are addressed through the PAT. PMT PID 0x1000 is
// *not* a universal Blu-ray value: the bundled tsMuxer uses 0x0100.
// Reassemble a section across packets, skipping the PUSI pointer field and
// payload stuffing. A fresh section start supersedes any incomplete section.
class PsiSectionReader {
public:
    std::optional<std::vector<unsigned char>> feed(const unsigned char* payload,
                                                    std::size_t size, bool start) {
        if (start) {
            if (!size || static_cast<std::size_t>(payload[0]) + 1 > size) {
                buffer_.clear();
                expected_ = 0;
                return std::nullopt;
            }
            const std::size_t skip = 1 + payload[0];
            payload += skip;
            size -= skip;
            buffer_.clear();
            expected_ = 0;
        } else if (buffer_.empty()) {
            return std::nullopt;
        }
        while (size) {
            if (buffer_.empty() && *payload == 0xff) break;
            if (!expected_ && buffer_.size() < 3) {
                const auto take = std::min<std::size_t>(3 - buffer_.size(), size);
                buffer_.insert(buffer_.end(), payload, payload + take);
                payload += take;
                size -= take;
                if (buffer_.size() == 3) {
                    const unsigned section_length = ((buffer_[1] & 0x0fU) << 8U) | buffer_[2];
                    if (section_length < 4 || section_length > 1021) {
                        buffer_.clear();
                        return std::nullopt;
                    }
                    expected_ = 3U + section_length;
                }
                continue;
            }
            const auto take = std::min(expected_ - buffer_.size(), size);
            buffer_.insert(buffer_.end(), payload, payload + take);
            payload += take;
            size -= take;
            if (buffer_.size() == expected_) {
                auto complete = std::move(buffer_);
                buffer_.clear();
                expected_ = 0;
                return complete;
            }
        }
        return std::nullopt;
    }
private:
    std::vector<unsigned char> buffer_;
    std::size_t expected_ = 0;
};

// CRC-32/MPEG-2, initial value 0xffffffff, no reflection or final XOR.
std::uint32_t mpeg_psi_crc(const std::vector<unsigned char>& section) {
    std::uint32_t crc = 0xffffffffU;
    for (const auto byte : section) {
        crc ^= static_cast<std::uint32_t>(byte) << 24U;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc & 0x80000000U) ? (crc << 1U) ^ 0x04c11db7U : crc << 1U;
    }
    return crc;
}

std::optional<unsigned> program_pmt_pid(const std::vector<unsigned char>& pat) {
    if (pat.size() < 12 || pat[0] != 0x00 || mpeg_psi_crc(pat) != 0 ||
        (pat[5] & 1U) == 0U) return std::nullopt;
    for (std::size_t pos = 8; pos + 4 <= pat.size() - 4; pos += 4) {
        const auto program_number = be16(pat.data() + pos);
        if (program_number != 0)
            return ((pat[pos + 2] & 0x1fU) << 8U) | pat[pos + 3];
    }
    return std::nullopt;
}

bool valid_pmt_for_clock(const std::vector<unsigned char>& pmt, unsigned pcr_pid) {
    return pmt.size() >= 16 && pmt[0] == 0x02 && mpeg_psi_crc(pmt) == 0 &&
           (pmt[5] & 1U) &&
           ((((pmt[8] & 0x1fU) << 8U) | pmt[9]) == pcr_pid);
}
}  // namespace

void validate_bluray_pcr_clip(const std::filesystem::path& stream,
                              const std::filesystem::path& clpi) {
    std::error_code error;
    const auto length = std::filesystem::file_size(stream, error);
    if (error || !length || length % kBdcmfSegmentBytes || length % kM2tsPacketBytes)
        fail(stream, "size is not a nonzero multiple of 96 logical sectors (196608 bytes)");

    const unsigned declared_clpi_pid = clpi_pcr_pid(clpi);
    if (declared_clpi_pid != kBluRayPcrPid)
        fail(clpi, "SequenceInfo PCR PID is not the dedicated Blu-ray 0x1001");

    std::ifstream in(stream, std::ios::binary);
    if (!in) fail(stream, "cannot open M2TS stream");
    std::array<unsigned char, kM2tsPacketBytes> source_packet{};
    PsiSectionReader pat_reader;
    PsiSectionReader pmt_reader;
    std::optional<unsigned> pmt_pid;
    bool found_pmt = false;
    bool found_pcr = false;
    // Discover the PMT from the PAT instead of assuming a fixed PID. Both
    // tables and dedicated PCR packets must be present in a short preloaded
    // IG subclip as well as an ordinary main-program clip.
    constexpr std::uintmax_t kScanLimit = 16U * 1024U * 1024U;
    const auto to_scan = std::min(length, kScanLimit);
    for (std::uintmax_t consumed = 0; consumed + kM2tsPacketBytes <= to_scan;
         consumed += kM2tsPacketBytes) {
        if (!in.read(reinterpret_cast<char*>(source_packet.data()),
                     static_cast<std::streamsize>(source_packet.size())))
            fail(stream, "M2TS read stopped early");
        const unsigned char* ts = source_packet.data() + 4;
        if (ts[0] != 0x47) fail(stream, "transport packet synchronization lost");
        const unsigned pid = ((static_cast<unsigned>(ts[1]) & 0x1fU) << 8U) | ts[2];
        const bool start = (ts[1] & 0x40U) != 0U;
        const bool has_adaptation = (ts[3] & 0x20U) != 0U;
        const bool has_payload = (ts[3] & 0x10U) != 0U;
        if (!has_payload && !has_adaptation)
            fail(stream, "reserved transport adaptation control");
        unsigned payload_offset = 4;
        if (has_adaptation) {
            const unsigned adaptation_size = ts[4];
            if (adaptation_size > 183) fail(stream, "invalid transport adaptation-field size");
            if (adaptation_size >= 7 && (ts[5] & 0x10U)) {
                if (pid != kBluRayPcrPid)
                    fail(stream, "PCR appears on an elementary PID rather than dedicated 0x1001");
                found_pcr = true;
            }
            payload_offset += 1U + adaptation_size;
            if (payload_offset > 188) fail(stream, "transport adaptation field exceeds packet");
        }
        if (has_payload && payload_offset < 188) {
            const auto* payload = ts + payload_offset;
            const auto size = static_cast<std::size_t>(188U - payload_offset);
            if (pid == 0) {
                if (auto pat = pat_reader.feed(payload, size, start)) {
                    if (auto discovered = program_pmt_pid(*pat)) {
                        if (pmt_pid && *pmt_pid != *discovered)
                            fail(stream, "PAT changed PMT PID inside clip");
                        pmt_pid = *discovered;
                    }
                }
            } else if (pmt_pid && pid == *pmt_pid) {
                if (auto pmt = pmt_reader.feed(payload, size, start)) {
                    if (pmt->size() >= 16 && (*pmt)[0] == 0x02 && mpeg_psi_crc(*pmt) == 0) {
                        if (!valid_pmt_for_clock(*pmt, kBluRayPcrPid))
                            fail(stream, "PMT PCR PID is not dedicated Blu-ray 0x1001");
                        found_pmt = true;
                    }
                }
            }
        }
        if (found_pcr && found_pmt) return;
    }
    if (!pmt_pid) fail(stream, "missing valid PAT with a program PMT PID within first 16 MiB");
    if (!found_pmt) fail(stream, "missing valid PMT on PID declared by PAT within first 16 MiB");
    if (!found_pcr) fail(stream, "no actual PCR packets on dedicated PID 0x1001 within first 16 MiB");
}
}  // namespace bdmvauthor
