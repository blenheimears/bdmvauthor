// SPDX-License-Identifier: Apache-2.0

#include "udf_writer.hpp"

#include <algorithm>
#include <atomic>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <ctime>
#include <unordered_set>
#include <utility>

namespace udf25 {
namespace fs = std::filesystem;

namespace {

constexpr std::uint32_t kAllocSectors = 32;                 // 64 KiB
constexpr std::uint32_t kPartitionStart = 288;              // 576 KiB
constexpr std::uint32_t kMetadataDataStart = 320;           // 640 KiB
constexpr std::uint32_t kMainVdsStart = 32;                 // 64 KiB
constexpr std::uint32_t kIntegrityStart = 64;               // 128 KiB
constexpr std::uint32_t kAnchorStart = 256;
constexpr std::uint32_t kVdsSectors = 16;
constexpr std::uint32_t kTailSectors = 288;
constexpr std::uint32_t kMaxExtentBytes = 0x3ffff800U;       // sector-aligned, < 1 GiB
constexpr std::size_t kMaxLongAds = (kSectorSize - 216) / 16;

constexpr std::uint16_t kTagPvd = 1;
constexpr std::uint16_t kTagAvdp = 2;
constexpr std::uint16_t kTagIuvd = 4;
constexpr std::uint16_t kTagPd = 5;
constexpr std::uint16_t kTagLvd = 6;
constexpr std::uint16_t kTagUsd = 7;
constexpr std::uint16_t kTagTd = 8;
constexpr std::uint16_t kTagLvid = 9;
constexpr std::uint16_t kTagFsd = 256;
constexpr std::uint16_t kTagFid = 257;
constexpr std::uint16_t kTagEfe = 266;

constexpr std::uint8_t kFileTypeDirectory = 4;
constexpr std::uint8_t kFileTypeFile = 5;
constexpr std::uint8_t kFileTypeSymlink = 12;
constexpr std::uint8_t kFileTypeSystemStreamDirectory = 13;
constexpr std::uint8_t kFileTypeRealtime = 249;
constexpr std::uint8_t kFileTypeMetadata = 250;
constexpr std::uint8_t kFileTypeMetadataMirror = 251;

std::uint32_t div_up(std::uint64_t value, std::uint32_t divisor) {
    return static_cast<std::uint32_t>((value + divisor - 1) / divisor);
}

std::uint32_t align_up(std::uint32_t value, std::uint32_t alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

std::uint64_t align_up64(std::uint64_t value, std::uint64_t alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

constexpr std::uint64_t kFnv1aOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnv1aPrime = 1099511628211ULL;

void hash_bytes(std::uint64_t& hash, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= kFnv1aPrime;
    }
}

void hash_u64(std::uint64_t& hash, std::uint64_t value) {
    std::array<std::uint8_t, 8> bytes{};
    for (unsigned i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<std::uint8_t>(value >> (i * 8));
    hash_bytes(hash, bytes.data(), bytes.size());
}

void hash_string(std::uint64_t& hash, const std::string& value) {
    hash_u64(hash, value.size());
    hash_bytes(hash, value.data(), value.size());
}

std::uint64_t mix64(std::uint64_t value) {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

std::string hex_volume_set_id(std::uint64_t value) {
    std::ostringstream out;
    out << std::uppercase << std::hex << std::setfill('0') << std::setw(16) << value;
    return out.str();
}

std::string random_volume_set_id() {
    static std::atomic<std::uint64_t> counter{0};
    const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    std::uint64_t seed = static_cast<std::uint64_t>(now);
    seed ^= mix64(++counter);
    seed ^= mix64(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&seed)));
    try {
        std::random_device random;
        for (int i = 0; i < 4; ++i)
            seed ^= mix64(static_cast<std::uint64_t>(random()) << ((i & 1) * 32));
    } catch (...) {
        // The clock, address-space randomization and process-local counter remain as fallbacks.
    }
    return hex_volume_set_id(mix64(seed));
}

void put16(std::uint8_t* p, std::uint16_t value) {
    p[0] = static_cast<std::uint8_t>(value);
    p[1] = static_cast<std::uint8_t>(value >> 8);
}

void put32(std::uint8_t* p, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(value >> (i * 8));
}

void put64(std::uint8_t* p, std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) p[i] = static_cast<std::uint8_t>(value >> (i * 8));
}

std::uint16_t get16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (static_cast<std::uint16_t>(p[1]) << 8));
}

std::uint32_t get32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint64_t get64(const std::uint8_t* p) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= static_cast<std::uint64_t>(p[i]) << (i * 8);
    return value;
}

std::uint16_t crc16_ccitt(const std::uint8_t* data, std::size_t length) {
    std::uint16_t crc = 0;
    while (length--) {
        crc = static_cast<std::uint16_t>(crc ^ static_cast<std::uint16_t>(static_cast<std::uint16_t>(*data++) << 8));
        for (int bit = 0; bit < 8; ++bit)
            crc = static_cast<std::uint16_t>((crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1);
    }
    return crc;
}

void begin_tag(std::uint8_t* p, std::uint16_t id, std::uint32_t location) {
    put16(p + 0, id);
    put16(p + 2, 3);          // Descriptor Version for NSR03
    p[4] = 0;                 // checksum filled later
    p[5] = 0;
    put16(p + 6, 1);          // Tag Serial Number
    put16(p + 8, 0);
    put16(p + 10, 0);
    put32(p + 12, location);
}

void finish_tag(std::uint8_t* p, std::size_t descriptor_bytes) {
    if (descriptor_bytes < 16 || descriptor_bytes > 0xffff + 16)
        throw std::runtime_error("invalid descriptor length");
    put16(p + 8, crc16_ccitt(p + 16, descriptor_bytes - 16));
    put16(p + 10, static_cast<std::uint16_t>(descriptor_bytes - 16));
    p[4] = 0;
    std::uint8_t sum = 0;
    for (int i = 0; i < 16; ++i) if (i != 4) sum = static_cast<std::uint8_t>(sum + p[i]);
    p[4] = sum;
}

bool valid_tag(const std::uint8_t* p, std::size_t available, std::string& error) {
    if (available < 16) {
        error = "descriptor shorter than tag";
        return false;
    }
    std::uint8_t sum = 0;
    for (int i = 0; i < 16; ++i) if (i != 4) sum = static_cast<std::uint8_t>(sum + p[i]);
    if (sum != p[4]) {
        error = "tag checksum mismatch";
        return false;
    }
    const std::uint16_t crc_len = get16(p + 10);
    if (static_cast<std::size_t>(crc_len) + 16 > available) {
        error = "descriptor CRC length exceeds available data";
        return false;
    }
    if (crc16_ccitt(p + 16, crc_len) != get16(p + 8)) {
        error = "descriptor CRC mismatch";
        return false;
    }
    return true;
}

std::vector<std::uint32_t> decode_utf8(const std::string& s) {
    std::vector<std::uint32_t> out;
    for (std::size_t i = 0; i < s.size();) {
        const auto c = static_cast<std::uint8_t>(s[i]);
        std::uint32_t cp = 0;
        std::size_t n = 0;
        if (c < 0x80) { cp = c; n = 1; }
        else if ((c & 0xe0) == 0xc0) { cp = c & 0x1f; n = 2; }
        else if ((c & 0xf0) == 0xe0) { cp = c & 0x0f; n = 3; }
        else if ((c & 0xf8) == 0xf0) { cp = c & 0x07; n = 4; }
        else throw std::runtime_error("invalid UTF-8 in file name");
        if (i + n > s.size()) throw std::runtime_error("truncated UTF-8 in file name");
        for (std::size_t j = 1; j < n; ++j) {
            const auto d = static_cast<std::uint8_t>(s[i + j]);
            if ((d & 0xc0) != 0x80) throw std::runtime_error("invalid UTF-8 continuation byte");
            cp = (cp << 6) | (d & 0x3f);
        }
        if ((n == 2 && cp < 0x80) || (n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000) ||
            cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
            throw std::runtime_error("invalid UTF-8 scalar value");
        out.push_back(cp);
        i += n;
    }
    return out;
}

std::vector<std::uint8_t> encode_cs0(const std::string& s, bool fixed_dstring, std::size_t fixed_size = 0) {
    if (fixed_dstring && fixed_size == 0) throw std::runtime_error("zero-sized dstring");
    if (s.empty()) return fixed_dstring ? std::vector<std::uint8_t>(fixed_size, 0) : std::vector<std::uint8_t>{8};

    const auto cps = decode_utf8(s);
    bool eight_bit = true;
    for (auto cp : cps) if (cp > 0xff) eight_bit = false;

    std::vector<std::uint8_t> raw;
    raw.push_back(eight_bit ? 8 : 16);
    for (auto cp : cps) {
        if (eight_bit) {
            raw.push_back(static_cast<std::uint8_t>(cp));
        } else if (cp <= 0xffff) {
            raw.push_back(static_cast<std::uint8_t>(cp >> 8));
            raw.push_back(static_cast<std::uint8_t>(cp));
        } else {
            cp -= 0x10000;
            const std::uint16_t high = static_cast<std::uint16_t>(0xd800 + (cp >> 10));
            const std::uint16_t low = static_cast<std::uint16_t>(0xdc00 + (cp & 0x3ff));
            raw.push_back(static_cast<std::uint8_t>(high >> 8));
            raw.push_back(static_cast<std::uint8_t>(high));
            raw.push_back(static_cast<std::uint8_t>(low >> 8));
            raw.push_back(static_cast<std::uint8_t>(low));
        }
    }

    if (!fixed_dstring) {
        if (raw.size() > 255) throw std::runtime_error("UDF file identifier exceeds 255 bytes: " + s);
        return raw;
    }
    if (raw.size() + 1 > fixed_size) raw.resize(fixed_size - 1);
    std::vector<std::uint8_t> result(fixed_size, 0);
    std::copy(raw.begin(), raw.end(), result.begin());
    result.back() = static_cast<std::uint8_t>(raw.size());
    return result;
}

void write_dstring(std::uint8_t* p, std::size_t size, const std::string& text) {
    const auto encoded = encode_cs0(text, true, size);
    std::copy(encoded.begin(), encoded.end(), p);
}

void write_charspec(std::uint8_t* p, std::size_t size) {
    std::memset(p, 0, size);
    const char* name = "OSTA Compressed Unicode";
    std::memcpy(p + 1, name, std::min<std::size_t>(std::strlen(name), size - 1));
}

void write_entity_id(std::uint8_t* p, const std::string& id, bool udf_suffix = false, std::uint8_t domain_flags = 0) {
    std::memset(p, 0, 32);
    const std::size_t n = std::min<std::size_t>(23, id.size());
    std::memcpy(p + 1, id.data(), n);
    if (udf_suffix) {
        put16(p + 24, 0x0250);
        p[26] = domain_flags;
    }
}

void write_extent_ad(std::uint8_t* p, std::uint32_t bytes, std::uint32_t location) {
    put32(p, bytes);
    put32(p + 4, location);
}

void write_long_ad(std::uint8_t* p, std::uint32_t bytes, std::uint32_t lbn,
                   std::uint16_t partition, std::uint32_t unique_id = 0) {
    put32(p + 0, bytes);
    put32(p + 4, lbn);
    put16(p + 8, partition);
    put16(p + 10, 0);
    put32(p + 12, unique_id);
}

void write_short_ad(std::uint8_t* p, std::uint32_t bytes, std::uint32_t lbn) {
    put32(p + 0, bytes);
    put32(p + 4, lbn);
}

std::int16_t timezone_minutes(std::time_t t) {
    std::tm local{};
    std::tm utc{};
#if defined(_WIN32)
    localtime_s(&local, &t);
    gmtime_s(&utc, &t);
#else
    localtime_r(&t, &local);
    gmtime_r(&t, &utc);
#endif
    const auto local_as_local = std::mktime(&local);
    const auto utc_as_local = std::mktime(&utc);
    auto delta = static_cast<long>((local_as_local - utc_as_local) / 60);
    delta = std::clamp(delta, -1440L, 1440L);
    return static_cast<std::int16_t>(delta);
}

void write_timestamp(std::uint8_t* p, std::time_t value, bool force_utc) {
    std::tm parts{};
#if defined(_WIN32)
    if (force_utc) gmtime_s(&parts, &value); else localtime_s(&parts, &value);
#else
    if (force_utc) gmtime_r(&value, &parts); else localtime_r(&value, &parts);
#endif
    const std::int16_t tz = force_utc ? 0 : timezone_minutes(value);
    put16(p + 0, static_cast<std::uint16_t>(0x1000 | (tz & 0x0fff)));
    put16(p + 2, static_cast<std::uint16_t>(parts.tm_year + 1900));
    p[4] = static_cast<std::uint8_t>(parts.tm_mon + 1);
    p[5] = static_cast<std::uint8_t>(parts.tm_mday);
    p[6] = static_cast<std::uint8_t>(parts.tm_hour);
    p[7] = static_cast<std::uint8_t>(parts.tm_min);
    p[8] = static_cast<std::uint8_t>(parts.tm_sec);
    p[9] = p[10] = p[11] = 0;
}

std::string path_to_utf8(const fs::path& p) {
#if defined(_WIN32)
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
#else
    return p.string();
#endif
}

bool ascii_iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto ca = static_cast<unsigned char>(a[i]);
        const auto cb = static_cast<unsigned char>(b[i]);
        const auto lower_a = static_cast<unsigned char>((ca >= 'A' && ca <= 'Z') ? ca + ('a' - 'A') : ca);
        const auto lower_b = static_cast<unsigned char>((cb >= 'A' && cb <= 'Z') ? cb + ('a' - 'A') : cb);
        if (lower_a != lower_b) return false;
    }
    return true;
}

std::vector<std::string> split_image_path(const std::string& path) {
    std::vector<std::string> parts;
    std::string part;
    for (char c : path) {
        if (c == '/' || c == '\\') {
            if (!part.empty()) { parts.push_back(part); part.clear(); }
        } else {
            part.push_back(c);
        }
    }
    if (!part.empty()) parts.push_back(part);
    for (const auto& s : parts) if (s == "." || s == "..") throw std::runtime_error("invalid image path component: " + s);
    return parts;
}

std::time_t file_mtime(const fs::path& path) {
    const auto ftime = fs::last_write_time(path);
    const auto now_file = fs::file_time_type::clock::now();
    const auto now_sys = std::chrono::system_clock::now();
    const auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(ftime - now_file + now_sys);
    return std::chrono::system_clock::to_time_t(sys);
}

class SequentialOutput {
public:
    explicit SequentialOutput(const fs::path& path)
        : file_(path, std::ios::binary | std::ios::out | std::ios::trunc), stream_(&file_) {
        if (!file_) throw std::runtime_error("cannot create output file: " + path_to_utf8(path));
    }

    explicit SequentialOutput(std::ostream& stream) : stream_(&stream) {}

    void write_at(std::uint64_t offset, const void* data, std::size_t size) {
        if (offset < position_)
            throw std::runtime_error("internal error: attempted an out-of-order image write");
        write_zeros(offset - position_);
        write_raw(data, size);
    }

    void zero_range(std::uint64_t offset, std::uint64_t size) {
        if (offset < position_)
            throw std::runtime_error("internal error: attempted an out-of-order zero fill");
        write_zeros(offset - position_);
        write_zeros(size);
    }

    void finish(std::uint64_t final_size) {
        if (final_size < position_)
            throw std::runtime_error("internal error: image output exceeded its planned size");
        write_zeros(final_size - position_);
        stream_->flush();
        if (!*stream_) throw std::runtime_error("flush failed while creating image");
    }

private:
    void write_raw(const void* data, std::size_t size) {
        stream_->write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
        if (!*stream_) throw std::runtime_error("write failed while creating image");
        position_ += size;
    }

    void write_zeros(std::uint64_t size) {
        static const std::array<std::uint8_t, 1024 * 1024> zeros{};
        while (size != 0) {
            const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(size, zeros.size()));
            write_raw(zeros.data(), n);
            size -= n;
        }
    }

    std::ofstream file_;
    std::ostream* stream_ = nullptr;
    std::uint64_t position_ = 0;
};

} // namespace

struct ImageWriter::Impl {
    struct Extent {
        std::uint32_t lbn = 0;       // physical-partition-relative LBN
        std::uint32_t bytes = 0;
    };

    struct Node {
        std::string name;
        fs::path source;
        Node* parent = nullptr;
        bool directory = false;
        bool system_stream_directory = false;
        bool named_stream = false;
        bool metadata_fid = false;
        bool realtime_file = false;
        std::uint64_t size = 0;
        std::time_t mtime = 0;
        std::uint64_t unique_id = 0;
        std::uint32_t efe_lbn = 0;        // metadata-partition-relative
        std::uint32_t dir_data_lbn = 0;   // metadata-partition-relative
        std::uint32_t dir_blocks = 0;
        std::uint64_t dir_length = 0;
        std::vector<Extent> extents;
        std::vector<std::unique_ptr<Node>> children;
    };

    explicit Impl(WriterOptions o) : options(std::move(o)) {
        root.directory = true;
        root.name.clear();
        root.unique_id = 0;

        system_stream_dir.directory = true;
        system_stream_dir.system_stream_directory = true;
        system_stream_dir.unique_id = 0;
        system_stream_dir.mtime = 0;

        auto mapping = std::make_unique<Node>();
        mapping->name = "*UDF Unique ID Mapping Data";
        mapping->parent = &system_stream_dir;
        mapping->named_stream = true;
        mapping->metadata_fid = true;
        mapping->unique_id = 0;
        system_stream_dir.children.push_back(std::move(mapping));
        unique_id_mapping_node = system_stream_dir.children.back().get();
    }

    WriterOptions options;

    void cancellation_point() const {
        if (options.cancel_requested && options.cancel_requested())
            throw std::runtime_error("UDF image creation cancelled");
    }
    Node root;
    Node system_stream_dir;
    Node* unique_id_mapping_node = nullptr;
    ImageStats stats;
    std::uint64_t next_unique_id = 16;
    std::uint32_t metadata_sectors = 0;
    std::uint32_t data_start = 0;
    std::uint32_t partition_end = 0;
    std::uint32_t mirror_efe_abs = 0;
    std::uint32_t mirror_data_abs = 0;
    std::uint32_t reserve_vds_start = 0;
    std::uint32_t final_anchor = 0;
    std::time_t volume_time = 0;
    std::string volume_set_id;
    std::uint64_t reproducible_fingerprint = kFnv1aOffset;
    bool force_utc = false;
    std::unordered_set<std::string> active_directories;

    std::time_t chosen_time(const fs::path& path) const {
        if (options.fixed_mtime) return static_cast<std::time_t>(*options.fixed_mtime);
        if (options.reproducible) return static_cast<std::time_t>(946684800); // 2000-01-01 UTC
        try { return file_mtime(path); } catch (...) { return volume_time; }
    }

    Node* find_or_create_dir(Node* start, const std::vector<std::string>& parts) {
        Node* current = start;
        for (const auto& part : parts) {
            auto it = std::find_if(current->children.begin(), current->children.end(),
                                   [&](const auto& n) { return n->name == part; });
            if (it != current->children.end()) {
                if (!(*it)->directory) throw std::runtime_error("image path collides with a file: " + part);
                current = it->get();
                continue;
            }
            auto node = std::make_unique<Node>();
            node->name = part;
            node->parent = current;
            node->directory = true;
            node->unique_id = next_unique_id++;
            node->mtime = volume_time;
            current->children.push_back(std::move(node));
            current = current->children.back().get();
        }
        return current;
    }

    void add_source(Node* parent, const fs::path& source, const std::string& name, bool copy_directory_contents) {
        std::error_code ec;
        const auto symlink_status = fs::symlink_status(source, ec);
        if (ec) throw std::runtime_error("cannot stat source: " + path_to_utf8(source) + ": " + ec.message());
        const bool is_link = fs::is_symlink(symlink_status);
        if (is_link && !options.follow_symlinks) {
            if (options.verbose) std::cerr << "skip symlink: " << path_to_utf8(source) << '\n';
            return;
        }
        const auto status = options.follow_symlinks ? fs::status(source, ec) : symlink_status;
        if (ec) throw std::runtime_error("cannot follow source: " + path_to_utf8(source) + ": " + ec.message());

        if (fs::is_directory(status)) {
            std::optional<std::string> active_key;
            if (options.follow_symlinks) {
                std::error_code canon_ec;
                const auto canonical = fs::weakly_canonical(source, canon_ec);
                if (!canon_ec) {
                    active_key = path_to_utf8(canonical);
                    if (!active_directories.insert(*active_key).second) {
                        if (options.verbose) std::cerr << "skip directory cycle: " << path_to_utf8(source) << '\n';
                        return;
                    }
                }
            }

            Node* target = parent;
            if (!copy_directory_contents) {
                auto existing = std::find_if(parent->children.begin(), parent->children.end(),
                                             [&](const auto& n) { return n->name == name; });
                if (existing != parent->children.end()) {
                    if (!(*existing)->directory) throw std::runtime_error("image path collision: " + name);
                    target = existing->get();
                } else {
                    auto node = std::make_unique<Node>();
                    node->name = name;
                    node->source = source;
                    node->parent = parent;
                    node->directory = true;
                    node->unique_id = next_unique_id++;
                    node->mtime = chosen_time(source);
                    parent->children.push_back(std::move(node));
                    target = parent->children.back().get();
                }
            } else if (target == &root) {
                root.source = source;
                root.mtime = chosen_time(source);
            }

            std::vector<fs::directory_entry> entries;
            for (fs::directory_iterator it(source, fs::directory_options::skip_permission_denied, ec), end;
                 it != end; it.increment(ec)) {
                if (ec) throw std::runtime_error("directory traversal failed: " + path_to_utf8(source) + ": " + ec.message());
                entries.push_back(*it);
            }
            std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
                return path_to_utf8(a.path().filename()) < path_to_utf8(b.path().filename());
            });
            for (const auto& entry : entries)
                add_source(target, entry.path(), path_to_utf8(entry.path().filename()), false);
            if (active_key) active_directories.erase(*active_key);
        } else if (fs::is_regular_file(status)) {
            auto collision = std::find_if(parent->children.begin(), parent->children.end(),
                                          [&](const auto& n) { return n->name == name; });
            if (collision != parent->children.end()) throw std::runtime_error("image path collision: " + name);
            auto node = std::make_unique<Node>();
            node->name = name;
            node->source = source;
            node->parent = parent;
            node->directory = false;
            node->unique_id = next_unique_id++;
            node->mtime = chosen_time(source);
            node->size = fs::file_size(source, ec);
            if (ec) throw std::runtime_error("cannot obtain file size: " + path_to_utf8(source));
            stats.payload_bytes += node->size;
            parent->children.push_back(std::move(node));
            if (options.verbose) std::cerr << "add file: " << path_to_utf8(source) << '\n';
        } else {
            if (options.verbose) std::cerr << "skip non-regular file: " << path_to_utf8(source) << '\n';
        }
    }

    void import_grafts() {
        root.mtime = volume_time;
        for (const auto& graft : options.grafts) {
            if (!fs::exists(graft.source)) throw std::runtime_error("source does not exist: " + path_to_utf8(graft.source));
            auto parts = split_image_path(graft.destination);
            const bool at_root = parts.empty();
            if (at_root) {
                if (!fs::is_directory(graft.source))
                    throw std::runtime_error("only a directory can be grafted at image root");
                add_source(&root, graft.source, "", true);
                continue;
            }
            const std::string leaf = parts.back();
            parts.pop_back();
            Node* parent = find_or_create_dir(&root, parts);
            add_source(parent, graft.source, leaf, false);
        }
        for (const auto& destination : options.virtual_directories) {
            const auto parts = split_image_path(destination);
            if (parts.empty())
                throw std::runtime_error("virtual directory path must not refer to the image root");
            (void)find_or_create_dir(&root, parts);
        }
        if (options.automatic_bdmv_correction) apply_bdmv_tree_correction();
        if (options.automatic_hddvd_correction) apply_hddvd_tree_correction();
        warn_about_dvd_application_directories();
        mark_realtime_video_files();
        sort_tree(root);
    }

    Node* unique_root_child(const std::string& name) {
        Node* match = nullptr;
        for (auto& child : root.children) {
            if (!ascii_iequals(child->name, name)) continue;
            if (match)
                throw std::runtime_error("multiple case-variant /" + name + " entries exist at image root");
            match = child.get();
        }
        return match;
    }

    void apply_bdmv_tree_correction() {
        Node* bdmv = unique_root_child("BDMV");
        if (!bdmv || !bdmv->directory) return;
        bdmv->name = "BDMV";

        Node* certificate = unique_root_child("CERTIFICATE");
        if (certificate) {
            if (!certificate->directory)
                throw std::runtime_error("/CERTIFICATE exists in the image but is not a directory");
            certificate->name = "CERTIFICATE";
            return;
        }

        (void)find_or_create_dir(&root, {"CERTIFICATE", "BACKUP"});
        if (options.verbose) std::cerr << "Blu-ray correction: create /CERTIFICATE/BACKUP\n";
    }


    void apply_hddvd_tree_correction() {
        Node* hvdvd_ts = unique_root_child("HVDVD_TS");
        if (!hvdvd_ts || !hvdvd_ts->directory) return;
        hvdvd_ts->name = "HVDVD_TS";

        Node* adv_obj = unique_root_child("ADV_OBJ");
        if (!adv_obj) return;
        if (!adv_obj->directory)
            throw std::runtime_error("/ADV_OBJ exists in the image but is not a directory");
        adv_obj->name = "ADV_OBJ";
    }

    bool root_contains_directory_named(const std::string& name) const {
        return std::any_of(root.children.begin(), root.children.end(), [&](const auto& child) {
            return child->directory && ascii_iequals(child->name, name);
        });
    }

    bool flattened_root_source_is_named(const std::string& name) const {
        if (root.source.empty()) return false;
        return ascii_iequals(path_to_utf8(root.source.lexically_normal().filename()), name);
    }

    void warn_about_dvd_application_directories() const {
        std::vector<std::string> names;
        for (const std::string& candidate : {std::string("VIDEO_TS"), std::string("AUDIO_TS")}) {
            if (root_contains_directory_named(candidate) || flattened_root_source_is_named(candidate))
                names.push_back(candidate);
        }
        if (names.empty()) return;

        std::cerr << "udf25mkiso: warning: ";
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (i != 0) std::cerr << (i + 1 == names.size() ? " and " : ", ");
            std::cerr << names[i];
        }
        std::cerr << (names.size() == 1 ? " is" : " are")
                  << " being added as DVD application content to a UDF 2.50 image; "
                     "this is valid UDF, but most DVD players expect DVD media authored "
                     "as UDF 1.02, commonly with ISO 9660 compatibility, and may not play it\n";
    }

    static std::string ascii_upper(std::string value) {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::toupper(c));
        });
        return value;
    }

    static bool is_realtime_bdmv_file_path(const std::vector<std::string>& parts, const Node& node) {
        if (node.directory) return false;
        if (parts.size() == 3 && ascii_upper(parts[0]) == "BDMV" && ascii_upper(parts[1]) == "STREAM") {
            return ascii_upper(fs::path(parts[2]).extension().string()) == ".M2TS";
        }
        if (parts.size() == 4 && ascii_upper(parts[0]) == "BDMV" && ascii_upper(parts[1]) == "STREAM" &&
            ascii_upper(parts[2]) == "SSIF") {
            return ascii_upper(fs::path(parts[3]).extension().string()) == ".SSIF";
        }
        return false;
    }

    static bool is_realtime_hddvd_file_path(const std::vector<std::string>& parts, const Node& node) {
        if (node.directory || parts.size() < 2 || ascii_upper(parts[0]) != "HVDVD_TS") return false;
        return ascii_upper(fs::path(parts.back()).extension().string()) == ".EVO";
    }

    void mark_realtime_video_files_recursive(Node& node, std::vector<std::string>& parts) {
        for (auto& child : node.children) {
            parts.push_back(child->name);
            const bool bdmv_realtime = options.automatic_bdmv_correction &&
                                       is_realtime_bdmv_file_path(parts, *child);
            const bool hddvd_realtime = options.automatic_hddvd_correction &&
                                        is_realtime_hddvd_file_path(parts, *child);
            child->realtime_file = bdmv_realtime || hddvd_realtime;
            if (child->realtime_file && options.verbose) {
                std::cerr << (bdmv_realtime ? "Blu-ray" : "HD DVD")
                          << " correction: mark /" << join_path(parts)
                          << " as UDF real-time file type 249\n";
            }
            if (child->directory) mark_realtime_video_files_recursive(*child, parts);
            parts.pop_back();
        }
    }

    static std::string join_path(const std::vector<std::string>& parts) {
        std::string out;
        for (std::size_t i = 0; i < parts.size(); ++i) {
            if (i) out.push_back('/');
            out += parts[i];
        }
        return out;
    }

    void mark_realtime_video_files() {
        std::vector<std::string> parts;
        mark_realtime_video_files_recursive(root, parts);
    }

    void sort_tree(Node& node) {
        std::sort(node.children.begin(), node.children.end(), [](const auto& a, const auto& b) {
            if (a->directory != b->directory) return !a->directory; // files first, matching tsMuxer ordering
            return a->name < b->name;
        });
        for (auto& child : node.children) if (child->directory) sort_tree(*child);
    }

    std::size_t fid_size(const std::string& name) const {
        const auto encoded = encode_cs0(name, false);
        return align_up(static_cast<std::uint32_t>(38 + encoded.size()), 4);
    }

    void calculate_directory_lengths(Node& node) {
        if (!node.directory) return;
        std::uint64_t length = align_up(38U, 4U); // parent FID has no identifier
        for (const auto& child : node.children) length += fid_size(child->name);
        if (length > kMaxExtentBytes)
            throw std::runtime_error("directory stream exceeds the maximum UDF short extent size");
        node.dir_length = length;
        node.dir_blocks = div_up(length, kSectorSize);
        const auto subdirs = std::count_if(node.children.begin(), node.children.end(),
                                           [](const auto& child) { return child->directory; });
        if (subdirs >= std::numeric_limits<std::uint16_t>::max())
            throw std::runtime_error("directory has too many subdirectories for a UDF link count");
        for (auto& child : node.children) calculate_directory_lengths(*child);
    }

    void assign_efe_lbns(Node& node, std::uint32_t& lbn) {
        node.efe_lbn = lbn++;
        for (auto& child : node.children) assign_efe_lbns(*child, lbn);
    }

    void assign_directory_lbns(Node& node, std::uint32_t& lbn) {
        if (node.directory) {
            node.dir_data_lbn = lbn;
            lbn += node.dir_blocks;
        }
        for (auto& child : node.children) assign_directory_lbns(*child, lbn);
    }

    void count_nodes(const Node& node) {
        if (node.directory) ++stats.directories; else ++stats.files;
        for (const auto& child : node.children) count_nodes(*child);
    }

    void hash_tree_structure(const Node& node) {
        const std::uint8_t kind = node.directory ? 1U : 2U;
        hash_bytes(reproducible_fingerprint, &kind, sizeof(kind));
        hash_string(reproducible_fingerprint, node.name);
        hash_u64(reproducible_fingerprint, node.size);
        hash_u64(reproducible_fingerprint, node.unique_id);
        hash_u64(reproducible_fingerprint, node.children.size());
        for (const auto& child : node.children) hash_tree_structure(*child);
    }

    void initialize_reproducible_fingerprint() {
        reproducible_fingerprint = kFnv1aOffset;
        hash_string(reproducible_fingerprint, "udf25mkiso-volume-set-id-v1");
        hash_string(reproducible_fingerprint, options.volume_label);
        hash_string(reproducible_fingerprint, options.application_id);
        hash_string(reproducible_fingerprint, options.implementation_id);
        hash_tree_structure(root);
    }

    void assign_one_file_extents(Node& node, std::uint32_t& absolute_sector) {
        std::uint64_t remaining = node.size;
        std::uint32_t current_rel = absolute_sector - kPartitionStart;
        while (remaining) {
            const auto bytes = static_cast<std::uint32_t>(std::min<std::uint64_t>(remaining, kMaxExtentBytes));
            node.extents.push_back({current_rel, bytes});
            const auto sectors = div_up(bytes, kSectorSize);
            if (absolute_sector > std::numeric_limits<std::uint32_t>::max() - sectors)
                throw std::runtime_error("image exceeds the 32-bit UDF logical-block address space");
            absolute_sector += sectors;
            current_rel += sectors;
            remaining -= bytes;
        }
        if (node.extents.size() > kMaxLongAds) {
            const auto label = node.source.empty() ? node.name : path_to_utf8(node.source);
            throw std::runtime_error("file is too large for this implementation (more than about 114 GiB): " + label);
        }
    }

    void assign_data_extents(Node& node, std::uint32_t& absolute_sector) {
        if (!node.directory) assign_one_file_extents(node, absolute_sector);
        for (auto& child : node.children) assign_data_extents(*child, absolute_sector);
    }

    std::uint64_t mapping_entry_count() const {
        return stats.files + stats.directories - 1; // root has no non-parent FID
    }

    void plan_layout() {
        calculate_directory_lengths(root);
        calculate_directory_lengths(system_stream_dir);
        std::uint32_t meta_lbn = 2; // FSD and terminating descriptor
        assign_efe_lbns(root, meta_lbn);
        assign_directory_lbns(root, meta_lbn);
        assign_efe_lbns(system_stream_dir, meta_lbn);
        assign_directory_lbns(system_stream_dir, meta_lbn);
        metadata_sectors = std::max<std::uint32_t>(96, align_up(meta_lbn + 2, kAllocSectors));
        data_start = align_up(kMetadataDataStart + metadata_sectors, kAllocSectors);
        const auto entries = mapping_entry_count();
        if (entries > (std::numeric_limits<std::uint64_t>::max() - 48) / 16)
            throw std::runtime_error("unique-ID mapping stream size overflow");
        unique_id_mapping_node->size = 48 + 16 * entries;
        unique_id_mapping_node->mtime = volume_time;
        system_stream_dir.mtime = volume_time;
        std::uint32_t cursor = data_start;
        assign_one_file_extents(*unique_id_mapping_node, cursor);
        assign_data_extents(root, cursor);
        cursor = align_up(cursor, kAllocSectors);
        const std::uint64_t planned_end = static_cast<std::uint64_t>(cursor) + kAllocSectors + metadata_sectors + kTailSectors;
        if (planned_end > std::numeric_limits<std::uint32_t>::max())
            throw std::runtime_error("image exceeds the 32-bit UDF logical-block address space");
        mirror_efe_abs = cursor + kAllocSectors - 1;
        mirror_data_abs = cursor + kAllocSectors;
        partition_end = mirror_data_abs + metadata_sectors;
        reserve_vds_start = partition_end + 32;
        final_anchor = partition_end + kTailSectors - 1;
        stats.sectors = static_cast<std::uint64_t>(final_anchor) + 1;
        stats.image_bytes = stats.sectors * kSectorSize;
    }

    std::array<std::uint8_t, kSectorSize> make_efe(std::uint32_t tag_location, std::uint8_t file_type,
                                                   std::uint64_t info_length, std::uint64_t unique_id,
                                                   std::uint16_t link_count, std::time_t mtime,
                                                   const std::vector<Extent>* extents,
                                                   std::optional<std::pair<std::uint32_t, std::uint32_t>> short_ad,
                                                   bool named_stream = false) const {
        std::array<std::uint8_t, kSectorSize> b{};
        begin_tag(b.data(), kTagEfe, tag_location);
        // ICB tag at BP 16
        put32(b.data() + 16, 0);
        put16(b.data() + 20, 4);
        put16(b.data() + 22, 0);
        put16(b.data() + 24, 1);
        b[27] = file_type;
        const bool file_uses_long_ads =
            (file_type == kFileTypeFile || file_type == kFileTypeRealtime || file_type == kFileTypeSymlink);
        std::uint16_t icb_flags = file_uses_long_ads ? 0x0021 : 0x0020;
        if (named_stream) icb_flags = static_cast<std::uint16_t>(icb_flags | 0x2000);
        put16(b.data() + 34, icb_flags);
        put32(b.data() + 36, std::numeric_limits<std::uint32_t>::max());
        put32(b.data() + 40, std::numeric_limits<std::uint32_t>::max());
        put32(b.data() + 44, (file_type == kFileTypeDirectory || file_type == kFileTypeSystemStreamDirectory)
                                   ? 0x14a5
                                   : (file_type == kFileTypeFile || file_type == kFileTypeRealtime ? 0x1084 : 0));
        put16(b.data() + 48, link_count);
        b[50] = 0;
        b[51] = 0;
        put32(b.data() + 52, 0);
        put64(b.data() + 56, info_length);
        put64(b.data() + 64, info_length);
        put64(b.data() + 72, div_up(info_length, kSectorSize));
        write_timestamp(b.data() + 80, mtime, force_utc);
        write_timestamp(b.data() + 92, mtime, force_utc);
        write_timestamp(b.data() + 104, mtime, force_utc);
        write_timestamp(b.data() + 116, mtime, force_utc);
        put32(b.data() + 128, 1);
        write_entity_id(b.data() + 168, options.implementation_id, false);
        put64(b.data() + 200, unique_id);
        put32(b.data() + 208, 0);

        std::size_t descriptor_bytes = 216;
        if (short_ad) {
            put32(b.data() + 212, 8);
            write_short_ad(b.data() + 216, short_ad->first, short_ad->second);
            descriptor_bytes = 224;
        } else if (extents && !extents->empty()) {
            put32(b.data() + 212, static_cast<std::uint32_t>(extents->size() * 16));
            std::size_t off = 216;
            for (const auto& e : *extents) {
                write_long_ad(b.data() + off, e.bytes, e.lbn, 0, 0);
                off += 16;
            }
            descriptor_bytes = off;
        } else {
            put32(b.data() + 212, 0);
        }
        finish_tag(b.data(), descriptor_bytes);
        return b;
    }

    std::vector<std::uint8_t> make_fid(const Node& directory, const Node* target, bool parent_entry,
                                       std::uint64_t stream_offset) const {
        const std::vector<std::uint8_t> name = parent_entry ? std::vector<std::uint8_t>{} : encode_cs0(target->name, false);
        const std::size_t total = align_up(static_cast<std::uint32_t>(38 + name.size()), 4);
        std::vector<std::uint8_t> b(total, 0);
        const std::uint32_t tag_location = directory.dir_data_lbn + static_cast<std::uint32_t>(stream_offset / kSectorSize);
        begin_tag(b.data(), kTagFid, tag_location);
        put16(b.data() + 16, 1);
        std::uint8_t characteristics = 0;
        if (parent_entry) characteristics = 0x08 | 0x02;
        else if (target->directory) characteristics = 0x02;
        if (!parent_entry && target->metadata_fid) characteristics = static_cast<std::uint8_t>(characteristics | 0x10);
        b[18] = characteristics;
        b[19] = static_cast<std::uint8_t>(name.size());
        const Node* pointed = parent_entry ? (directory.parent ? directory.parent : &directory) : target;
        write_long_ad(b.data() + 20, kSectorSize, pointed->efe_lbn, 1,
                      pointed->unique_id ? static_cast<std::uint32_t>(pointed->unique_id) : 0);
        put16(b.data() + 36, 0);
        if (!name.empty()) std::copy(name.begin(), name.end(), b.begin() + 38);
        finish_tag(b.data(), total);
        return b;
    }

    std::vector<std::uint8_t> make_directory_stream(const Node& directory) const {
        std::vector<std::uint8_t> out;
        out.reserve(static_cast<std::size_t>(directory.dir_blocks) * kSectorSize);
        auto append = [&](std::vector<std::uint8_t> fid) {
            out.insert(out.end(), fid.begin(), fid.end());
        };
        append(make_fid(directory, nullptr, true, out.size()));
        for (const auto& child : directory.children) append(make_fid(directory, child.get(), false, out.size()));
        if (out.size() != directory.dir_length) throw std::runtime_error("internal directory-size mismatch");
        out.resize(static_cast<std::size_t>(directory.dir_blocks) * kSectorSize, 0);
        return out;
    }

    std::array<std::uint8_t, kSectorSize> make_fsd() const {
        std::array<std::uint8_t, kSectorSize> b{};
        begin_tag(b.data(), kTagFsd, 0);
        write_timestamp(b.data() + 16, volume_time, force_utc);
        put16(b.data() + 28, 3);
        put16(b.data() + 30, 3);
        put32(b.data() + 32, 1);
        put32(b.data() + 36, 1);
        put32(b.data() + 40, 0);
        put32(b.data() + 44, 0);
        write_charspec(b.data() + 48, 64);
        write_dstring(b.data() + 112, 128, options.volume_label);
        write_charspec(b.data() + 240, 64);
        write_dstring(b.data() + 304, 32, options.volume_label);
        write_long_ad(b.data() + 400, kSectorSize, root.efe_lbn, 1, 0);
        write_entity_id(b.data() + 416, "*OSTA UDF Compliant", true, 0x03);
        write_long_ad(b.data() + 448, 0, 0, 0, 0);
        write_long_ad(b.data() + 464, kSectorSize, system_stream_dir.efe_lbn, 1, 0);
        finish_tag(b.data(), 512);
        return b;
    }

    std::array<std::uint8_t, kSectorSize> make_terminating(std::uint32_t location) const {
        std::array<std::uint8_t, kSectorSize> b{};
        begin_tag(b.data(), kTagTd, location);
        finish_tag(b.data(), 512);
        return b;
    }

    void write_metadata_copy(SequentialOutput& out, std::uint32_t absolute_start) const {
        auto fsd = make_fsd();
        out.write_at(static_cast<std::uint64_t>(absolute_start) * kSectorSize, fsd.data(), fsd.size());
        auto td = make_terminating(1);
        out.write_at(static_cast<std::uint64_t>(absolute_start + 1) * kSectorSize, td.data(), td.size());
        write_efe_nodes(out, root, absolute_start);
        write_directory_nodes(out, root, absolute_start);
        write_efe_nodes(out, system_stream_dir, absolute_start);
        write_directory_nodes(out, system_stream_dir, absolute_start);
    }

    void write_efe_nodes(SequentialOutput& out, const Node& node, std::uint32_t absolute_start) const {
        const auto type = node.system_stream_directory ? kFileTypeSystemStreamDirectory
                                                         : (node.directory ? kFileTypeDirectory
                                                                           : (node.realtime_file ? kFileTypeRealtime : kFileTypeFile));
        const std::uint16_t link_count = node.directory
            ? static_cast<std::uint16_t>(1 + std::count_if(node.children.begin(), node.children.end(),
                                                          [](const auto& c) { return c->directory; }))
            : static_cast<std::uint16_t>(1);
        std::optional<std::pair<std::uint32_t, std::uint32_t>> sad;
        const std::vector<Extent>* extents = nullptr;
        if (node.directory) sad = std::make_pair(static_cast<std::uint32_t>(node.dir_length), node.dir_data_lbn);
        else extents = &node.extents;
        auto efe = make_efe(node.efe_lbn, type, node.directory ? node.dir_length : node.size,
                            node.unique_id, link_count, node.mtime, extents, sad, node.named_stream);
        out.write_at(static_cast<std::uint64_t>(absolute_start + node.efe_lbn) * kSectorSize, efe.data(), efe.size());
        for (const auto& child : node.children) write_efe_nodes(out, *child, absolute_start);
    }

    void write_directory_nodes(SequentialOutput& out, const Node& node, std::uint32_t absolute_start) const {
        if (node.directory) {
            const auto stream = make_directory_stream(node);
            out.write_at(static_cast<std::uint64_t>(absolute_start + node.dir_data_lbn) * kSectorSize,
                         stream.data(), stream.size());
        }
        for (const auto& child : node.children) write_directory_nodes(out, *child, absolute_start);
    }

    void collect_mapping_nodes(const Node& node, std::vector<const Node*>& nodes) const {
        for (const auto& child : node.children) {
            nodes.push_back(child.get());
            if (child->directory) collect_mapping_nodes(*child, nodes);
        }
    }

    std::vector<std::uint8_t> make_unique_id_mapping_data() const {
        std::vector<const Node*> nodes;
        nodes.reserve(static_cast<std::size_t>(mapping_entry_count()));
        collect_mapping_nodes(root, nodes);
        std::sort(nodes.begin(), nodes.end(), [](const Node* a, const Node* b) {
            return a->unique_id < b->unique_id;
        });
        if (nodes.size() != mapping_entry_count())
            throw std::runtime_error("internal unique-ID mapping count mismatch");
        std::vector<std::uint8_t> data(48 + nodes.size() * 16, 0);
        write_entity_id(data.data(), options.implementation_id, false);
        put32(data.data() + 32, 0); // no direct-indexing promise
        put32(data.data() + 36, static_cast<std::uint32_t>(nodes.size()));
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            const Node& node = *nodes[i];
            if (node.unique_id < 16 || node.unique_id > std::numeric_limits<std::uint32_t>::max())
                throw std::runtime_error("unique ID does not fit the mapping stream");
            auto* entry = data.data() + 48 + i * 16;
            put32(entry + 0, static_cast<std::uint32_t>(node.unique_id));
            put32(entry + 4, node.parent ? node.parent->efe_lbn : root.efe_lbn);
            put32(entry + 8, node.efe_lbn);
            put16(entry + 12, 1);
            put16(entry + 14, 1);
        }
        return data;
    }

    void write_unique_id_mapping_payload(SequentialOutput& out) const {
        const auto data = make_unique_id_mapping_data();
        if (data.size() != unique_id_mapping_node->size || unique_id_mapping_node->extents.empty())
            throw std::runtime_error("internal unique-ID mapping layout mismatch");
        std::size_t offset = 0;
        for (const auto& extent : unique_id_mapping_node->extents) {
            out.write_at(static_cast<std::uint64_t>(kPartitionStart + extent.lbn) * kSectorSize,
                         data.data() + offset, extent.bytes);
            offset += extent.bytes;
        }
        const auto final_extent = unique_id_mapping_node->extents.back();
        const auto padded = align_up64(final_extent.bytes, kSectorSize);
        if (padded > final_extent.bytes) {
            const auto end = static_cast<std::uint64_t>(kPartitionStart + final_extent.lbn) * kSectorSize + final_extent.bytes;
            out.zero_range(end, padded - final_extent.bytes);
        }
    }

    void hash_file_payloads(const Node& node, std::vector<char>& buffer) {
        if (!node.directory && node.size != 0) {
            std::ifstream input(node.source, std::ios::binary);
            if (!input) throw std::runtime_error("cannot open input file: " + path_to_utf8(node.source));
            std::uint64_t read = 0;
            while (read < node.size) {
                cancellation_point();
                const auto wanted = static_cast<std::streamsize>(
                    std::min<std::uint64_t>(buffer.size(), node.size - read));
                input.read(buffer.data(), wanted);
                const auto got = input.gcount();
                if (got <= 0)
                    throw std::runtime_error("input became shorter while hashing: " + path_to_utf8(node.source));
                hash_bytes(reproducible_fingerprint, buffer.data(), static_cast<std::size_t>(got));
                read += static_cast<std::uint64_t>(got);
            }
            char extra = 0;
            if (input.read(&extra, 1))
                throw std::runtime_error("input became longer while hashing: " + path_to_utf8(node.source));
        }
        for (const auto& child : node.children) hash_file_payloads(*child, buffer);
    }

    void write_file_payloads(SequentialOutput& out, const Node& node, std::vector<char>& buffer) {
        if (!node.directory && node.size != 0) {
            std::ifstream input(node.source, std::ios::binary);
            if (!input) throw std::runtime_error("cannot open input file: " + path_to_utf8(node.source));
            std::uint64_t written = 0;
            const std::uint64_t image_offset =
                static_cast<std::uint64_t>(kPartitionStart + node.extents.front().lbn) * kSectorSize;
            while (written < node.size) {
                cancellation_point();
                const auto wanted = static_cast<std::streamsize>(
                    std::min<std::uint64_t>(buffer.size(), node.size - written));
                input.read(buffer.data(), wanted);
                const auto got = input.gcount();
                if (got <= 0)
                    throw std::runtime_error("input became shorter while reading: " + path_to_utf8(node.source));
                out.write_at(image_offset + written, buffer.data(), static_cast<std::size_t>(got));
                written += static_cast<std::uint64_t>(got);
            }
            char extra = 0;
            if (input.read(&extra, 1))
                throw std::runtime_error("input became longer while reading: " + path_to_utf8(node.source));
            const auto padded = align_up64(node.size, kSectorSize);
            if (padded > node.size) out.zero_range(image_offset + node.size, padded - node.size);
        }
        for (const auto& child : node.children) write_file_payloads(out, *child, buffer);
    }


    std::array<std::uint8_t, kSectorSize> make_pvd(std::uint32_t location) const {
        std::array<std::uint8_t, kSectorSize> b{};
        begin_tag(b.data(), kTagPvd, location);
        put32(b.data() + 16, 0);
        put32(b.data() + 20, 0);
        write_dstring(b.data() + 24, 32, options.volume_label);
        put16(b.data() + 56, 1);
        put16(b.data() + 58, 1);
        put16(b.data() + 60, 3);
        put16(b.data() + 62, 3);
        put32(b.data() + 64, 1);
        put32(b.data() + 68, 1);
        write_dstring(b.data() + 72, 128, volume_set_id);
        write_charspec(b.data() + 200, 64);
        write_charspec(b.data() + 264, 64);
        write_extent_ad(b.data() + 328, 0, 0);
        write_extent_ad(b.data() + 336, 0, 0);
        write_entity_id(b.data() + 344, options.application_id, false);
        write_timestamp(b.data() + 376, volume_time, force_utc);
        write_entity_id(b.data() + 388, options.implementation_id, false);
        put32(b.data() + 484, 0);
        put16(b.data() + 488, 1);
        finish_tag(b.data(), 512);
        return b;
    }

    std::array<std::uint8_t, kSectorSize> make_iuvd(std::uint32_t location) const {
        std::array<std::uint8_t, kSectorSize> b{};
        begin_tag(b.data(), kTagIuvd, location);
        put32(b.data() + 16, 1);
        write_entity_id(b.data() + 20, "*UDF LV Info", true);
        write_charspec(b.data() + 52, 64);
        write_dstring(b.data() + 116, 128, options.volume_label);
        write_entity_id(b.data() + 352, options.implementation_id, false);
        const auto app = encode_cs0(options.application_id, false);
        std::copy_n(app.begin(), std::min<std::size_t>(app.size(), 128), b.begin() + 384);
        finish_tag(b.data(), 512);
        return b;
    }

    std::array<std::uint8_t, kSectorSize> make_pd(std::uint32_t location) const {
        std::array<std::uint8_t, kSectorSize> b{};
        begin_tag(b.data(), kTagPd, location);
        put32(b.data() + 16, 2);
        put16(b.data() + 20, 1);
        put16(b.data() + 22, 0);
        write_entity_id(b.data() + 24, "+NSR03", false);
        put32(b.data() + 184, 1); // read-only
        put32(b.data() + 188, kPartitionStart);
        put32(b.data() + 192, partition_end - kPartitionStart);
        write_entity_id(b.data() + 196, options.implementation_id, false);
        finish_tag(b.data(), 512);
        return b;
    }

    std::array<std::uint8_t, kSectorSize> make_lvd(std::uint32_t location) const {
        std::array<std::uint8_t, kSectorSize> b{};
        begin_tag(b.data(), kTagLvd, location);
        put32(b.data() + 16, 3);
        write_charspec(b.data() + 20, 64);
        write_dstring(b.data() + 84, 128, options.volume_label);
        put32(b.data() + 212, kSectorSize);
        write_entity_id(b.data() + 216, "*OSTA UDF Compliant", true, 0x03);
        write_long_ad(b.data() + 248, 2 * kSectorSize, 0, 1, 0);
        put32(b.data() + 264, 70);
        put32(b.data() + 268, 2);
        write_entity_id(b.data() + 272, options.implementation_id, false);
        write_extent_ad(b.data() + 432, kVdsSectors * kSectorSize, kIntegrityStart);

        // Type 1 partition map: physical partition 0.
        b[440] = 1;
        b[441] = 6;
        put16(b.data() + 442, 1);
        put16(b.data() + 444, 0);

        // Type 2 Metadata Partition Map: logical partition 1.
        b[446] = 2;
        b[447] = 64;
        put16(b.data() + 448, 0);
        write_entity_id(b.data() + 450, "*UDF Metadata Partition", true);
        put16(b.data() + 482, 1);
        put16(b.data() + 484, 0);
        put32(b.data() + 486, 0); // metadata file EFE at physical-partition LBN 0
        put32(b.data() + 490, mirror_efe_abs - kPartitionStart);
        put32(b.data() + 494, 0xffffffffU); // no metadata bitmap file
        put32(b.data() + 498, kAllocSectors);
        put16(b.data() + 502, kAllocSectors);
        b[504] = 1; // duplicate metadata

        finish_tag(b.data(), 510);
        return b;
    }

    std::array<std::uint8_t, kSectorSize> make_usd(std::uint32_t location) const {
        std::array<std::uint8_t, kSectorSize> b{};
        begin_tag(b.data(), kTagUsd, location);
        put32(b.data() + 16, 4);
        put32(b.data() + 20, 0);
        finish_tag(b.data(), 24);
        return b;
    }

    std::array<std::uint8_t, kSectorSize> make_lvid(std::uint32_t location) const {
        std::array<std::uint8_t, kSectorSize> b{};
        begin_tag(b.data(), kTagLvid, location);
        write_timestamp(b.data() + 16, volume_time, force_utc);
        put32(b.data() + 28, 1); // closed integrity
        write_extent_ad(b.data() + 32, 0, 0);
        put64(b.data() + 40, next_unique_id);
        put32(b.data() + 72, 2);

        const std::size_t app_len = std::min<std::size_t>(options.application_id.size(), 128);
        const std::uint32_t implementation_use_len = static_cast<std::uint32_t>(46 + app_len);
        put32(b.data() + 76, implementation_use_len);
        put32(b.data() + 80, 0); // physical partition free-space information unavailable/zero for closed image
        put32(b.data() + 84, 0); // metadata partition
        put32(b.data() + 88, partition_end - kPartitionStart);
        put32(b.data() + 92, metadata_sectors);
        write_entity_id(b.data() + 96, options.implementation_id, false);
        put32(b.data() + 128, static_cast<std::uint32_t>(stats.files));
        put32(b.data() + 132, static_cast<std::uint32_t>(stats.directories));
        put16(b.data() + 136, 0x0250);
        put16(b.data() + 138, 0x0250);
        put16(b.data() + 140, 0x0250);
        std::memcpy(b.data() + 142, options.application_id.data(), app_len);
        finish_tag(b.data(), 96 + implementation_use_len);
        return b;
    }

    std::array<std::uint8_t, kSectorSize> make_avdp(std::uint32_t location) const {
        std::array<std::uint8_t, kSectorSize> b{};
        begin_tag(b.data(), kTagAvdp, location);
        write_extent_ad(b.data() + 16, kVdsSectors * kSectorSize, kMainVdsStart);
        write_extent_ad(b.data() + 24, kVdsSectors * kSectorSize, reserve_vds_start);
        finish_tag(b.data(), 512);
        return b;
    }

    void write_vrs(SequentialOutput& out) const {
        auto write_vsd = [&](std::uint32_t sector, const char id[6]) {
            std::array<std::uint8_t, kSectorSize> b{};
            b[0] = 0;
            std::memcpy(b.data() + 1, id, 5);
            b[6] = 1;
            out.write_at(static_cast<std::uint64_t>(sector) * kSectorSize, b.data(), b.size());
        };
        write_vsd(16, "BEA01");
        write_vsd(17, "NSR03");
        write_vsd(18, "TEA01");
    }

    void write_vds(SequentialOutput& out, std::uint32_t start) const {
        const auto pvd = make_pvd(start + 0);
        const auto iuvd = make_iuvd(start + 1);
        const auto pd = make_pd(start + 2);
        const auto lvd = make_lvd(start + 3);
        const auto usd = make_usd(start + 4);
        const auto td = make_terminating(start + 5);
        out.write_at(static_cast<std::uint64_t>(start + 0) * kSectorSize, pvd.data(), pvd.size());
        out.write_at(static_cast<std::uint64_t>(start + 1) * kSectorSize, iuvd.data(), iuvd.size());
        out.write_at(static_cast<std::uint64_t>(start + 2) * kSectorSize, pd.data(), pd.size());
        out.write_at(static_cast<std::uint64_t>(start + 3) * kSectorSize, lvd.data(), lvd.size());
        out.write_at(static_cast<std::uint64_t>(start + 4) * kSectorSize, usd.data(), usd.size());
        out.write_at(static_cast<std::uint64_t>(start + 5) * kSectorSize, td.data(), td.size());
    }

    void write_integrity_sequence(SequentialOutput& out) const {
        const auto lvid = make_lvid(kIntegrityStart);
        const auto td = make_terminating(kIntegrityStart + 1);
        out.write_at(static_cast<std::uint64_t>(kIntegrityStart) * kSectorSize, lvid.data(), lvid.size());
        out.write_at(static_cast<std::uint64_t>(kIntegrityStart + 1) * kSectorSize, td.data(), td.size());
    }

    void write_main_metadata_file(SequentialOutput& out) const {
        const std::uint32_t metadata_bytes = metadata_sectors * kSectorSize;
        const std::uint32_t main_data_lbn = kMetadataDataStart - kPartitionStart;
        const auto main_efe = make_efe(0, kFileTypeMetadata, metadata_bytes, 0, 1, volume_time,
                                       nullptr, std::make_pair(metadata_bytes, main_data_lbn));
        out.write_at(static_cast<std::uint64_t>(kPartitionStart) * kSectorSize, main_efe.data(), main_efe.size());
        write_metadata_copy(out, kMetadataDataStart);
    }

    void write_mirror_metadata_file(SequentialOutput& out) const {
        const std::uint32_t metadata_bytes = metadata_sectors * kSectorSize;
        const std::uint32_t mirror_data_lbn = mirror_data_abs - kPartitionStart;
        const auto mirror_efe = make_efe(mirror_efe_abs - kPartitionStart, kFileTypeMetadataMirror,
                                         metadata_bytes, 0, 1, volume_time, nullptr,
                                         std::make_pair(metadata_bytes, mirror_data_lbn));
        out.write_at(static_cast<std::uint64_t>(mirror_efe_abs) * kSectorSize, mirror_efe.data(), mirror_efe.size());
        write_metadata_copy(out, mirror_data_abs);
    }

    void check_output_collision(const Node& node) const {
        if (!node.directory && !node.source.empty()) {
            std::error_code ec;
            if (fs::exists(options.output, ec) && !ec && fs::equivalent(options.output, node.source, ec) && !ec)
                throw std::runtime_error("output image is also an input file: " + path_to_utf8(node.source));
        }
        for (const auto& child : node.children) check_output_collision(*child);
    }

    ImageStats build() {
        if (options.grafts.empty()) throw std::runtime_error("no input paths supplied");
        if (options.volume_label.empty()) throw std::runtime_error("volume identifier must not be empty");
        if (encode_cs0(options.volume_label, false).size() > 31)
            throw std::runtime_error("volume identifier does not fit the 32-byte UDF volume-id field");
        if (options.implementation_id.empty()) options.implementation_id = "*udf25mkiso";
        if (options.application_id.empty()) options.application_id = "*udf25mkiso";

        force_utc = options.reproducible || options.fixed_mtime.has_value();
        if (options.fixed_mtime) volume_time = static_cast<std::time_t>(*options.fixed_mtime);
        else if (options.reproducible) volume_time = static_cast<std::time_t>(946684800);
        else volume_time = std::time(nullptr);

        cancellation_point();
        import_grafts();
        count_nodes(root);
        // Keep the 1 MiB streaming buffer on the heap.  Native Windows GUI
        // authoring runs on a Qt worker thread whose stack can be small enough
        // that the former 1 MiB automatic std::array overflowed immediately
        // when the first UDF payload file was reached.  Reuse one heap buffer
        // for both reproducible hashing and payload copying.
        std::vector<char> io_buffer(1024 * 1024);
        if (options.reproducible) {
            initialize_reproducible_fingerprint();
            hash_file_payloads(root, io_buffer);
        }
        if (stats.files > std::numeric_limits<std::uint32_t>::max() ||
            stats.directories > std::numeric_limits<std::uint32_t>::max())
            throw std::runtime_error("too many files or directories for the integrity descriptor");
        if (next_unique_id > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1)
            throw std::runtime_error("too many objects for 32-bit UDF unique IDs");
        cancellation_point();
        plan_layout();
        if (static_cast<std::uint64_t>(metadata_sectors) * kSectorSize > kMaxExtentBytes)
            throw std::runtime_error("metadata partition exceeds the maximum single UDF extent size");

        if (options.dry_run) return stats;
        if (!options.output_to_stdout) {
            if (options.output.empty()) throw std::runtime_error("no output destination selected");
            check_output_collision(root);
        }
        if (options.verbose) {
            std::cerr << "layout: partition=" << kPartitionStart << ".." << (partition_end - 1)
                      << ", metadata=" << metadata_sectors << " sectors, mirror=" << mirror_data_abs
                      << ", reserve-vds=" << reserve_vds_start << "\n";
        }

        volume_set_id = options.reproducible
                            ? hex_volume_set_id(reproducible_fingerprint)
                            : random_volume_set_id();
        if (options.verbose) std::cerr << "volume UUID: " << volume_set_id << '\n';

        std::unique_ptr<SequentialOutput> output;
        if (options.output_to_stdout) output = std::make_unique<SequentialOutput>(std::cout);
        else output = std::make_unique<SequentialOutput>(options.output);
        auto& out = *output;

        cancellation_point();
        write_vrs(out);
        write_vds(out, kMainVdsStart);
        write_integrity_sequence(out);
        {
            const auto avdp = make_avdp(kAnchorStart);
            out.write_at(static_cast<std::uint64_t>(kAnchorStart) * kSectorSize, avdp.data(), avdp.size());
        }
        cancellation_point();
        write_main_metadata_file(out);
        write_unique_id_mapping_payload(out);
        cancellation_point();
        write_file_payloads(out, root, io_buffer);
        cancellation_point();
        write_mirror_metadata_file(out);
        {
            const auto anchor = final_anchor - 256;
            const auto avdp = make_avdp(anchor);
            out.write_at(static_cast<std::uint64_t>(anchor) * kSectorSize, avdp.data(), avdp.size());
        }
        write_vds(out, reserve_vds_start);
        {
            const auto avdp = make_avdp(final_anchor);
            out.write_at(static_cast<std::uint64_t>(final_anchor) * kSectorSize, avdp.data(), avdp.size());
        }
        cancellation_point();
        out.finish(stats.image_bytes);

        if (options.verify_after_write && !options.output_to_stdout) {
            std::ostringstream report;
            if (!ImageWriter::verify(options.output, report))
                throw std::runtime_error("new image failed internal verification:\n" + report.str());
            if (options.verbose) std::cerr << report.str();
        } else if (options.verify_after_write && options.output_to_stdout && options.verbose) {
            std::cerr << "post-write verification skipped for standard output\n";
        }
        return stats;
    }
};

namespace {

void append_utf8(std::string& out, std::uint32_t cp) {
    if (cp <= 0x7fU) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7ffU) {
        out.push_back(static_cast<char>(0xc0U | (cp >> 6)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3fU)));
    } else {
        out.push_back(static_cast<char>(0xe0U | (cp >> 12)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 6) & 0x3fU)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3fU)));
    }
}

std::string decode_cs0_identifier(const std::uint8_t* data, std::size_t size) {
    if (size == 0) return {};
    const std::uint8_t compression = data[0];
    std::string result;
    if (compression == 8) {
        for (std::size_t i = 1; i < size; ++i) append_utf8(result, data[i]);
        return result;
    }
    if (compression == 16) {
        if ((size - 1) % 2 != 0)
            throw std::runtime_error("odd-length 16-bit CS0 file identifier");
        for (std::size_t i = 1; i < size; i += 2) {
            const std::uint32_t cp = (static_cast<std::uint32_t>(data[i]) << 8) | data[i + 1];
            if (cp >= 0xd800U && cp <= 0xdfffU)
                throw std::runtime_error("surrogate code point in CS0 file identifier");
            append_utf8(result, cp);
        }
        return result;
    }
    throw std::runtime_error("unsupported CS0 compression ID " + std::to_string(compression));
}

std::string udf_revision_string(std::uint16_t revision) {
    const unsigned major = ((revision >> 12) & 0x0fU) * 10U + ((revision >> 8) & 0x0fU);
    const unsigned minor = ((revision >> 4) & 0x0fU) * 10U + (revision & 0x0fU);
    std::ostringstream out;
    out << major << '.' << std::setw(2) << std::setfill('0') << minor;
    return out.str();
}

class ImageReader {
public:
    explicit ImageReader(const fs::path& path) : path_(path) {
        stream_.open(path, std::ios::binary);
        if (!stream_) throw std::runtime_error("cannot open image: " + path_to_utf8(path));
        stream_.seekg(0, std::ios::end);
        const auto end = stream_.tellg();
        if (end < 0) throw std::runtime_error("cannot determine image size: " + path_to_utf8(path));
        size_ = static_cast<std::uint64_t>(end);
    }

    std::uint64_t size() const { return size_; }

    void read_at(std::uint64_t offset, void* data, std::size_t bytes) {
        if (offset > size_ || bytes > size_ - offset) throw std::runtime_error("read beyond end of image");
        stream_.seekg(static_cast<std::streamoff>(offset));
        if (!stream_) throw std::runtime_error("seek failed while reading image");
        stream_.read(static_cast<char*>(data), static_cast<std::streamsize>(bytes));
        if (stream_.gcount() != static_cast<std::streamsize>(bytes))
            throw std::runtime_error("short read while reading image");
    }

    std::array<std::uint8_t, kSectorSize> sector(std::uint32_t lba) {
        std::array<std::uint8_t, kSectorSize> b{};
        read_at(static_cast<std::uint64_t>(lba) * kSectorSize, b.data(), b.size());
        return b;
    }

private:
    fs::path path_;
    std::ifstream stream_;
    std::uint64_t size_ = 0;
};

bool check_descriptor(const std::array<std::uint8_t, kSectorSize>& b, std::uint16_t expected_id,
                      std::uint32_t expected_location, std::ostream& report, const std::string& what) {
    std::string error;
    if (get16(b.data()) != expected_id) {
        report << what << ": expected tag " << expected_id << ", found " << get16(b.data()) << '\n';
        return false;
    }
    if (get32(b.data() + 12) != expected_location) {
        report << what << ": tag location is " << get32(b.data() + 12)
               << ", expected " << expected_location << '\n';
        return false;
    }
    if (!valid_tag(b.data(), b.size(), error)) {
        report << what << ": " << error << '\n';
        return false;
    }
    return true;
}

bool identifier_is(const std::uint8_t* entity_id, const std::string& expected) {
    const std::size_t n = std::min<std::size_t>(23, expected.size());
    return std::memcmp(entity_id + 1, expected.data(), n) == 0;
}

} // namespace

ImageWriter::ImageWriter(WriterOptions options) : impl_(std::make_unique<Impl>(std::move(options))) {}
ImageWriter::~ImageWriter() = default;
ImageWriter::ImageWriter(ImageWriter&&) noexcept = default;
ImageWriter& ImageWriter::operator=(ImageWriter&&) noexcept = default;

ImageStats ImageWriter::build() { return impl_->build(); }

bool ImageWriter::verify(const fs::path& image, std::ostream& report) {
    try {
        ImageReader reader(image);
        if (reader.size() % kSectorSize != 0) {
            report << "not a UDF 2.50 image: size is not a multiple of 2048 bytes\n";
            return false;
        }
        const std::uint64_t sectors64 = reader.size() / kSectorSize;
        if (sectors64 > std::numeric_limits<std::uint32_t>::max()) {
            report << "unsupported image sector count: " << sectors64 << '\n';
            return false;
        }
        const auto sectors = static_cast<std::uint32_t>(sectors64);
        if (sectors <= 18) {
            report << "not a UDF 2.50 image: volume is too small to contain a UDF recognition sequence\n";
            return false;
        }

        // Reject non-UDF and non-2.50 volumes before attempting detailed UDF verification.
        bool saw_iso9660 = false;
        std::optional<std::uint32_t> bea_lba;
        std::optional<std::uint32_t> nsr02_lba;
        std::optional<std::uint32_t> nsr03_lba;
        std::optional<std::uint32_t> tea_lba;
        const std::uint32_t recognition_end = std::min<std::uint32_t>(sectors, 64);
        for (std::uint32_t lba = 16; lba < recognition_end; ++lba) {
            const auto descriptor = reader.sector(lba);
            if (std::memcmp(descriptor.data() + 1, "CD001", 5) == 0) saw_iso9660 = true;
            if (std::memcmp(descriptor.data() + 1, "BEA01", 5) == 0 && !bea_lba) bea_lba = lba;
            if (std::memcmp(descriptor.data() + 1, "NSR02", 5) == 0 && !nsr02_lba) nsr02_lba = lba;
            if (std::memcmp(descriptor.data() + 1, "NSR03", 5) == 0 && !nsr03_lba) nsr03_lba = lba;
            if (std::memcmp(descriptor.data() + 1, "TEA01", 5) == 0 && !tea_lba) tea_lba = lba;
        }
        if (!nsr03_lba) {
            if (nsr02_lba) {
                report << "not a UDF 2.50 image: NSR02 identifies an older UDF volume\n";
            } else if (saw_iso9660) {
                report << "not a UDF 2.50 image: ISO 9660 volume detected without a UDF NSR03 descriptor\n";
            } else {
                report << "not a UDF 2.50 image: no UDF NSR03 descriptor was found\n";
            }
            return false;
        }
        if (!bea_lba || !tea_lba || *bea_lba >= *nsr03_lba || *nsr03_lba >= *tea_lba) {
            report << "not a valid UDF 2.50 image: malformed BEA01/NSR03/TEA01 recognition sequence\n";
            return false;
        }
        if (sectors <= 512) {
            report << "not a valid UDF 2.50 image: volume is too small for the required anchor locations\n";
            return false;
        }

        const auto probe_anchor = reader.sector(256);
        if (!check_descriptor(probe_anchor, kTagAvdp, 256, report, "UDF format probe anchor")) {
            report << "not a valid UDF 2.50 image: no valid anchor at sector 256\n";
            return false;
        }
        const std::uint32_t probe_vds_bytes = get32(probe_anchor.data() + 16);
        const std::uint32_t probe_vds_lba = get32(probe_anchor.data() + 20);
        const std::uint32_t probe_vds_sectors = div_up(probe_vds_bytes, kSectorSize);
        if (probe_vds_bytes < kSectorSize || probe_vds_lba >= sectors ||
            probe_vds_sectors > sectors - probe_vds_lba) {
            report << "not a valid UDF 2.50 image: main volume descriptor sequence is outside the volume\n";
            return false;
        }
        std::optional<std::array<std::uint8_t, kSectorSize>> probe_lvd;
        for (std::uint32_t i = 0; i < probe_vds_sectors; ++i) {
            const auto descriptor = reader.sector(probe_vds_lba + i);
            if (get16(descriptor.data()) != kTagLvd) continue;
            std::string tag_error;
            if (get32(descriptor.data() + 12) != probe_vds_lba + i ||
                !valid_tag(descriptor.data(), descriptor.size(), tag_error)) {
                report << "not a valid UDF 2.50 image: logical volume descriptor is invalid: "
                       << tag_error << '\n';
                return false;
            }
            probe_lvd = descriptor;
            break;
        }
        if (!probe_lvd) {
            report << "not a valid UDF 2.50 image: no logical volume descriptor was found\n";
            return false;
        }
        const auto* probe_domain = probe_lvd->data() + 216;
        if (!identifier_is(probe_domain, "*OSTA UDF Compliant")) {
            report << "not a UDF 2.50 image: logical volume domain identifier is not *OSTA UDF Compliant\n";
            return false;
        }
        const std::uint16_t declared_revision = get16(probe_domain + 24);
        if (declared_revision != 0x0250) {
            report << "not a UDF 2.50 image: logical volume declares UDF revision "
                   << udf_revision_string(declared_revision) << '\n';
            return false;
        }

        bool ok = true;
        const std::array<std::uint32_t, 3> anchor_lbas{256, sectors - 257, sectors - 1};
        std::array<std::uint8_t, kSectorSize> first_anchor{};
        for (std::size_t i = 0; i < anchor_lbas.size(); ++i) {
            const auto a = reader.sector(anchor_lbas[i]);
            if (i == 0) first_anchor = a;
            ok &= check_descriptor(a, kTagAvdp, anchor_lbas[i], report,
                                   "anchor at sector " + std::to_string(anchor_lbas[i]));
        }
        const std::uint32_t main_vds_bytes = get32(first_anchor.data() + 16);
        const std::uint32_t main_vds = get32(first_anchor.data() + 20);
        const std::uint32_t reserve_vds_bytes = get32(first_anchor.data() + 24);
        const std::uint32_t reserve_vds = get32(first_anchor.data() + 28);
        if (main_vds_bytes < 6 * kSectorSize || reserve_vds_bytes < 6 * kSectorSize) {
            report << "volume descriptor sequence extent is too short\n";
            return false;
        }

        std::array<std::uint8_t, kSectorSize> pd{};
        std::array<std::uint8_t, kSectorSize> lvd{};
        auto verify_vds = [&](std::uint32_t start, const std::string& name, bool capture) {
            const std::array<std::uint16_t, 6> ids{kTagPvd, kTagIuvd, kTagPd, kTagLvd, kTagUsd, kTagTd};
            for (std::size_t i = 0; i < ids.size(); ++i) {
                const auto d = reader.sector(start + static_cast<std::uint32_t>(i));
                ok &= check_descriptor(d, ids[i], start + static_cast<std::uint32_t>(i), report,
                                       name + " descriptor " + std::to_string(i));
                if (capture && i == 2) pd = d;
                if (capture && i == 3) lvd = d;
            }
        };
        verify_vds(main_vds, "main VDS", true);
        verify_vds(reserve_vds, "reserve VDS", false);

        if (get32(lvd.data() + 212) != kSectorSize) {
            report << "logical block size is not 2048 bytes\n";
            ok = false;
        }
        if (get32(lvd.data() + 264) < 70 || get32(lvd.data() + 268) < 2 || lvd[440] != 1 || lvd[446] != 2 ||
            !identifier_is(lvd.data() + 450, "*UDF Metadata Partition")) {
            report << "logical volume does not contain the expected UDF 2.50 metadata partition map\n";
            return false;
        }
        const std::uint32_t partition_start = get32(pd.data() + 188);
        const std::uint32_t partition_length = get32(pd.data() + 192);
        if (partition_start >= sectors || partition_length > sectors - partition_start) {
            report << "physical partition lies outside image\n";
            return false;
        }
        const std::uint32_t metadata_efe_lbn = get32(lvd.data() + 486);
        const std::uint32_t mirror_efe_lbn = get32(lvd.data() + 490);
        const std::uint32_t integrity_bytes = get32(lvd.data() + 432);
        const std::uint32_t integrity_lba = get32(lvd.data() + 436);
        if (integrity_bytes < kSectorSize) {
            report << "integrity sequence extent is empty\n";
            return false;
        }
        const auto lvid = reader.sector(integrity_lba);
        ok &= check_descriptor(lvid, kTagLvid, integrity_lba, report, "logical volume integrity descriptor");
        const std::uint16_t minimum_read_revision = get16(lvid.data() + 136);
        const std::uint16_t minimum_write_revision = get16(lvid.data() + 138);
        const std::uint16_t maximum_write_revision = get16(lvid.data() + 140);
        if (minimum_read_revision != 0x0250) {
            report << "logical volume integrity descriptor requires UDF revision "
                   << udf_revision_string(minimum_read_revision) << " for reading; expected 2.50\n";
            ok = false;
        }
        if (minimum_write_revision > maximum_write_revision) {
            report << "logical volume integrity descriptor has minimum write revision "
                   << udf_revision_string(minimum_write_revision) << " greater than maximum write revision "
                   << udf_revision_string(maximum_write_revision) << '\n';
            ok = false;
        }

        const auto metadata_efe = reader.sector(partition_start + metadata_efe_lbn);
        const auto mirror_efe = reader.sector(partition_start + mirror_efe_lbn);
        ok &= check_descriptor(metadata_efe, kTagEfe, metadata_efe_lbn, report, "metadata file EFE");
        ok &= check_descriptor(mirror_efe, kTagEfe, mirror_efe_lbn, report, "metadata mirror EFE");
        if (metadata_efe[27] != kFileTypeMetadata || mirror_efe[27] != kFileTypeMetadataMirror) {
            report << "metadata files have incorrect ICB file types\n";
            ok = false;
        }
        if ((get16(metadata_efe.data() + 34) & 7U) != 0 || get32(metadata_efe.data() + 212) < 8 ||
            (get16(mirror_efe.data() + 34) & 7U) != 0 || get32(mirror_efe.data() + 212) < 8) {
            report << "metadata file does not use a short allocation descriptor\n";
            return false;
        }
        const std::uint32_t metadata_bytes = get32(metadata_efe.data() + 216) & 0x3fffffffU;
        const std::uint32_t metadata_data_lbn = get32(metadata_efe.data() + 220);
        const std::uint32_t mirror_bytes = get32(mirror_efe.data() + 216) & 0x3fffffffU;
        const std::uint32_t mirror_data_lbn = get32(mirror_efe.data() + 220);
        if (metadata_bytes == 0 || metadata_bytes % kSectorSize || metadata_bytes != mirror_bytes) {
            report << "metadata and mirror extents have inconsistent lengths\n";
            return false;
        }
        const std::uint32_t metadata_sectors = metadata_bytes / kSectorSize;
        if (metadata_data_lbn > partition_length || metadata_sectors > partition_length - metadata_data_lbn ||
            mirror_data_lbn > partition_length || metadata_sectors > partition_length - mirror_data_lbn) {
            report << "metadata extent lies outside physical partition\n";
            return false;
        }
        const std::uint32_t metadata_abs = partition_start + metadata_data_lbn;
        const std::uint32_t mirror_abs = partition_start + mirror_data_lbn;

        // Compare the duplicated metadata partition sector by sector.
        for (std::uint32_t i = 0; i < metadata_sectors; ++i) {
            if (reader.sector(metadata_abs + i) != reader.sector(mirror_abs + i)) {
                report << "metadata mirror differs at metadata block " << i << '\n';
                ok = false;
                break;
            }
        }

        const auto fsd = reader.sector(metadata_abs);
        ok &= check_descriptor(fsd, kTagFsd, 0, report, "file set descriptor");
        if (!identifier_is(fsd.data() + 416, "*OSTA UDF Compliant")) {
            report << "file set descriptor has unexpected domain identifier\n";
            ok = false;
        }
        const std::uint32_t root_efe_lbn = get32(fsd.data() + 404);
        const std::uint16_t root_partition = get16(fsd.data() + 408);
        const std::uint32_t system_stream_efe_lbn = get32(fsd.data() + 468);
        const std::uint16_t system_stream_partition = get16(fsd.data() + 472);
        if (root_partition != 1 || root_efe_lbn >= metadata_sectors) {
            report << "root directory ICB is outside metadata partition\n";
            return false;
        }
        if (system_stream_partition != 1 || system_stream_efe_lbn >= metadata_sectors) {
            report << "system stream directory ICB is outside metadata partition\n";
            return false;
        }

        struct VerifiedTreeEntry {
            std::string path;
            std::uint8_t file_type = 0;
        };
        std::set<std::uint32_t> visited;
        std::vector<VerifiedTreeEntry> verified_tree;
        std::uint64_t counted_files = 0;
        std::uint64_t counted_dirs = 0;
        std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> expected_mappings;
        std::function<void(std::uint32_t, const std::string&, std::optional<bool>)> verify_node;
        verify_node = [&](std::uint32_t efe_lbn, const std::string& image_path,
                          std::optional<bool> fid_says_directory) {
            if (!visited.insert(efe_lbn).second) return;
            if (efe_lbn >= metadata_sectors) {
                report << "EFE points outside metadata partition: " << efe_lbn << '\n';
                ok = false;
                return;
            }
            const auto efe = reader.sector(metadata_abs + efe_lbn);
            if (!check_descriptor(efe, kTagEfe, efe_lbn, report, "EFE at metadata block " + std::to_string(efe_lbn))) {
                ok = false;
                return;
            }
            const std::uint8_t type = efe[27];
            verified_tree.push_back({image_path, type});
            if (fid_says_directory && *fid_says_directory != (type == kFileTypeDirectory)) {
                report << "FID/ICB file-type mismatch at " << image_path << ": FID marks the object as "
                       << (*fid_says_directory ? "a directory" : "a non-directory")
                       << " but its ICB file type is " << static_cast<unsigned>(type) << '\n';
                ok = false;
            }
            const std::uint16_t ad_type = get16(efe.data() + 34) & 7U;
            const std::uint64_t info_length = get64(efe.data() + 56);
            const std::uint32_t ad_bytes = get32(efe.data() + 212);
            if (type == kFileTypeDirectory) {
                ++counted_dirs;
                if (ad_type != 0 || ad_bytes < 8) {
                    report << "directory EFE lacks short allocation descriptor\n";
                    ok = false;
                    return;
                }
                const std::uint32_t dir_bytes = get32(efe.data() + 216) & 0x3fffffffU;
                const std::uint32_t dir_lbn = get32(efe.data() + 220);
                if (dir_bytes != info_length || dir_lbn >= metadata_sectors ||
                    div_up(dir_bytes, kSectorSize) > metadata_sectors - dir_lbn) {
                    report << "directory stream lies outside metadata partition\n";
                    ok = false;
                    return;
                }
                std::vector<std::uint8_t> stream(static_cast<std::size_t>(info_length));
                if (!stream.empty())
                    reader.read_at(static_cast<std::uint64_t>(metadata_abs + dir_lbn) * kSectorSize,
                                   stream.data(), stream.size());
                std::size_t offset = 0;
                while (offset < stream.size()) {
                    const std::size_t in_block = offset % kSectorSize;
                    if (stream.size() - offset < 16) break;
                    if (get16(stream.data() + offset) == 0) {
                        offset += std::min<std::size_t>(kSectorSize - in_block, stream.size() - offset);
                        continue;
                    }
                    const std::uint8_t id_len = stream[offset + 19];
                    const std::uint16_t impl_len = get16(stream.data() + offset + 36);
                    const std::size_t raw_len = 38ULL + impl_len + id_len;
                    const std::size_t fid_len = align_up(static_cast<std::uint32_t>(raw_len), 4);
                    if (fid_len > kSectorSize || offset + fid_len > stream.size()) {
                        report << "FID exceeds one logical block or crosses the directory boundary\n";
                        ok = false;
                        return;
                    }
                    std::string tag_error;
                    if (get16(stream.data() + offset) != kTagFid ||
                        get32(stream.data() + offset + 12) != dir_lbn + offset / kSectorSize ||
                        !valid_tag(stream.data() + offset, fid_len, tag_error)) {
                        report << "invalid FID at directory byte " << offset << ": " << tag_error << '\n';
                        ok = false;
                        return;
                    }
                    const std::uint8_t characteristics = stream[offset + 18];
                    const std::uint16_t target_partition = get16(stream.data() + offset + 28);
                    const std::uint32_t target_lbn = get32(stream.data() + offset + 24);
                    const bool parent = (characteristics & 0x08U) != 0;
                    const bool deleted = (characteristics & 0x04U) != 0;
                    if (!parent && !deleted) {
                        if (target_partition != 1) {
                            report << "FID points to unexpected partition " << target_partition << '\n';
                            ok = false;
                        } else {
                            const std::uint32_t unique_id = get32(stream.data() + offset + 32);
                            if (unique_id < 16 ||
                                !expected_mappings.emplace(unique_id, std::make_pair(efe_lbn, target_lbn)).second) {
                                report << "invalid or duplicate UDF unique ID " << unique_id
                                       << " in directory FID\n";
                                ok = false;
                            }
                            std::string child_name;
                            try {
                                child_name = decode_cs0_identifier(stream.data() + offset + 38 + impl_len, id_len);
                            } catch (const std::exception& e) {
                                report << "invalid CS0 file identifier at directory byte " << offset
                                       << ": " << e.what() << '\n';
                                ok = false;
                                child_name = "<invalid-name-" + std::to_string(offset) + ">";
                            }
                            const std::string child_path = image_path == "/"
                                ? "/" + child_name
                                : image_path + "/" + child_name;
                            verify_node(target_lbn, child_path, (characteristics & 0x02U) != 0);
                        }
                    }
                    offset += fid_len;
                }
            } else if (type == kFileTypeFile || type == kFileTypeRealtime || type == kFileTypeSymlink) {
                ++counted_files;
                if (ad_type != 1 || ad_bytes % 16 != 0 || 216ULL + ad_bytes > kSectorSize) {
                    report << "file EFE has malformed long allocation descriptors\n";
                    ok = false;
                    return;
                }
                std::uint64_t extent_total = 0;
                for (std::uint32_t off = 216; off < 216 + ad_bytes; off += 16) {
                    const std::uint32_t bytes = get32(efe.data() + off) & 0x3fffffffU;
                    const std::uint32_t lbn = get32(efe.data() + off + 4);
                    const std::uint16_t part = get16(efe.data() + off + 8);
                    if (part != 0 || lbn > partition_length || div_up(bytes, kSectorSize) > partition_length - lbn) {
                        report << "file extent lies outside physical partition\n";
                        ok = false;
                    }
                    extent_total += bytes;
                }
                if (extent_total != info_length) {
                    report << "file extent lengths do not equal information length\n";
                    ok = false;
                }
            } else {
                report << "unexpected file type " << static_cast<unsigned>(type)
                       << " in metadata directory tree\n";
                ok = false;
            }
        };
        verify_node(root_efe_lbn, "/", std::nullopt);

        const auto path_parts = [](const std::string& path) {
            return split_image_path(path);
        };
        const auto is_realtime_bdmv_path = [&](const std::string& path) {
            const auto parts = path_parts(path);
            if (parts.size() == 3 && ascii_iequals(parts[0], "BDMV") &&
                ascii_iequals(parts[1], "STREAM")) {
                return ascii_iequals(fs::path(parts[2]).extension().string(), ".m2ts");
            }
            if (parts.size() == 4 && ascii_iequals(parts[0], "BDMV") &&
                ascii_iequals(parts[1], "STREAM") && ascii_iequals(parts[2], "SSIF")) {
                return ascii_iequals(fs::path(parts[3]).extension().string(), ".ssif");
            }
            return false;
        };
        const auto path_is_within = [&](const std::string& path, const std::string& root_name) {
            const auto parts = path_parts(path);
            return !parts.empty() && ascii_iequals(parts.front(), root_name);
        };

        const auto bdmv_root = std::find_if(verified_tree.begin(), verified_tree.end(), [](const auto& entry) {
            return ascii_iequals(entry.path, "/BDMV");
        });
        if (bdmv_root != verified_tree.end()) {
            if (bdmv_root->file_type != kFileTypeDirectory) {
                report << "BDMV application root /BDMV has UDF file type "
                       << static_cast<unsigned>(bdmv_root->file_type)
                       << "; expected 4 (directory)\n";
                ok = false;
            }
            for (const auto& entry : verified_tree) {
                if (entry.path == "/") continue;
                if (entry.file_type == kFileTypeSymlink) {
                    report << "BDMV application contains symbolic link " << entry.path
                           << " (UDF file type 12), which is forbidden for BDMV usage\n";
                    ok = false;
                    continue;
                }
                if (!path_is_within(entry.path, "BDMV") && !path_is_within(entry.path, "CERTIFICATE"))
                    continue;
                if (entry.file_type == kFileTypeDirectory) continue;
                const std::uint8_t expected_type = is_realtime_bdmv_path(entry.path)
                    ? kFileTypeRealtime
                    : kFileTypeFile;
                if (entry.file_type != expected_type) {
                    report << "BDMV file " << entry.path << " has UDF file type "
                           << static_cast<unsigned>(entry.file_type) << "; expected "
                           << static_cast<unsigned>(expected_type)
                           << (expected_type == kFileTypeRealtime
                                   ? " (real-time AV stream)"
                                   : " (standard byte-addressable file)")
                           << '\n';
                    ok = false;
                }
            }
        }

        const auto is_realtime_hddvd_path = [&](const std::string& path) {
            const auto parts = path_parts(path);
            return parts.size() >= 2 && ascii_iequals(parts[0], "HVDVD_TS") &&
                   ascii_iequals(fs::path(parts.back()).extension().string(), ".evo");
        };

        const auto hddvd_root = std::find_if(verified_tree.begin(), verified_tree.end(), [](const auto& entry) {
            return ascii_iequals(entry.path, "/HVDVD_TS");
        });
        if (hddvd_root != verified_tree.end()) {
            if (hddvd_root->file_type != kFileTypeDirectory) {
                report << "HD DVD application root /HVDVD_TS has UDF file type "
                       << static_cast<unsigned>(hddvd_root->file_type)
                       << "; expected 4 (directory)\n";
                ok = false;
            }
            const auto adv_obj_root = std::find_if(verified_tree.begin(), verified_tree.end(), [](const auto& entry) {
                return ascii_iequals(entry.path, "/ADV_OBJ");
            });
            if (adv_obj_root != verified_tree.end() && adv_obj_root->file_type != kFileTypeDirectory) {
                report << "HD DVD application root /ADV_OBJ has UDF file type "
                       << static_cast<unsigned>(adv_obj_root->file_type)
                       << "; expected 4 (directory)\n";
                ok = false;
            }
            for (const auto& entry : verified_tree) {
                if (entry.path == "/") continue;
                if (!path_is_within(entry.path, "HVDVD_TS") && !path_is_within(entry.path, "ADV_OBJ"))
                    continue;
                if (entry.file_type == kFileTypeSymlink) {
                    report << "HD DVD application contains symbolic link " << entry.path
                           << " (UDF file type 12), which is forbidden for HD DVD-Video usage\n";
                    ok = false;
                    continue;
                }
                if (entry.file_type == kFileTypeDirectory) continue;
                const std::uint8_t expected_type = is_realtime_hddvd_path(entry.path)
                    ? kFileTypeRealtime
                    : kFileTypeFile;
                if (entry.file_type != expected_type) {
                    report << "HD DVD file " << entry.path << " has UDF file type "
                           << static_cast<unsigned>(entry.file_type) << "; expected "
                           << static_cast<unsigned>(expected_type)
                           << (expected_type == kFileTypeRealtime
                                   ? " (real-time EVO stream)"
                                   : " (standard byte-addressable file)")
                           << '\n';
                    ok = false;
                }
            }
        }

        std::vector<std::string> dvd_application_directories;
        for (const auto& entry : verified_tree) {
            if (entry.file_type != kFileTypeDirectory) continue;
            if (ascii_iequals(entry.path, "/VIDEO_TS"))
                dvd_application_directories.push_back("/VIDEO_TS");
            else if (ascii_iequals(entry.path, "/AUDIO_TS"))
                dvd_application_directories.push_back("/AUDIO_TS");
        }
        if (!dvd_application_directories.empty()) {
            report << "warning: ";
            for (std::size_t i = 0; i < dvd_application_directories.size(); ++i) {
                if (i != 0)
                    report << (i + 1 == dvd_application_directories.size() ? " and " : ", ");
                report << dvd_application_directories[i];
            }
            report << (dvd_application_directories.size() == 1 ? " is" : " are")
                   << " present on this UDF 2.50 volume; this is valid UDF, but most DVD "
                      "players expect DVD media authored as UDF 1.02, commonly with ISO "
                      "9660 compatibility, and may not play it\n";
        }

        // UDF read-only mastering requires the Unique ID Mapping Data named system stream.
        const auto system_efe = reader.sector(metadata_abs + system_stream_efe_lbn);
        ok &= check_descriptor(system_efe, kTagEfe, system_stream_efe_lbn, report, "system stream directory EFE");
        std::uint32_t mapping_efe_lbn = std::numeric_limits<std::uint32_t>::max();
        if (system_efe[27] != kFileTypeSystemStreamDirectory || (get16(system_efe.data() + 34) & 7U) != 0 ||
            get32(system_efe.data() + 212) < 8) {
            report << "system stream directory has an invalid EFE\n";
            ok = false;
        } else {
            const std::uint64_t system_dir_length = get64(system_efe.data() + 56);
            const std::uint32_t system_dir_bytes = get32(system_efe.data() + 216) & 0x3fffffffU;
            const std::uint32_t system_dir_lbn = get32(system_efe.data() + 220);
            if (system_dir_length != system_dir_bytes || system_dir_lbn >= metadata_sectors ||
                div_up(system_dir_bytes, kSectorSize) > metadata_sectors - system_dir_lbn) {
                report << "system stream directory data lies outside metadata partition\n";
                ok = false;
            } else {
                std::vector<std::uint8_t> stream(static_cast<std::size_t>(system_dir_length));
                if (!stream.empty())
                    reader.read_at(static_cast<std::uint64_t>(metadata_abs + system_dir_lbn) * kSectorSize,
                                   stream.data(), stream.size());
                std::size_t offset = 0;
                unsigned non_parent_entries = 0;
                while (offset < stream.size()) {
                    if (stream.size() - offset < 38) {
                        report << "truncated FID in system stream directory\n";
                        ok = false;
                        break;
                    }
                    const std::uint8_t id_len = stream[offset + 19];
                    const std::uint16_t impl_len = get16(stream.data() + offset + 36);
                    const std::size_t fid_len = align_up(static_cast<std::uint32_t>(38ULL + impl_len + id_len), 4);
                    std::string tag_error;
                    if (fid_len > kSectorSize || offset + fid_len > stream.size() ||
                        get16(stream.data() + offset) != kTagFid ||
                        get32(stream.data() + offset + 12) != system_dir_lbn + offset / kSectorSize ||
                        !valid_tag(stream.data() + offset, fid_len, tag_error)) {
                        report << "invalid FID in system stream directory: " << tag_error << '\n';
                        ok = false;
                        break;
                    }
                    const bool parent = (stream[offset + 18] & 0x08U) != 0;
                    if (!parent) {
                        ++non_parent_entries;
                        const auto expected_name = encode_cs0("*UDF Unique ID Mapping Data", false);
                        if ((stream[offset + 18] & 0x10U) == 0 || id_len != expected_name.size() ||
                            std::memcmp(stream.data() + offset + 38 + impl_len,
                                        expected_name.data(), expected_name.size()) != 0) {
                            report << "system stream directory lacks the expected metadata-named mapping stream\n";
                            ok = false;
                        }
                        if (get16(stream.data() + offset + 28) != 1) {
                            report << "mapping stream FID points to an unexpected partition\n";
                            ok = false;
                        }
                        mapping_efe_lbn = get32(stream.data() + offset + 24);
                    }
                    offset += fid_len;
                }
                if (non_parent_entries != 1) {
                    report << "system stream directory contains " << non_parent_entries
                           << " non-parent entries; expected one\n";
                    ok = false;
                }
            }
        }

        if (mapping_efe_lbn >= metadata_sectors) {
            report << "unique-ID mapping stream EFE is outside metadata partition\n";
            ok = false;
        } else {
            const auto mapping_efe = reader.sector(metadata_abs + mapping_efe_lbn);
            ok &= check_descriptor(mapping_efe, kTagEfe, mapping_efe_lbn, report, "unique-ID mapping stream EFE");
            const std::uint16_t mapping_flags = get16(mapping_efe.data() + 34);
            const std::uint64_t mapping_length = get64(mapping_efe.data() + 56);
            const std::uint32_t mapping_ad_bytes = get32(mapping_efe.data() + 212);
            if (mapping_efe[27] != kFileTypeFile || (mapping_flags & 7U) != 1 ||
                (mapping_flags & 0x2000U) == 0 || mapping_ad_bytes == 0 ||
                mapping_ad_bytes % 16 != 0 || 216ULL + mapping_ad_bytes > kSectorSize ||
                mapping_length > std::numeric_limits<std::size_t>::max()) {
                report << "unique-ID mapping stream has an invalid named-stream EFE\n";
                ok = false;
            } else {
                std::vector<std::uint8_t> mapping(static_cast<std::size_t>(mapping_length));
                std::size_t copied = 0;
                for (std::uint32_t off = 216; off < 216 + mapping_ad_bytes; off += 16) {
                    const std::uint32_t bytes = get32(mapping_efe.data() + off) & 0x3fffffffU;
                    const std::uint32_t lbn = get32(mapping_efe.data() + off + 4);
                    const std::uint16_t part = get16(mapping_efe.data() + off + 8);
                    if (part != 0 || lbn > partition_length || div_up(bytes, kSectorSize) > partition_length - lbn ||
                        bytes > mapping.size() - copied) {
                        report << "unique-ID mapping stream extent is invalid\n";
                        ok = false;
                        break;
                    }
                    reader.read_at(static_cast<std::uint64_t>(partition_start + lbn) * kSectorSize,
                                   mapping.data() + copied, bytes);
                    copied += bytes;
                }
                if (copied != mapping.size() || mapping.size() < 48) {
                    report << "unique-ID mapping stream length is inconsistent\n";
                    ok = false;
                } else {
                    const std::uint32_t mapping_header_flags = get32(mapping.data() + 32);
                    const std::uint32_t mapping_count = get32(mapping.data() + 36);
                    if (mapping.size() != 48ULL + 16ULL * mapping_count) {
                        report << "unique-ID mapping entry count is inconsistent\n";
                        ok = false;
                    } else if ((mapping_header_flags & ~1U) != 0) {
                        report << "unique-ID mapping flags contain reserved bits: 0x" << std::hex
                               << mapping_header_flags << std::dec << '\n';
                        ok = false;
                    } else if (std::any_of(mapping.begin() + 40, mapping.begin() + 48,
                                           [](std::uint8_t value) { return value != 0; })) {
                        report << "unique-ID mapping reserved bytes are nonzero\n";
                        ok = false;
                    } else {
                        const bool index_mode = (mapping_header_flags & 1U) != 0;
                        std::uint32_t previous_id = 0;
                        std::set<std::uint32_t> mapped_live_ids;
                        for (std::uint32_t i = 0; i < mapping_count; ++i) {
                            const auto* entry = mapping.data() + 48 + 16ULL * i;
                            const std::uint32_t id = get32(entry + 0);
                            const std::uint32_t parent_lbn = get32(entry + 4);
                            const std::uint32_t object_lbn = get32(entry + 8);
                            const std::uint16_t parent_part = get16(entry + 12);
                            const std::uint16_t object_part = get16(entry + 14);
                            const bool unused = parent_lbn == 0 && object_lbn == 0 &&
                                                parent_part == 0 && object_part == 0;

                            auto entry_error = [&](const std::string& reason) {
                                report << "invalid unique-ID mapping entry " << i << " (ID " << id
                                       << "): " << reason << '\n';
                                ok = false;
                            };

                            bool usable_id = true;
                            if (id < 16) {
                                entry_error("unique ID is below 16");
                                usable_id = false;
                            }
                            if (index_mode) {
                                const std::uint64_t required = 16ULL + i;
                                if (required > std::numeric_limits<std::uint32_t>::max() || id != required) {
                                    entry_error("Index Mode requires ID " + std::to_string(required));
                                }
                            } else if (i != 0 && id <= previous_id) {
                                std::ostringstream reason;
                                reason << "unique IDs are not strictly ascending: ID " << id
                                       << " follows ID " << previous_id;
                                entry_error(reason.str());
                            }
                            // Always advance the comparison point and continue checking the entry.  An
                            // ordering defect does not make its parent/object mapping disappear, and
                            // skipping it would create a misleading cascade of "missing mapping" errors.
                            previous_id = id;
                            if (!usable_id) continue;

                            const auto expected = expected_mappings.find(id);
                            if (unused) {
                                if (expected != expected_mappings.end())
                                    entry_error("unused placeholder corresponds to a live directory FID");
                                continue;
                            }
                            if (expected == expected_mappings.end()) {
                                entry_error("no live non-stream, non-parent FID has this unique ID");
                                continue;
                            }
                            if (!mapped_live_ids.insert(id).second) {
                                entry_error("duplicate mapping for a live FID");
                                continue;
                            }
                            if (expected->second.first != parent_lbn ||
                                expected->second.second != object_lbn ||
                                parent_part != 1 || object_part != 1) {
                                std::ostringstream reason;
                                reason << "expected parent " << expected->second.first << ":1 and object "
                                       << expected->second.second << ":1, found parent " << parent_lbn << ':'
                                       << parent_part << " and object " << object_lbn << ':' << object_part;
                                entry_error(reason.str());
                            }
                        }

                        for (const auto& [id, location] : expected_mappings) {
                            if (mapped_live_ids.find(id) == mapped_live_ids.end()) {
                                report << "missing unique-ID mapping for live FID ID " << id
                                       << " (parent " << location.first << ":1, object "
                                       << location.second << ":1)\n";
                                ok = false;
                            }
                        }
                    }
                }
            }
        }

        const std::uint32_t declared_files = get32(lvid.data() + 128);
        const std::uint32_t declared_dirs = get32(lvid.data() + 132);
        if (declared_files != counted_files || declared_dirs != counted_dirs) {
            report << "integrity counts differ: declared " << declared_files << " files/" << declared_dirs
                   << " directories, found " << counted_files << '/' << counted_dirs << '\n';
            ok = false;
        }

        if (ok) {
            report << "UDF 2.50 image verified: " << counted_files << " files, " << counted_dirs
                   << " directories, " << sectors << " sectors\n";
        }
        return ok;
    } catch (const std::exception& e) {
        report << "verification failed: " << e.what() << '\n';
        return false;
    }
}

} // namespace udf25
