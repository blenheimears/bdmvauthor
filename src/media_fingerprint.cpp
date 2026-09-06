// SPDX-License-Identifier: Apache-2.0
#include "bdmvauthor/media_fingerprint.hpp"
#include <openssl/evp.h>
#include <array>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace bdmvauthor {
namespace {
using MdCtx = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
std::string hex(const unsigned char* data, unsigned int size) {
    std::ostringstream out; out << std::hex << std::setfill('0');
    for (unsigned int i=0;i<size;++i) out << std::setw(2) << static_cast<unsigned>(data[i]);
    return out.str();
}
std::string hash_stream(std::istream& in, std::uint64_t length) {
    MdCtx ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!ctx || EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1)
        throw std::runtime_error("unable to initialize SHA-256");
    std::array<char,256*1024> buf{};
    std::uint64_t remaining=length;
    while (remaining) {
        const auto want=static_cast<std::streamsize>(std::min<std::uint64_t>(remaining,buf.size()));
        in.read(buf.data(),want); const auto got=in.gcount();
        if (got<=0) throw std::runtime_error("unexpected end of file while calculating SHA-256");
        if (EVP_DigestUpdate(ctx.get(),buf.data(),static_cast<std::size_t>(got)) != 1)
            throw std::runtime_error("SHA-256 update failed");
        remaining-=static_cast<std::uint64_t>(got);
    }
    unsigned char digest[EVP_MAX_MD_SIZE]{}; unsigned int n=0;
    if (EVP_DigestFinal_ex(ctx.get(),digest,&n) != 1) throw std::runtime_error("SHA-256 finalize failed");
    return hex(digest,n);
}
std::string hash_block(const std::filesystem::path& path,std::uint64_t offset,std::uint64_t length) {
    std::ifstream in(path,std::ios::binary); if(!in) throw std::runtime_error("cannot open media file for SHA-256: "+path.string());
    in.seekg(static_cast<std::streamoff>(offset)); if(!in) throw std::runtime_error("cannot seek media file for SHA-256: "+path.string());
    return hash_stream(in,length);
}
}
std::string sha256_hex(std::string_view data) {
    MdCtx ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!ctx || EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1 ||
        EVP_DigestUpdate(ctx.get(),data.data(),data.size()) != 1)
        throw std::runtime_error("unable to calculate SHA-256");
    unsigned char digest[EVP_MAX_MD_SIZE]{}; unsigned int n=0;
    if (EVP_DigestFinal_ex(ctx.get(),digest,&n) != 1) throw std::runtime_error("SHA-256 finalize failed");
    return hex(digest,n);
}
MediaFingerprint fingerprint_media_file(const std::filesystem::path& path) {
    std::error_code ec; const auto size=std::filesystem::file_size(path,ec);
    if(ec) throw std::runtime_error("cannot determine media file size for SHA-256: "+path.string());
    MediaFingerprint fp; fp.file_size=size;
    if(size < MediaFingerprint::WholeFileThreshold) {
        fp.whole_file=true; fp.whole_sha256=hash_block(path,0,size); return fp;
    }
    fp.whole_file=false; fp.middle_offset=(size-MediaFingerprint::SampleBytes)/2;
    fp.first_sha256=hash_block(path,0,MediaFingerprint::SampleBytes);
    fp.middle_sha256=hash_block(path,fp.middle_offset,MediaFingerprint::SampleBytes);
    fp.last_sha256=hash_block(path,size-MediaFingerprint::SampleBytes,MediaFingerprint::SampleBytes);
    return fp;
}
std::string MediaFingerprint::canonical() const {
    std::ostringstream o; o << "SHA-256|scheme=1|size=" << file_size << '|';
    if(whole_file) o << "mode=whole|sha256=" << whole_sha256;
    else o << "mode=first-middle-last-1MiB|sample=" << SampleBytes << "|middleOffset=" << middle_offset
           << "|first=" << first_sha256 << "|middle=" << middle_sha256 << "|last=" << last_sha256;
    return o.str();
}
} // namespace bdmvauthor
