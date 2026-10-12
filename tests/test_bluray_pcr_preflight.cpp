// SPDX-License-Identifier: Apache-2.0
#include "bdmvauthor/bluray_preflight.hpp"

#include <array>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace {
constexpr std::size_t kPacket = 192;
constexpr std::size_t kSegmentPackets = 1024;
using Bytes = std::vector<unsigned char>;

void put32(Bytes& b, std::size_t at, std::uint32_t n) {
    for (unsigned i = 0; i < 4; ++i) b[at + i] = static_cast<unsigned char>(n >> (24U - 8U*i));
}
void write_file(const fs::path& file, const Bytes& b) {
    std::ofstream f(file, std::ios::binary);
    if (!f) throw std::runtime_error("fixture open failed");
    f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    if (!f) throw std::runtime_error("fixture write failed");
}
std::uint32_t crc32(const Bytes& v) {
    std::uint32_t c = 0xffffffffU;
    for (const auto x : v) {
        c ^= static_cast<std::uint32_t>(x) << 24U;
        for (unsigned j = 0; j < 8; ++j)
            c = c & 0x80000000U ? (c << 1U) ^ 0x04c11db7U : c << 1U;
    }
    return c;
}
void finish_crc(Bytes& v) {
    const auto c = crc32(v);
    for (unsigned j = 0; j < 4; ++j) v.push_back(static_cast<unsigned char>(c >> (24U-8U*j)));
    if (crc32(v) != 0) throw std::runtime_error("bad fixture CRC");
}
Bytes pat(unsigned pid) {
    Bytes v{0, 0xb0, 0x0d, 0, 1, 0xc1, 0, 0, 0, 1,
            static_cast<unsigned char>(0xe0 | (pid >> 8)), static_cast<unsigned char>(pid)};
    finish_crc(v);
    return v;
}
Bytes pmt(unsigned pcr, unsigned stream_count = 1) {
    Bytes v{2, 0xb0, 0, 0, 1, 0xc1, 0, 0,
            static_cast<unsigned char>(0xe0 | (pcr >> 8)), static_cast<unsigned char>(pcr), 0xf0, 0};
    for (unsigned j = 0; j < stream_count; ++j) {
        unsigned pid = 0x1400 + j;
        v.push_back(0x91);
        v.push_back(static_cast<unsigned char>(0xe0 | (pid >> 8)));
        v.push_back(static_cast<unsigned char>(pid));
        v.push_back(0xf0); v.push_back(0);
    }
    const auto len = static_cast<unsigned>(v.size() + 4 - 3);
    v[1] = static_cast<unsigned char>(0xb0U | (len >> 8U));
    v[2] = static_cast<unsigned char>(len);
    finish_crc(v);
    return v;
}
Bytes clip_template() {
    Bytes b(kSegmentPackets*kPacket, 0xff);
    for (std::size_t i = 0; i < kSegmentPackets; ++i) {
        const auto pos = i*kPacket+4;
        b[pos] = 0x47; b[pos+1] = 0x1f; b[pos+2] = 0xff; b[pos+3] = 0x10;
    }
    return b;
}
void put_section(Bytes& clip, unsigned first_packet, unsigned pid, const Bytes& section) {
    unsigned packet = first_packet;
    std::size_t cursor = 0;
    while (cursor < section.size()) {
        const auto offset = packet++*kPacket+4;
        clip[offset] = 0x47;
        clip[offset+1] = static_cast<unsigned char>(((pid >> 8) & 0x1fU) | (cursor ? 0 : 0x40));
        clip[offset+2] = static_cast<unsigned char>(pid);
        clip[offset+3] = 0x10;
        std::size_t payload = offset+4;
        if (!cursor) clip[payload++] = 0;
        const auto count = std::min<std::size_t>(section.size()-cursor, offset+188-payload);
        std::copy_n(section.begin()+static_cast<std::ptrdiff_t>(cursor),
                    static_cast<std::ptrdiff_t>(count), clip.begin()+static_cast<std::ptrdiff_t>(payload));
        cursor += count;
    }
}
void put_pcr(Bytes& clip, unsigned packet, unsigned pid) {
    const auto offset = packet*kPacket+4;
    clip[offset] = 0x47;
    clip[offset+1] = static_cast<unsigned char>((pid >> 8) & 0x1fU);
    clip[offset+2] = static_cast<unsigned char>(pid);
    clip[offset+3] = 0x20;
    clip[offset+4] = 7;
    clip[offset+5] = 0x10;
    for (unsigned i = 6; i < 12; ++i) clip[offset+i] = 0;
}
Bytes clipinfo() {
    Bytes ci(128,0);
    ci[0]='H'; ci[1]='D'; ci[2]='M'; ci[3]='V';
    put32(ci,8,48); put32(ci,48,22);
    ci[53]=1; ci[58]=1; ci[60]=0x10; ci[61]=0x01;
    return ci;
}
void validate(const fs::path& video, const fs::path& info, const Bytes& clip, const Bytes& clpi,
              bool should_pass, const std::string& scenario) {
    write_file(video, clip); write_file(info, clpi);
    try {
        bdmvauthor::validate_bluray_pcr_clip(video, info);
        if (!should_pass) throw std::logic_error("unexpected acceptance: " + scenario);
    } catch (const std::runtime_error& e) {
        if (should_pass) throw std::runtime_error(scenario + ": " + e.what());
    }
}
}
int main() {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto dir = fs::temp_directory_path() / ("bdmvauthor-pcr-209-" + std::to_string(unique));
    fs::create_directories(dir);
    const auto video = dir / "00001.m2ts";
    const auto info = dir / "00001.clpi";
    try {
        const Bytes clpi = clipinfo();
        auto clip = clip_template();
        // This is the real bundled tsMuxer contract: PAT -> 0x0100, not 0x1000.
        put_section(clip,0,0,pat(0x0100));
        put_section(clip,1,0x0100,pmt(0x1001));
        put_pcr(clip,2,0x1001);
        validate(video,info,clip,clpi,true,"tsMuxer PMT at 0x0100");
        // The validator must follow PAT on any legal nonzero PMT PID.
        auto alternative = clip_template();
        put_section(alternative,0,0,pat(0x0350));
        put_section(alternative,1,0x0350,pmt(0x1001));
        put_pcr(alternative,2,0x1001);
        validate(video,info,alternative,clpi,true,"PAT-discovered PMT PID 0x0350");
        // PMT can legitimately cross packet boundaries in larger programs.
        auto multipart = clip_template();
        put_section(multipart,0,0,pat(0x0100));
        put_section(multipart,1,0x0100,pmt(0x1001,36));
        put_pcr(multipart,3,0x1001);
        validate(video,info,multipart,clpi,true,"multipart PMT reassembly");
        auto bad = clip;
        bad[4+1] = 0x1f; bad[4+2] = 0xff;
        validate(video,info,bad,clpi,false,"missing PAT");
        bad = clip;
        bad[kPacket+4+1] = 0x1f; bad[kPacket+4+2] = 0xff;
        validate(video,info,bad,clpi,false,"missing PMT");
        bad = clip;
        bad[4+4+1+10] ^= 1;
        validate(video,info,bad,clpi,false,"corrupt PAT CRC");
        bad = clip;
        bad[kPacket+4+4+1+8] ^= 1;
        validate(video,info,bad,clpi,false,"corrupt PMT CRC");
        bad = clip_template();
        put_section(bad,0,0,pat(0x0100));
        put_section(bad,1,0x0100,pmt(0x1011));
        put_pcr(bad,2,0x1001);
        validate(video,info,bad,clpi,false,"wrong PMT clock PID");
        bad = clip;
        put_pcr(bad,2,0x1011);
        validate(video,info,bad,clpi,false,"PCR on elementary PID");
        bad = clip;
        bad[2*kPacket+4+5]=0;
        validate(video,info,bad,clpi,false,"no PCR field");
        auto wrong_clpi=clpi;
        wrong_clpi[61]=0x11;
        validate(video,info,clip,wrong_clpi,false,"CLPI PCR disagrees");
        bad = clip;
        bad.resize(bad.size()-kPacket);
        validate(video,info,bad,clpi,false,"unaligned stream");
        fs::remove_all(dir);
        return 0;
    } catch (...) {
        fs::remove_all(dir);
        throw;
    }
}
