// SPDX-License-Identifier: Apache-2.0
#include "bdmvauthor/author.hpp"
#include "bdmvauthor/bluray_preflight.hpp"
#include "bdmvauthor/audio_tools.hpp"
#include "bdmvauthor/compliance.hpp"
#include "bdmvauthor/dvd_mux.hpp"
#include "bdmvauthor/dvd_subpicture_palette.hpp"
#include "bdmvauthor/hdmv.hpp"
#include "bdmvauthor/font_renderer.hpp"
#include "bdmvauthor/menu_image.hpp"
#include "bdmvauthor/progress.hpp"
#include "bdmvauthor/media_fingerprint.hpp"
#include "udf_writer.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <charconv>
#include <condition_variable>
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
#include <iostream>
#include <initializer_list>
#include <iterator>
#include <mutex>
#include <optional>
#include <regex>
#include <map>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
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

constexpr int kUhdHevcVbvBufsizeKbits = 100000;

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


enum class EncodeFailureKind { Transient, UnavailableCapability, Fatal };

class UnavailableEncodingCapability final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

void verify_encoding_worker_capability(const ToolPaths& tools, const std::string& codec);
void verify_remote_hardware_encoder_device(const ToolPaths& tools, const std::string& codec);

EncodeFailureKind classify_encode_failure(const std::string& message) {
    std::string lower=message;
    std::transform(lower.begin(),lower.end(),lower.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
    // Deterministic encoder/device failures must win over secondary pipe errors.
    // A remote FFmpeg that cannot create QSV/VAAPI may close stdin, causing the
    // local feeder to also report Broken pipe; retrying that identical worker is
    // not useful and previously misclassified the whole failure as transient.
    static constexpr std::array<const char*,21> unavailable_terms={
        "unknown encoder","encoder not found","failed to find encoder","no such encoder",
        "unknown decoder","decoder not found","command not found","not recognized as an internal or external command",
        "is not recognized as an internal or external command","executable file not found","not found in path",": not found",
        "device creation failed","failed to create a qsv device","failed to create a vaapi device",
        "failed to set value 'qsv=","failed to set value 'vaapi=","no va display found for device",
        "failed to initialise vaapi connection","failed to initialize vaapi connection","generic error in an external library"
    };
    for(const auto* term:unavailable_terms)if(lower.find(term)!=std::string::npos)return EncodeFailureKind::UnavailableCapability;
    static constexpr std::array<const char*,18> transient_terms={
        "connection reset","connection timed out","connection timeout","connection refused",
        "no route to host","network is unreachable","broken pipe","connection closed",
        "connection lost","temporary failure","temporarily unavailable","resource temporarily unavailable",
        "device or resource busy","device busy","input/output error","i/o error","timeout","timed out"
    };
    for(const auto* term:transient_terms)if(lower.find(term)!=std::string::npos)return EncodeFailureKind::Transient;
    return EncodeFailureKind::Fatal;
}

ToolPaths local_worker_tools(const ToolPaths& base) {
    ToolPaths t=base;
    t.remote_encoding=false;
    t.remote_servers.clear();
    return t;
}

ToolPaths remote_worker_tools(const ToolPaths& base,const RemoteEncodingServer& server) {
    ToolPaths t=base;
    t.remote_encoding=true;
    t.ssh=server.ssh.empty()?base.ssh:server.ssh;
    t.remote_host=server.host;
    t.remote_user=server.user;
    t.remote_ssh_port=server.port;
    t.remote_operating_system=server.operating_system;
    t.remote_identity_file=server.identity_file;
    t.remote_ffmpeg=server.ffmpeg.empty()?"ffmpeg":server.ffmpeg;
    if(t.remote_operating_system==RemoteOperatingSystem::Auto){
        std::string f=t.remote_ffmpeg;std::transform(f.begin(),f.end(),f.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
        t.remote_operating_system=(f.ends_with(".exe")||f.find('\\')!=std::string::npos)?RemoteOperatingSystem::Windows:RemoteOperatingSystem::Posix;
    }
    t.remote_x264=server.x264.empty()?"x264":server.x264;
    t.remote_x265=server.x265.empty()?"x265":server.x265;
    t.remote_vc1enc=server.vc1enc.empty()?"vc1enc":server.vc1enc;
    t.remote_hdmaenc=server.hdmaenc.empty()?"hdmaenc":server.hdmaenc;
    t.h264_provider=server.h264_provider;
    t.hevc_provider=server.hevc_provider;
    t.h264_hardware_encoder=server.h264_hardware_encoder;
    t.hevc_hardware_encoder=server.hevc_hardware_encoder;
    t.hardware_decoder=server.hardware_decoder;
    t.remote_hardware_device=server.hardware_device;
    t.filter_hardware_device=server.hardware_device;
    t.remote_servers.clear();
    return t;
}

class EncodingWorkerPool {
    struct Worker {
        std::string name;
        ToolPaths tools;
        int limit=1;
        int active=0;
        int consecutive_transient_failures=0;
        bool fallback_only=false;
        bool supports_h264=true;
        bool supports_hevc=true;
        bool supports_mpeg2=true;
        bool supports_vc1=true;
        bool supports_hdma=true;
        std::unordered_set<std::string> unavailable_codecs;
        std::unordered_set<std::string> verified_codecs;
    };
    std::vector<Worker> workers_;
    EncodingSchedulerSettings settings_;
    EncodeJobProgressCallback progress_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::size_t cursor_=0;

    static bool compatible(const Worker& w,const std::string& codec) {
        if(w.unavailable_codecs.contains(codec)) return false;
        if(codec=="h264-hardware") return !w.tools.h264_hardware_encoder.empty();
        if(codec=="hevc-hardware") return !w.tools.hevc_hardware_encoder.empty();
        if(codec=="vc1") return w.supports_vc1&&(!w.tools.remote_encoding||!w.tools.remote_vc1enc.empty());
        if(codec=="hdma") return w.supports_hdma&&(!w.tools.remote_encoding||!w.tools.remote_hdmaenc.empty());
        if(codec=="eac371") return !w.tools.remote_encoding && !w.tools.eac371enc.empty();
        if(codec=="hevc") return w.supports_hevc&&(!w.tools.remote_encoding||!w.tools.remote_ffmpeg.empty()||!w.tools.remote_x265.empty());
        if(codec=="x264") return w.supports_h264&&(!w.tools.remote_encoding||!w.tools.remote_ffmpeg.empty()||!w.tools.remote_x264.empty());
        if(codec=="mpeg2") return w.supports_mpeg2&&(!w.tools.remote_encoding||!w.tools.remote_ffmpeg.empty());
        if(codec.rfind("audio-",0)==0) return !w.tools.remote_encoding||!w.tools.remote_ffmpeg.empty();
        return true;
    }
    std::optional<std::size_t> named_worker(const std::string& name) const {
        if(name.empty())return std::nullopt;
        for(std::size_t i=0;i<workers_.size();++i)if(workers_[i].name==name)return i;
        return std::nullopt;
    }
    std::size_t acquire(const std::string& requested_worker,EncodingWorkerAffinityMode affinity_mode,
                        const std::optional<std::size_t>& retry_preferred,const std::optional<std::size_t>& avoid,
                        const std::string& codec) {
        std::unique_lock lock(mutex_);
        const auto requested=named_worker(requested_worker);
        if(affinity_mode==EncodingWorkerAffinityMode::Required){
            if(requested_worker.empty())throw std::runtime_error("required encoding worker is not specified for codec "+codec);
            if(!requested)throw std::runtime_error("required encoding worker '"+requested_worker+"' is not enabled");
            if(!compatible(workers_[*requested],codec))throw std::runtime_error("required encoding worker '"+requested_worker+"' does not support codec "+codec);
            cv_.wait(lock,[&]{return authoring_cancel_requested()||workers_[*requested].active<workers_[*requested].limit;});
            authoring_cancellation_point();++workers_[*requested].active;return *requested;
        }
        const auto has_normal_compatible=[&]{
            for(std::size_t i=0;i<workers_.size();++i){
                if(avoid&&i==*avoid)continue;
                if(!workers_[i].fallback_only&&compatible(workers_[i],codec))return true;
            }
            return false;
        };
        const bool normal_available=has_normal_compatible();
        const auto usable=[&](std::size_t i){
            if(avoid&&i==*avoid)return false;
            if(!compatible(workers_[i],codec))return false;
            if(workers_[i].fallback_only&&normal_available){
                // An explicitly preferred Local worker may still be used even
                // when Local is otherwise fallback-only.
                return affinity_mode==EncodingWorkerAffinityMode::Preferred&&requested&&i==*requested;
            }
            return true;
        };
        bool any=false;for(std::size_t i=0;i<workers_.size();++i)if(usable(i)){any=true;break;}
        if(!any){
            // A failover avoidance may be the only thing excluding a worker.
            // If no alternative exists, allow the failed worker again rather
            // than deadlocking forever.
            if(avoid){
                lock.unlock();
                return acquire(requested_worker,affinity_mode,retry_preferred,std::nullopt,codec);
            }
            throw std::runtime_error("no enabled encoding worker supports codec "+codec);
        }
        cv_.wait(lock,[&]{
            if(authoring_cancel_requested())return true;
            for(std::size_t i=0;i<workers_.size();++i)if(usable(i)&&workers_[i].active<workers_[i].limit)return true;
            return false;
        });
        authoring_cancellation_point();
        auto take=[&](std::size_t i){++workers_[i].active;return i;};
        if(retry_preferred&&*retry_preferred<workers_.size()&&usable(*retry_preferred)&&workers_[*retry_preferred].active<workers_[*retry_preferred].limit)
            return take(*retry_preferred);
        if(affinity_mode==EncodingWorkerAffinityMode::Preferred&&requested&&usable(*requested)&&workers_[*requested].active<workers_[*requested].limit)
            return take(*requested);
        for(std::size_t offset=0;offset<workers_.size();++offset){
            const std::size_t i=(cursor_+offset)%workers_.size();
            if(usable(i)&&workers_[i].active<workers_[i].limit){cursor_=(i+1)%workers_.size();return take(i);}
        }
        throw std::runtime_error("no encoding worker is available");
    }
    void release(std::size_t i,bool success,bool transient){
        {std::lock_guard lock(mutex_);auto& w=workers_.at(i);if(w.active>0)--w.active;if(success)w.consecutive_transient_failures=0;else if(transient)++w.consecutive_transient_failures;}
        cv_.notify_all();
    }
    int failures(std::size_t i){std::lock_guard lock(mutex_);return workers_.at(i).consecutive_transient_failures;}
    void mark_codec_unavailable(std::size_t i,const std::string& codec){
        std::lock_guard lock(mutex_);auto& w=workers_.at(i);
        // Remember the exact requested encoder path rather than disabling an
        // entire tool. A remote FFmpeg may lack TrueHD, for example, while
        // still being perfectly usable for AC-3 or LPCM.
        w.unavailable_codecs.insert(codec);
        w.verified_codecs.erase(codec);
    }
    bool codec_verified(std::size_t i,const std::string& codec){
        std::lock_guard lock(mutex_);
        return workers_.at(i).verified_codecs.contains(codec);
    }
    void mark_codec_verified(std::size_t i,const std::string& codec){
        std::lock_guard lock(mutex_);
        workers_.at(i).verified_codecs.insert(codec);
    }
    void emit(const EncodeJobProgress& p)const{if(progress_)progress_(p);}
public:
    EncodingWorkerPool(const ToolPaths& base,EncodeJobProgressCallback progress):settings_(base.scheduler),progress_(std::move(progress)){
        settings_.local_parallel_encodes=std::max(1,settings_.local_parallel_encodes);
        settings_.retry_delay_seconds=std::max(0,settings_.retry_delay_seconds);
        settings_.maximum_retries=std::max(0,settings_.maximum_retries);
        settings_.failover_after_consecutive_failures=std::max(1,settings_.failover_after_consecutive_failures);
        if(settings_.local_enabled||settings_.local_fallback_when_remote_unavailable)
            workers_.push_back({"Local",local_worker_tools(base),settings_.local_parallel_encodes,0,0,!settings_.local_enabled,true,true,true,true,true,{},{}});
        if(!base.remote_servers.empty()){
            for(const auto& server:base.remote_servers){
                if(!server.enabled||server.host.empty())continue;
                std::string name=server.name.empty()?server.host:server.name;
                workers_.push_back({name,remote_worker_tools(base,server),std::max(1,server.parallel_encodes),0,0,false,
                                    server.supports_h264,server.supports_hevc,server.supports_mpeg2,server.supports_vc1,server.supports_hdma,{},{}});
            }
        }else if(base.remote_encoding&&!base.remote_host.empty()){
            RemoteEncodingServer legacy;legacy.name=base.remote_host;legacy.ssh=base.ssh;legacy.host=base.remote_host;legacy.user=base.remote_user;legacy.port=base.remote_ssh_port;legacy.operating_system=base.remote_operating_system;legacy.identity_file=base.remote_identity_file;legacy.ffmpeg=base.remote_ffmpeg;legacy.x264=base.remote_x264;legacy.x265=base.remote_x265;legacy.vc1enc=base.remote_vc1enc;legacy.hdmaenc=base.remote_hdmaenc;legacy.h264_provider=base.h264_provider;legacy.hevc_provider=base.hevc_provider;legacy.h264_hardware_encoder=base.h264_hardware_encoder;legacy.hevc_hardware_encoder=base.hevc_hardware_encoder;legacy.hardware_decoder=base.hardware_decoder;legacy.hardware_device=base.remote_hardware_device;
            workers_.push_back({legacy.name,remote_worker_tools(base,legacy),1,0,0,false,true,true,true,true,true,{},{}});
        }
        if(workers_.empty())throw std::runtime_error("encoding is disabled: enable local encoding or at least one remote server");
    }
    std::size_t worker_count()const{return workers_.size();}
    int total_slots()const{int n=0;for(const auto&w:workers_)if(!w.fallback_only)n+=w.limit;return std::max(1,n);}

    template<class Function>
    auto run(const std::string& job,const std::string& codec,const std::string& requested_worker,
             EncodingWorkerAffinityMode affinity_mode,Function&& function)->decltype(function(std::declval<const ToolPaths&>(),std::declval<const std::string&>())){
        using Result=decltype(function(std::declval<const ToolPaths&>(),std::declval<const std::string&>()));
        if(requested_worker.empty())affinity_mode=EncodingWorkerAffinityMode::Automatic;
        int retries=0;std::optional<std::size_t> retry_preferred;std::optional<std::size_t> avoid;
        for(;;){
            const auto i=acquire(requested_worker,affinity_mode,retry_preferred,avoid,codec);std::string worker_name;ToolPaths tools;
            {std::lock_guard lock(mutex_);worker_name=workers_[i].name;tools=workers_[i].tools;}
            EncodeJobProgress start;start.worker=worker_name;start.job=job;start.codec=codec;start.status=retries?"retrying":"checking capability";start.attempt=retries+1;start.maximum_attempts=settings_.retry_forever?0:settings_.maximum_retries+1;emit(start);
            try{
                if(!codec_verified(i,codec)){
                    verify_encoding_worker_capability(tools,codec);
                    mark_codec_verified(i,codec);
                }
                start.status=retries?"retrying":"running";emit(start);
                if constexpr(std::is_void_v<Result>){function(tools,worker_name);release(i,true,false);start.status="completed";start.fraction=1.0;emit(start);return;}
                else{Result result=function(tools,worker_name);release(i,true,false);start.status="completed";start.fraction=1.0;emit(start);return result;}
            }catch(const AuthorCancelled&){release(i,false,false);throw;}
            catch(const UnavailableEncodingCapability& e){
                release(i,false,false);
                start.failure_type="permanent";
                start.diagnostic=e.what();
                if(affinity_mode==EncodingWorkerAffinityMode::Required){
                    start.status="required encoder/tool unavailable";emit(start);throw;
                }
                mark_codec_unavailable(i,codec);
                start.status="encoder/tool unavailable; trying another worker";emit(start);
                start.failure_type.clear();start.diagnostic.clear();
                retry_preferred.reset();avoid.reset();
                continue;
            }
            catch(const std::exception& e){
                const auto failure=classify_encode_failure(e.what());
                const bool transient=failure==EncodeFailureKind::Transient;
                release(i,false,transient);
                start.failure_type=transient?"transient":"permanent";
                start.diagnostic=e.what();
                if(failure==EncodeFailureKind::UnavailableCapability&&affinity_mode!=EncodingWorkerAffinityMode::Required){
                    mark_codec_unavailable(i,codec);
                    start.status="encoder unavailable; trying another worker";emit(start);
                    start.failure_type.clear();start.diagnostic.clear();
                    retry_preferred.reset();avoid.reset();
                    continue;
                }
                if(!transient){start.status="failed";emit(start);throw;}
                start.status="transient error";emit(start);
                start.failure_type.clear();start.diagnostic.clear();
                ++retries;const bool exhausted=!settings_.retry_forever&&retries>settings_.maximum_retries;
                if(exhausted){start.status="failed after retries";emit(start);throw;}
                const bool can_failover=affinity_mode!=EncodingWorkerAffinityMode::Required;
                const bool failover=can_failover&&failures(i)>=settings_.failover_after_consecutive_failures&&workers_.size()>1;
                if(failover){retry_preferred.reset();avoid=i;}else{retry_preferred=i;avoid.reset();}
                start.status=(failover?"retrying on another worker in ":"retrying in ")+std::to_string(settings_.retry_delay_seconds)+"s";
                start.attempt=retries+1;emit(start);
                for(int slept=0;slept<settings_.retry_delay_seconds;++slept){authoring_cancellation_point();std::this_thread::sleep_for(std::chrono::seconds(1));}
            }
        }
    }
    template<class Function>
    auto run(const std::string& job,const std::string& codec,Function&& function)->decltype(function(std::declval<const ToolPaths&>(),std::declval<const std::string&>())){
        return run(job,codec,{},EncodingWorkerAffinityMode::Automatic,std::forward<Function>(function));
    }
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
        case VideoCodec::H264Hardware: return "h264-hardware";
        case VideoCodec::Mpeg2: return "mpeg2";
        case VideoCodec::Hevc: return "hevc";
        case VideoCodec::HevcHardware: return "hevc-hardware";
        case VideoCodec::Vc1: return "vc1";
    }
    return "unknown";
}
std::string audio_codec_name(AudioCodec c) {
    switch(c){case AudioCodec::Ac3:return "ac3";case AudioCodec::Lpcm:return "lpcm";case AudioCodec::Dca:return "dca";case AudioCodec::TrueHdAc3:return "truehd-ac3";case AudioCodec::Libhdma:return "libhdma";}
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

fs::path default_temporary_root() {
    if (const char* override_dir = std::getenv("BDMVAUTHOR_TEMP_DIR"); override_dir && *override_dir)
        return fs::path(override_dir);
    std::error_code ec;
    const auto root = fs::temp_directory_path(ec);
    return ec ? fs::path{} : root;
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
        case DiscTarget::Avchd: return "AVCHD";
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
    const std::string location = tools.remote_encoding ? ("remote:" + tools.remote_host + ":" + std::to_string(tools.remote_ssh_port) + "/") : std::string{};
    const std::string provider = location + (e.video_codec == VideoCodec::X264
        ? (tools.h264_provider == VideoEncoderProvider::Ffmpeg ? "ffmpeg/libx264" : "x264")
        : e.video_codec == VideoCodec::H264Hardware
            ? ("ffmpeg/" + tools.h264_hardware_encoder)
        : e.video_codec == VideoCodec::Hevc
            ? (tools.hevc_provider == VideoEncoderProvider::Ffmpeg ? "ffmpeg/libx265" : "x265")
        : e.video_codec == VideoCodec::HevcHardware
            ? ("ffmpeg/" + tools.hevc_hardware_encoder)
            : e.video_codec == VideoCodec::Vc1 ? "vc1enc" : "ffmpeg");
    const std::string preset = video_codec_is_hardware(e.video_codec) ? "hardware-default"
        : e.video_codec == VideoCodec::Hevc ? e.x265_preset
        : e.video_codec == VideoCodec::Vc1 ? e.vc1_preset : e.x264_preset;
    settings << "provider=" << provider
             << "; preset=" << preset
             << "; two-pass=" << (e.two_pass ? "yes" : "no")
             << "; keyframe=" << e.keyframe_interval_frames
             << "; minrate-kbps=" << e.video_min_bitrate_kbps
             << "; maxrate-kbps=" << e.video_max_bitrate_kbps
             << "; peak-limit-kbps=" << peak_bitrate_limit_kbps
             << "; hw-decode=" << (tools.hardware_decoder.empty() ? "software" : tools.hardware_decoder)
             << "; scaling-filter=" << tools.scaling_filter_backend
             << "; tonemap-filter=" << tools.tonemap_filter_backend;
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

bool hardware_filter_requested(const ToolPaths& tools);
bool non_default_video_filter_requested(const ToolPaths& tools);

std::string video_cache_key_impl(const MediaFingerprint& fp,const EncodingProfile& e,const Project& p,
                                 const VideoTimingMode& timing, const ToolPaths& tools, int peak_bitrate_limit_kbps,
                                 bool remote_location_compat) {
    std::ostringstream k;k<<"bdmvauthor-video-cache-v14\n"<<fp.canonical()<<"\ncodec="<<video_codec_name(e.video_codec)
      <<"\nbitrate="<<e.video_bitrate_kbps<<"\nminrate="<<e.video_min_bitrate_kbps<<"\nmaxrate="<<e.video_max_bitrate_kbps<<"\nx264Preset="<<e.x264_preset<<"\nx265Preset="<<e.x265_preset<<"\nvc1Preset="<<e.vc1_preset<<"\ntwoPass="<<(e.two_pass?1:0)<<"\nfastFirstPass="<<(e.fast_first_pass?1:0)
      <<"\nframeTiming="<<timing_cache_text(timing)<<"\ntarget="<<static_cast<int>(p.target)
      <<"\nkeyframeInterval="<<e.keyframe_interval_frames
      <<"\nprovider=" << (e.video_codec==VideoCodec::X264
            ? (tools.h264_provider==VideoEncoderProvider::Ffmpeg?"ffmpeg":"standalone")
            : e.video_codec==VideoCodec::H264Hardware ? tools.h264_hardware_encoder
            : e.video_codec==VideoCodec::Hevc
                ? (tools.hevc_provider==VideoEncoderProvider::Ffmpeg?"ffmpeg":"standalone")
            : e.video_codec==VideoCodec::HevcHardware ? tools.hevc_hardware_encoder
                : e.video_codec==VideoCodec::Vc1 ? "vc1enc" : "ffmpeg");
    // 0.1.154 introduced encodingLocation without bumping the cache schema.
    // It is not part of encoded-media identity, so 0.1.162 restores the
    // pre-remote key as canonical.  The compatibility key below is lookup-only
    // so 0.1.154-0.1.160 local-token objects remain reusable too.
    if (remote_location_compat) k << "\nencodingLocation=local";
    k <<"\nhardwareDecode="<<(tools.hardware_decoder.empty()?"software":tools.hardware_decoder);
    if (non_default_video_filter_requested(tools)) k << "\nscalingFilter=" << tools.scaling_filter_backend
        << "\ntonemapFilter=" << tools.tonemap_filter_backend << "\nfilterDevice=" << tools.filter_hardware_device;
    k <<"\nadvancedVideo="<<advanced_video_options_for_codec(e)
      <<"\npeakBitrateLimitKbps="<<peak_bitrate_limit_kbps;
    const auto cache_geometry = target_geometry_for_timing(p.target, timing);
    k << "\nsize=" << cache_geometry.video_width << "x" << cache_geometry.video_height
      << "\naspect=" << timing.aspect_ratio << "\nsar=" << timing.sample_aspect_ratio;
    if (target_is_dvd(p.target)) k << "\ndvdRaster=multi-mode-v3";
    k << "\n";
    return sha256_hex(k.str());
}
std::string video_cache_key(const MediaFingerprint& fp,const EncodingProfile& e,const Project& p,
                            const VideoTimingMode& timing, const ToolPaths& tools, int peak_bitrate_limit_kbps) {
    return video_cache_key_impl(fp,e,p,timing,tools,peak_bitrate_limit_kbps,false);
}
std::string video_cache_key_remote_location_compat(const MediaFingerprint& fp,const EncodingProfile& e,const Project& p,
                                                   const VideoTimingMode& timing, const ToolPaths& tools, int peak_bitrate_limit_kbps) {
    return video_cache_key_impl(fp,e,p,timing,tools,peak_bitrate_limit_kbps,true);
}

std::string menu_video_cache_key_impl(const fs::path& input, const fs::path& overlay, const EncodingProfile& e,
                                      const Project& p, const VideoTimingMode& timing, const ToolPaths& tools,
                                      bool source_interlaced, bool still, double seconds, const Rgba& background_color,
                                      bool loop_media, int peak_bitrate_limit_kbps, bool remote_location_compat) {
    std::ostringstream k;
    k << "bdmvauthor-menu-video-cache-v5\n";
    if (input.empty()) k << "input=generated-color\n";
    else k << "input=" << fingerprint_media_file(input).canonical() << "\n";
    if (overlay.empty()) k << "overlay=none\n";
    else k << "overlay=" << fingerprint_media_file(overlay).canonical() << "\n";
    k << "codec=" << video_codec_name(e.video_codec)
      << "\nbitrate=" << e.video_bitrate_kbps
      << "\nminrate=" << e.video_min_bitrate_kbps << "\nmaxrate=" << e.video_max_bitrate_kbps
      << "\nx264Preset=" << e.x264_preset << "\nx265Preset=" << e.x265_preset << "\nvc1Preset=" << e.vc1_preset
      << "\ntwoPass=" << (e.two_pass ? 1 : 0) << "\nfastFirstPass=" << (e.fast_first_pass ? 1 : 0)
      << "\nframeTiming=" << timing_cache_text(timing)
      << "\ntarget=" << static_cast<int>(p.target)
      << "\nkeyframeInterval=" << e.keyframe_interval_frames
      << "\nprovider=" << (e.video_codec == VideoCodec::X264
            ? (tools.h264_provider == VideoEncoderProvider::Ffmpeg ? "ffmpeg" : "standalone")
            : e.video_codec == VideoCodec::H264Hardware ? tools.h264_hardware_encoder
            : e.video_codec == VideoCodec::Hevc
                ? (tools.hevc_provider == VideoEncoderProvider::Ffmpeg ? "ffmpeg" : "standalone")
            : e.video_codec == VideoCodec::HevcHardware ? tools.hevc_hardware_encoder
                : e.video_codec == VideoCodec::Vc1 ? "vc1enc" : "ffmpeg");
    if (remote_location_compat) k << "\nencodingLocation=local";
    k << "\nhardwareDecode=" << (tools.hardware_decoder.empty() ? "software" : tools.hardware_decoder);
    if (non_default_video_filter_requested(tools)) k << "\nscalingFilter=" << tools.scaling_filter_backend
        << "\ntonemapFilter=" << tools.tonemap_filter_backend << "\nfilterDevice=" << tools.filter_hardware_device;
    k << "\nadvancedVideo=" << advanced_video_options_for_codec(e)
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
std::string menu_video_cache_key(const fs::path& input, const fs::path& overlay, const EncodingProfile& e,
                                 const Project& p, const VideoTimingMode& timing, const ToolPaths& tools,
                                 bool source_interlaced, bool still, double seconds, const Rgba& background_color,
                                 bool loop_media, int peak_bitrate_limit_kbps) {
    return menu_video_cache_key_impl(input,overlay,e,p,timing,tools,source_interlaced,still,seconds,background_color,
                                     loop_media,peak_bitrate_limit_kbps,false);
}
std::string menu_video_cache_key_remote_location_compat(const fs::path& input, const fs::path& overlay, const EncodingProfile& e,
                                                        const Project& p, const VideoTimingMode& timing, const ToolPaths& tools,
                                                        bool source_interlaced, bool still, double seconds, const Rgba& background_color,
                                                        bool loop_media, int peak_bitrate_limit_kbps) {
    return menu_video_cache_key_impl(input,overlay,e,p,timing,tools,source_interlaced,still,seconds,background_color,
                                     loop_media,peak_bitrate_limit_kbps,true);
}

std::string audio_cache_key_impl(const MediaFingerprint& fp,const EncodingProfile& e,int source_ordinal,int output_channels,
                                 const ToolPaths& tools, bool remote_location_compat) {
    (void)tools; // Worker selection is deliberately independent of encoded-media identity.
    std::ostringstream k;k<<"bdmvauthor-audio-cache-v7\n"<<fp.canonical()<<"\ncodec="<<audio_codec_name(e.audio_codec)
      <<"\nsourceAudioOrdinal="<<source_ordinal<<"\noutputChannels="<<output_channels
      <<"\nac3Bitrate="<<e.ac3_bitrate_kbps<<"\ndcaBitrate="<<e.dca_bitrate_kbps
      <<"\nadvancedAudio="<<advanced_audio_options_for_codec(e)
      <<"\nlpcmSampleRate="<<e.lpcm_sample_rate_hz<<"\nlpcmBitDepth="<<e.lpcm_bit_depth;
    if(e.audio_codec==AudioCodec::TrueHdAc3)k<<"\nac3CoreAdvanced="<<e.ac3_advanced_options;
    if(e.audio_codec==AudioCodec::Libhdma){
        k<<"\nhdmaAdvanced="<<e.hdma_advanced_options<<"\nhdmaVersion=0.0.37-xll-decorrelation-on-default";
        if (remote_location_compat) k<<"\nhdmaLocation=local";
    }
    k<<"\nfixedCodecSampleRate="<<(e.audio_codec==AudioCodec::Lpcm || e.audio_codec==AudioCodec::Libhdma?e.lpcm_sample_rate_hz:48000)<<"\n";
    return sha256_hex(k.str());
}
std::string audio_cache_key(const MediaFingerprint& fp,const EncodingProfile& e,int source_ordinal,int output_channels,
                            const ToolPaths& tools) {
    return audio_cache_key_impl(fp,e,source_ordinal,output_channels,tools,false);
}
std::string audio_cache_key_remote_location_compat(const MediaFingerprint& fp,const EncodingProfile& e,int source_ordinal,int output_channels,
                                                   const ToolPaths& tools) {
    return audio_cache_key_impl(fp,e,source_ordinal,output_channels,tools,true);
}

std::string menu_audio_cache_key_impl(const MediaFingerprint& fp, const EncodingProfile& e, DiscTarget target,
                                      double seconds, bool dvd_elementary, bool loop_media, const ToolPaths& tools,
                                      bool remote_location_compat) {
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
        k << "base=" << audio_cache_key_impl(fp, e, 0, 0, tools, remote_location_compat) << "\n";
    }
    return sha256_hex(k.str());
}
std::string menu_audio_cache_key(const MediaFingerprint& fp, const EncodingProfile& e, DiscTarget target,
                                 double seconds, bool dvd_elementary, bool loop_media, const ToolPaths& tools) {
    return menu_audio_cache_key_impl(fp,e,target,seconds,dvd_elementary,loop_media,tools,false);
}
std::string menu_audio_cache_key_remote_location_compat(const MediaFingerprint& fp, const EncodingProfile& e, DiscTarget target,
                                                        double seconds, bool dvd_elementary, bool loop_media, const ToolPaths& tools) {
    return menu_audio_cache_key_impl(fp,e,target,seconds,dvd_elementary,loop_media,tools,true);
}

std::string video_extension(const EncodingProfile& e){if(video_codec_is_h264(e.video_codec))return ".264";if(video_codec_is_hevc(e.video_codec))return ".265";if(e.video_codec==VideoCodec::Vc1)return ".vc1";return ".m2v";}
std::string audio_extension(const EncodingProfile& e){
    switch(e.audio_codec){case AudioCodec::Ac3:return ".ac3";case AudioCodec::Lpcm:return ".wav";case AudioCodec::Dca:return ".dts";case AudioCodec::TrueHdAc3:return ".thd+ac3";case AudioCodec::Libhdma:return ".dts";}
    return ".audio";
}
std::string video_meta_for(const fs::path& path,const EncodingProfile& e,const VideoTimingMode& timing){
    const std::string ar = ", ar=" + timing.aspect_ratio;
    if(video_codec_is_h264(e.video_codec)) return "V_MPEG4/ISO/AVC, "+shq(path)+", fps="+timing.tsmuxer_fps+ar+", insertSEI, contSPS\n";
    if(video_codec_is_hevc(e.video_codec)) return "V_MPEGH/ISO/HEVC, "+shq(path)+", fps="+timing.tsmuxer_fps+ar+"\n";
    // Raw VC-1 has no container timestamps. Pass the authoring timing and
    // display aspect explicitly so tsMuxer does not have to guess them from
    // a containerless elementary stream.
    if(e.video_codec==VideoCodec::Vc1) return "V_MS/VFW/WVC1, "+shq(path)+", fps="+timing.tsmuxer_fps+ar+"\n";
    return "V_MPEG-2, "+shq(path)+", fps="+timing.tsmuxer_fps+ar+"\n";
}
std::string audio_meta_for(const fs::path& path,const EncodingProfile& e,const std::string& lang){
    std::string type="A_AC3"; if(e.audio_codec==AudioCodec::Lpcm)type="A_LPCM";else if(e.audio_codec==AudioCodec::Dca || e.audio_codec==AudioCodec::Libhdma)type="A_DTS";
    return type+", "+shq(path)+", lang="+lang+"\n";
}
int configured_audio_rate_kbps(const EncodingProfile& e, DiscTarget target, int channels) {
    channels = std::max(1, channels);
    switch (e.audio_codec) {
        case AudioCodec::Lpcm:
            return lpcm_bitrate_kbps(e.lpcm_sample_rate_hz, e.lpcm_bit_depth,
                                     std::min(channels, lpcm_max_channels_for_target(target, e.lpcm_sample_rate_hz)));
        case AudioCodec::Dca: return e.dca_bitrate_kbps;
        case AudioCodec::Libhdma: return 0; // VBR lossless: measure actual encoded peak
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
        case AudioCodec::Libhdma: return 0; // measured after the lossless encode
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
    // only as far as the DVD-Video 9.8 Mb/s video limit and the payload rate
    // that mplex can packetize inside the 10.08 Mb/s program-stream ceiling.
    // Merely subtracting audio from 10.08 Mb/s is insufficient: mplex reserves
    // about 2.05% for program-stream packetization and reports AC-3 nominal
    // rates in binary kbit/s.
    constexpr int practical_peak_kbps = 9000;
    const int codec_peak_kbps = std::min(
        video_codec_max_bitrate_kbps(DiscTarget::DvdVideo480p, VideoCodec::Mpeg2), requested_maxrate_kbps);
    const int spec_peak_kbps = dvd_mplex_safe_video_bitrate_kbps(
        target_max_combined_av_bitrate_kbps(DiscTarget::DvdVideo480p),
        std::max(0, aggregate_audio_kbps), codec_peak_kbps);
    const int preferred_peak_kbps = std::min(practical_peak_kbps, spec_peak_kbps);
    const int peak_kbps = average_video_kbps < preferred_peak_kbps ? preferred_peak_kbps : spec_peak_kbps;
    if (average_video_kbps > peak_kbps)
        throw std::runtime_error(where + " average video bitrate of " + std::to_string(average_video_kbps) +
            " kb/s exceeds the DVD peak/maxrate of " + std::to_string(peak_kbps) +
            " kb/s after reserving " + std::to_string(std::max(0, aggregate_audio_kbps)) +
            " kb/s for audio plus mplex program-stream packetization overhead; "
            "reduce the configured video/audio bitrate or maxrate");
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

bool cache_file_usable(const fs::path& cached);

int cached_audio_peak_kbps(const ToolPaths& tools, const fs::path& cached, int fixed_peak_kbps,
                           const fs::path& probe_output, CacheMetadata metadata) {
    if (!cache_file_usable(cached)) return 0;
    if (fixed_peak_kbps > 0) return fixed_peak_kbps;
    int peak = cache_peak_bitrate_kbps(cached);
    if (peak > 0) return peak;
    // Older cache objects may predate peakBitrateKbps metadata.  Probe the
    // already-encoded elementary stream locally and repair metadata instead of
    // needlessly sending the source to an encoding worker again.
    try {
        peak = probe_elementary_audio_peak_kbps(tools, cached, probe_output);
    } catch (...) {
        return 0;
    }
    if (peak > 0) {
        metadata.peak_bitrate_kbps = peak;
        touch_cache_metadata(cached, metadata);
    }
    return peak;
}

int max_audio_channels_for_encoding(const EncodingProfile& e, DiscTarget target) {
    if (e.audio_codec == AudioCodec::Lpcm || e.audio_codec == AudioCodec::Libhdma)
        return lpcm_max_channels_for_target(target, e.lpcm_sample_rate_hz);
#if defined(BDMVAUTHOR_HAVE_EAC371ENC)
    if (e.audio_codec == AudioCodec::Ac3 && target_supports_ac3_71_extension(target))
        return 8;
#endif
    return 6;
}

int resolved_audio_output_channels(int source_channels, const EncodingProfile& e, DiscTarget target,
                                   int requested_channels, const std::string& where) {
    source_channels = std::max(1, source_channels);
    const int maximum = max_audio_channels_for_encoding(e, target);
#if defined(BDMVAUTHOR_HAVE_EAC371ENC)
    const bool ac3_71 = e.audio_codec == AudioCodec::Ac3 && maximum >= 8;
    if (requested_channels <= 0)
        return ac3_71 && source_channels >= 8 ? 8 : std::min(source_channels, std::min(maximum, 6));
#else
    if (requested_channels <= 0) return std::min(source_channels, maximum);
#endif
    if (requested_channels > source_channels)
        throw std::runtime_error(where + " requests " + std::to_string(requested_channels) +
                                 " output channels from a " + std::to_string(source_channels) +
                                 "-channel source; BDMV Author does not upmix");
    if (requested_channels > maximum)
        throw std::runtime_error(where + " requests " + std::to_string(requested_channels) +
                                 " channels but the selected codec/target supports at most " + std::to_string(maximum));
#if defined(BDMVAUTHOR_HAVE_EAC371ENC)
    if (ac3_71 && requested_channels == 7)
        throw std::runtime_error(where + " AC-3 output supports either up to 5.1 or the 7.1 core+extension format; 7.0/6.1 output is not supported");
#endif
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

fs::path compatible_encoded_cache_hit(const fs::path& canonical, const fs::path& remote_location_compat) {
    if (cache_file_usable(canonical)) return canonical;
    if (!remote_location_compat.empty() && remote_location_compat != canonical && cache_file_usable(remote_location_compat))
        return remote_location_compat;
    return {};
}

struct EncodedCachePaths {
    fs::path write;
    fs::path hit;
};

EncodedCachePaths encoded_cache_paths(const fs::path& root, const fs::path& category,
                                       const std::string& canonical_key, const std::string& remote_location_compat_key,
                                       const std::string& extension, bool allow_lookup) {
    EncodedCachePaths paths;
    if (root.empty()) return paths;
    paths.write = root / category / (canonical_key + extension);
    if (!allow_lookup) return paths;
    const fs::path compatibility = root / category / (remote_location_compat_key + extension);
    paths.hit = compatible_encoded_cache_hit(paths.write, compatibility);
    return paths;
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

// A software retry must not be published under a hardware-filter cache key.
// Retain the completed output in the job work directory rather than the
// ephemeral cache staging directory, and try the configured GPU again next time.
fs::path fallback_filter_marker(const fs::path& output_base) {
    return fs::path(output_base.string() + ".filter-software-fallback");
}

fs::path preserve_software_filter_fallback(const fs::path& staged, const fs::path& final_base,
                                          const std::string& extension) {
    const fs::path final = fs::path(final_base.string() + extension);
    std::error_code ec;
    fs::create_directories(final.parent_path(), ec);
    if (ec) throw std::runtime_error("cannot create software-filter fallback output directory: " + final.parent_path().string());
    fs::copy_file(staged, final, fs::copy_options::overwrite_existing, ec);
    if (ec) throw std::runtime_error("cannot preserve software-filter fallback output: " + final.string() + ": " + ec.message());
    return final;
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
    bool keep_work_directory = false;
    ~RenderTempCleanup() {
        // Cancellation, an FFmpeg/spumux/dvdauthor failure, and exceptions must
        // have the same cleanup semantics as a successful authoring run.  The
        // explicitly selected --keep-work/--work mode remains an opt-in escape.
        std::error_code ec;
        if (!temporary_image.empty()) fs::remove(temporary_image, ec);
        if (!keep_work_directory && !work_dir.empty()) {
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

std::string read_all_text(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::ostringstream out;
    out << f.rdbuf();
    return out.str();
}

std::string read_small_text(const fs::path& p) {
    auto x = read_all_text(p);
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
    if (!fs::exists(src)) throw std::runtime_error("required authored file is missing: " + src.string());
    fs::create_directories(dst.parent_path());
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing);
}

void move_one(const fs::path& src, const fs::path& dst) {
    if (!fs::exists(src)) throw std::runtime_error("tsMuxeR did not produce " + src.string());
    fs::create_directories(dst.parent_path());
    std::error_code ec;
    if (fs::exists(dst, ec)) { ec.clear(); fs::remove(dst, ec); }
    if (ec) throw std::runtime_error("cannot replace authored destination: " + dst.string());
    fs::rename(src, dst, ec);
    if (ec) throw std::runtime_error("cannot move authored file into the canonical work tree: " + src.string() + " -> " + dst.string());
}

std::string bd_number(unsigned n) {
    std::ostringstream name;
    name << std::setw(5) << std::setfill('0') << n;
    return name.str();
}

void move_playlist_clips(const fs::path& from, const fs::path& to, unsigned playlist,
                         std::initializer_list<unsigned> clips) {
    // tsMuxer authors each menu/title into a private subtree. Move its final
    // BDMV payload into the canonical disc tree instead of copying it, so a
    // second full-sized temporary copy never exists. Both paths are under the
    // same per-run work root, making rename the intended operation.
    for (const auto clip : clips) {
        const auto x = bd_number(clip);
        move_one(from / "BDMV/STREAM" / (x + ".m2ts"), to / "BDMV/STREAM" / (x + ".m2ts"));
        move_one(from / "BDMV/CLIPINF" / (x + ".clpi"), to / "BDMV/CLIPINF" / (x + ".clpi"));
    }
    const auto p = bd_number(playlist);
    move_one(from / "BDMV/PLAYLIST" / (p + ".mpls"), to / "BDMV/PLAYLIST" / (p + ".mpls"));
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
    // Absolute FFmpeg input-stream index captured by ffprobe. Persisted project
    // settings remain keyed by media-type ordinal, but commands use this exact
    // index when available so unusual MPEG-TS/M2TS layouts cannot make
    // `0:a:N` resolve to a different/nonexistent track.
    int input_stream_index = -1;
};

struct SourceTrackCatalog {
    std::vector<SourceTrackDescriptor> audio;
    std::vector<SourceTrackDescriptor> subtitles;
};

std::string ffmpeg_audio_map_spec(int ordinal, int input_stream_index) {
    return input_stream_index >= 0 ? "0:" + std::to_string(input_stream_index)
                                   : "0:a:" + std::to_string(ordinal);
}
std::string ffprobe_audio_stream_spec(int ordinal, int input_stream_index) {
    return input_stream_index >= 0 ? std::to_string(input_stream_index)
                                   : "a:" + std::to_string(ordinal);
}

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
    return codec == "hdmv_pgs_subtitle" || codec == "dvd_subtitle" ||
           codec == "hddvd_subtitle" || codec == "external_text" ||
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
        " -v error -show_entries stream=index,codec_type,codec_name:stream_tags=language "
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
        int input_stream_index = -1;
        std::size_t start = 0;
        while (start <= line.size()) {
            const auto end = line.find('|', start);
            const auto token = line.substr(start, end == std::string::npos ? std::string::npos : end - start);
            const auto eq = token.find('=');
            if (eq != std::string::npos) {
                const auto key = token.substr(0, eq);
                const auto value = token.substr(eq + 1);
                if (key == "index") {
                    std::istringstream index_value(value);
                    int parsed_index = -1;
                    if ((index_value >> parsed_index) && index_value.peek() == std::char_traits<char>::eof())
                        input_stream_index = parsed_index;
                } else if (key == "codec_type") type = value;
                else if (key == "codec_name") codec = value;
                else if (key == "tag:language" || key == "TAG:language") language = value;
            }
            if (end == std::string::npos) break;
            start = end + 1;
        }
        if (type == "audio") {
            SourceTrackDescriptor track{audio_ordinal++, codec, normalized_language(language), {}, {}};
            track.input_stream_index = input_stream_index;
            catalog.audio.push_back(std::move(track));
        } else if (type == "subtitle") {
            SourceTrackDescriptor track{subtitle_ordinal++, codec, normalized_language(language), {}, {}};
            track.input_stream_index = input_stream_index;
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
    if (target == DiscTarget::BluRay1080 || target == DiscTarget::Avchd) return {
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
    if (target == DiscTarget::UltraHdBluRay2160 && video_codec_is_h264(codec)) {
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
    if (video_codec_is_h264(codec)) return explicit_rate_in(kBluRay1080AvcTimingModes, v);
    return explicit_rate_in(kBluRay1080TimingModes, v);
}

VideoTimingMode timing_for_raster(DiscTarget target, int width, int height, const std::string& requested_rate,
                                  double source_rate, bool source_interlaced, VideoCodec codec) {
    const auto v = normalize_frame_rate_for_target(target, requested_rate);
    const bool automatic = v.empty() || v == "auto";
    if (target == DiscTarget::UltraHdBluRay2160) {
        if (video_codec_is_h264(codec)) {
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
        if (video_codec_is_h264(codec)) {
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
                                                      int ordinal, double duration_seconds, int input_stream_index = -1) {
    double maximum = 0.0;
    bool saw = false;
    for (const auto& sample : compliance_sample_windows(duration_seconds)) {
        std::ostringstream command;
        command << shq_s(tools.ffmpeg) << " -hide_banner -loglevel error -ss "
                << std::fixed << std::setprecision(3) << sample.start_seconds
                << " -i " << shq(source) << " -t 0.250 -map " << ffmpeg_audio_map_spec(ordinal, input_stream_index)
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
                                                int ordinal, double duration_seconds, int input_stream_index = -1) {
    double max_average_bps = 0.0;
    bool saw_rate = false;
    for (const auto& sample : compliance_sample_windows(duration_seconds)) {
        std::ostringstream command;
        command << shq_s(tools.ffmpeg) << " -hide_banner -loglevel error -ss "
                << std::fixed << std::setprecision(3) << sample.start_seconds
                << " -i " << shq(source) << " -t " << sample.length_seconds
                << " -map " << ffmpeg_audio_map_spec(ordinal, input_stream_index) << " -vn -sn -dn -c:a copy -f dts pipe:1 2>" << null_device();
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

AudioStreamInfo probe_audio_stream(const ToolPaths& tools, const fs::path& source, const fs::path& out, int ordinal, double duration_seconds,
                                   int input_stream_index = -1) {
    AudioStreamInfo info;
    fs::create_directories(out);
    const auto stream_file = out / "audio-stream.txt";
    const std::string probe = shq_s(tools.ffprobe) +
        " -v error -select_streams " + ffprobe_audio_stream_spec(ordinal, input_stream_index) + " -show_entries "
        "stream=codec_name,profile,sample_fmt,sample_rate,channels,channel_layout,bit_rate,bits_per_raw_sample,bits_per_sample "
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
    info.channel_layout = text("channel_layout");
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
        const auto core_rate = probe_dts_core_bitrate_sampled(tools, source, ordinal, duration_seconds, input_stream_index);
        if (core_rate) info.dts_core_bitrate_bps = *core_rate;
        const bool hd = info.profile.find("DTS-HD") != std::string::npos;
        if (hd) {
            const double measured = probe_audio_elementary_bitrate_sampled(tools, source, ordinal, duration_seconds, input_stream_index);
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
        auto info = probe_audio_stream(tools, source, out / ("audio-" + std::to_string(track.source_ordinal + 1)), track.source_ordinal, duration_seconds,
                                       track.input_stream_index);
        analysis.audio.push_back({track, info, evaluate_blu_ray_audio(info, target)});
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
                                  const BluRayStreamDecision& decision, const std::string& language, int ordinal,
                                  int input_stream_index = -1) {
    fs::create_directories(dir);
    const fs::path out = dir / (std::string("audio") + decision.elementary_extension);
    std::ostringstream command;
    command << shq_s(tools.ffmpeg) << " -hide_banner -loglevel error -y -i " << shq(source)
            << " -map " << ffmpeg_audio_map_spec(ordinal, input_stream_index) << " -vn -sn -dn -c:a copy ";
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

bool is_bitmap_subtitle(const std::string& codec);

std::string subtitle_tsmuxer_style_meta(const SourceTrackDescriptor& track, int subtitle_plane_height) {
    const auto& st=track.subtitle_style;
    const int bottom_offset = subtitle_units_to_pixels(st.bottom_offset_px, subtitle_plane_height);
    if(is_bitmap_subtitle(track.codec_name)) {
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

// Keep this deliberately bitmap-based: OCR cannot reproduce colors, positioning,
// non-text signs, or forced flags. BDSup2Sub is an optional, separately supplied
// open-source program, rather than embedding an incompatible licensed binary.
bool is_bitmap_subtitle(const std::string& codec) {
    return codec == "hdmv_pgs_subtitle" || codec == "hddvd_subtitle" || codec == "dvd_subtitle";
}

std::string lowercase_extension(const fs::path& file) {
    auto ext = file.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

std::string external_subtitle_codec(const fs::path& file) {
    const auto ext = lowercase_extension(file);
    if (ext == ".idx" || ext == ".sub") return "dvd_subtitle";
    if (ext != ".sup" && ext != ".pgs") return "external_text";
    std::ifstream input(file, std::ios::binary);
    unsigned char magic[2]{};
    if (!input.read(reinterpret_cast<char*>(magic), 2))
        throw std::runtime_error("cannot inspect bitmap subtitle signature: " + file.string());
    if (magic[0] == 'P' && magic[1] == 'G') return "hdmv_pgs_subtitle";
    if (magic[0] == 'S' && magic[1] == 'P') return "hddvd_subtitle";
    throw std::runtime_error("unrecognized Blu-ray/HD-DVD SUP subtitle signature in " + file.string());
}

fs::path normalized_bitmap_input(const ToolPaths& tools, const fs::path& title_source,
                                 const SourceTrackDescriptor& track, const fs::path& dir) {
    if (!track.external_source.empty()) {
        const auto src = track.external_source;
        if (track.codec_name == "dvd_subtitle") {
            const auto idx = lowercase_extension(src) == ".sub" ? fs::path(src).replace_extension(".idx") : src;
            if (!fs::is_regular_file(idx) || !fs::is_regular_file(fs::path(idx).replace_extension(".sub")))
                throw std::runtime_error("DVD VobSub conversion requires matching .idx and .sub files: " + src.string());
            return idx;
        }
        return src;
    }
    if (track.codec_name == "dvd_subtitle")
        throw std::runtime_error("Embedded DVD bitmap subtitle stream " + std::to_string(track.source_ordinal + 1) +
            " cannot yet be demuxed to VobSub by FFmpeg; attach its .idx/.sub files instead");
    // FFmpeg supports demuxing PGS into .sup, but cannot decode HD-DVD's SPU.
    if (track.codec_name == "hddvd_subtitle")
        throw std::runtime_error("Embedded HD-DVD subtitles require a demuxed HD-DVD .sup attachment");
    const auto out = dir / "source-pgs.sup";
    const auto stream = track.input_stream_index >= 0
        ? "0:" + std::to_string(track.input_stream_index)
        : "0:s:" + std::to_string(track.source_ordinal);
    run(shq_s(tools.ffmpeg) + " -hide_banner -loglevel error -y -i " + shq(title_source) +
        " -map " + stream + " -vn -an -dn -c:s copy -f sup " + shq(out));
    if (!fs::exists(out) || fs::file_size(out) == 0)
        throw std::runtime_error("PGS subtitle extraction produced no images: " + title_source.string());
    return out;
}

void convert_bitmap_with_bdsup2sub(const ToolPaths& tools, const fs::path& source, const fs::path& output,
                                   int target_height, const std::string& target_fps = {}) {
    fs::create_directories(output.parent_path());
    std::string resolution;
    switch (target_height) {
        case 480: resolution = "ntsc"; break;
        case 576: resolution = "pal"; break;
        case 720: resolution = "720p"; break;
        default: resolution = "1080p"; break;
    }
    // bdsup2sub-native uses the historical BDSup2Sub CLI option spellings.
    // Its native binary may be on PATH, beside BDMV Author, or configured.
    const auto command = tools.bdsup2sub.empty() ? std::string("bdsup2sub") : tools.bdsup2sub;
    auto args = shq_s(command) + " " + shq(source) + " " + shq(output) +
        " /res:" + resolution + " /fps:" + (target_fps.empty() ? "keep" : target_fps) +
        " /tmin:0 /tmerge:0 /forced-";
    try {
        run(args);
    } catch (const std::exception& e) {
        throw std::runtime_error("Native bitmap subtitle conversion failed (install the optional "
            "bdsup2sub utility or supply third_party/bdsup2sub at CMake configure time): " +
            std::string(e.what()));
    }
    if (!fs::is_regular_file(output) || fs::file_size(output) == 0)
        throw std::runtime_error("Native bdsup2sub produced no subtitles at " + output.string());
}

std::string prepare_subtitle(const ToolPaths& tools, const fs::path& source, const fs::path& dir,
                             const SourceTrackDescriptor& track, int width, int height, double fps) {
    fs::create_directories(dir);
    const bool pgs = is_bitmap_subtitle(track.codec_name);
    const fs::path out = dir / (pgs ? "subtitle.sup" : "subtitle.srt");
    if (pgs) {
        const auto input = normalized_bitmap_input(tools, source, track, dir);
        if (track.codec_name == "hdmv_pgs_subtitle" && height == 1080) {
            // Do not re-encode original BD PGS when its graphics plane matches.
            fs::copy_file(input, out, fs::copy_options::overwrite_existing);
        } else {
            convert_bitmap_with_bdsup2sub(tools, input, out, height);
        }
    } else {
        const fs::path subtitle_source = track.external_source.empty() ? source : track.external_source;
        const auto stream = track.external_source.empty()
            ? (track.input_stream_index >= 0 ? "0:" + std::to_string(track.input_stream_index)
                                           : "0:s:" + std::to_string(track.source_ordinal))
            : "0:s:0";
        run(shq_s(tools.ffmpeg) + " -hide_banner -loglevel error -y -i " + shq(subtitle_source) +
            " -map " + stream + " -vn -an -dn -c:s srt -f srt " + shq(out));
    }

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

using VideoProgressCallback = std::function<void(double, int, int, double, double)>;

void run_with_ffmpeg_progress(const std::string& command, const fs::path& progress_file,
                              double duration_seconds, const std::function<void(double)>& progress,
                              const std::function<void(double,double,double)>& detailed_progress = {}) {
    std::error_code ec;
    fs::remove(progress_file, ec);
    const fs::path stderr_file = progress_file.string() + ".stderr";
    fs::remove(stderr_file, ec);
    const auto guarded_command = fail_on_interrupt_command("(" + command + ") 2> " + shq(stderr_file));
    auto process = std::async(std::launch::async, [guarded_command] { return shell_system(guarded_command); });
    double last_fraction = -1.0;
    while (process.wait_for(std::chrono::milliseconds(125)) != std::future_status::ready) {
        if ((!progress && !detailed_progress) || duration_seconds <= 0.0) continue;
        const auto snapshot = detail::read_ffmpeg_progress_snapshot(progress_file);
        if (!snapshot) continue;
        const double fraction = std::min(detail::progress_fraction(snapshot->encoded_seconds, duration_seconds), 0.999);
        if (fraction > last_fraction + 0.0001) {
            last_fraction = fraction;
            if (progress) progress(fraction);
            if (detailed_progress) detailed_progress(fraction, snapshot->fps, snapshot->speed);
        }
    }
    const int rc = process.get();
    if (rc != 0) {
        const auto diagnostic = read_all_text(stderr_file);
        fs::remove(stderr_file, ec);
        std::string message = "command failed (" + command_status(rc) + "): " + command;
        if (!diagnostic.empty()) message += "\nFFmpeg: " + diagnostic;
        throw std::runtime_error(message);
    }
    fs::remove(stderr_file, ec);
    if (progress) progress(1.0);
    if (detailed_progress) detailed_progress(1.0, 0.0, 0.0);
}

int audio_channels(const ToolPaths& tools, const fs::path& source, const fs::path& tmp, int ordinal, int input_stream_index = -1) {
    // Several callers deliberately pass a per-purpose nested probe directory
    // (for example menu-1/audio-rate).  Shell redirection opens the output
    // file before ffprobe starts, so the directory must exist first.
    fs::create_directories(tmp);
    auto out = tmp / "probe-channels.txt";
    std::string c = shq_s(tools.ffprobe) +
                    " -v error -select_streams " + ffprobe_audio_stream_spec(ordinal, input_stream_index) +
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

bool valid_vc1_preset(const std::string& preset) {
    static constexpr std::array<const char*, 2> presets = {"normal", "fast"};
    return std::any_of(presets.begin(), presets.end(), [&](const char* p) { return preset == p; });
}

void validate_encoding(const EncodingProfile& e, const char* where, bool allow_exceeding_format_limits, DiscTarget target) {
    if (e.keyframe_interval_frames < 0)
        throw std::runtime_error(std::string(where) + " keyframe/GOP interval must be zero (default) or a positive frame count");
    const int max_video_bitrate = video_codec_is_hevc(e.video_codec) ? 86000 : 40000;
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
    if (!valid_vc1_preset(e.vc1_preset))
        throw std::runtime_error(std::string(where) + " has an invalid vc1enc preset: " + e.vc1_preset);
    if (video_codec_is_hardware(e.video_codec) && e.two_pass)
        throw std::runtime_error(std::string(where) + " hardware H.264/H.265 encoding uses the selected FFmpeg hardware encoder's native rate control and does not support BDMV Author two-pass mode");
    if (e.video_codec == VideoCodec::Vc1 && e.video_min_bitrate_kbps != 0)
        throw std::runtime_error(std::string(where) + " VC-1 currently requires video minrate 0 because vc1enc does not expose a separate minrate control");
    const int minimum_ac3_bitrate_kbps = allow_exceeding_format_limits ? 1 : (target == DiscTarget::Avchd ? 64 : 192);
    int maximum_ac3_bitrate_kbps = 640;
#if defined(BDMVAUTHOR_HAVE_EAC371ENC)
    if (target_supports_ac3_71_extension(target))
        maximum_ac3_bitrate_kbps = target_max_ac3_71_bitrate_kbps(target);
#endif
    if (e.ac3_bitrate_kbps < minimum_ac3_bitrate_kbps ||
        (!allow_exceeding_format_limits && e.ac3_bitrate_kbps > maximum_ac3_bitrate_kbps))
        throw std::runtime_error(std::string(where) + " AC-3 bitrate is outside the permitted range");
    if (e.dca_bitrate_kbps < (allow_exceeding_format_limits ? 1 : 384) ||
        (!allow_exceeding_format_limits && e.dca_bitrate_kbps > 1536))
        throw std::runtime_error(std::string(where) + " DCA bitrate is outside the permitted range");
    if (!lossless_bit_depth_supported_by_encoder(e.lpcm_bit_depth))
        throw std::runtime_error(std::string(where) + " lossless audio bit depth must be Auto, 16 or 24 bits");
}

void validate_encoding_for_target(const EncodingProfile& e, DiscTarget target, const std::string& where,
                                  bool allow_exceeding_format_limits = false) {
    validate_encoding(e, where.c_str(), allow_exceeding_format_limits, target);
    if (!video_codec_allowed_for_target(target, e.video_codec)) {
        if (target == DiscTarget::UltraHdBluRay2160)
            throw std::runtime_error(where + " must use HEVC/H.265 or AVC/H.264 video for Ultra HD Blu-ray");
        if (target == DiscTarget::Avchd)
            throw std::runtime_error(where + " must use AVC/H.264 video for AVCHD");
        if (target == DiscTarget::DvdVideo480p)
            throw std::runtime_error(where + " must use MPEG-2 video for DVD-Video");
        throw std::runtime_error(where + " must use H.264/AVC, MPEG-2, or experimental libvc1 VC-1 video for standard Blu-ray");
    }
    if (!audio_codec_allowed_for_target(target, e.audio_codec)) {
        if (target == DiscTarget::Avchd)
            throw std::runtime_error(where + " AVCHD audio must use Dolby Digital AC-3 or Linear PCM");
        if (target == DiscTarget::DvdVideo480p)
            throw std::runtime_error(where + " DVD-Video audio must use AC-3 or LPCM; DCA, THD, and lossless libhdma are not supported by BDMV Author's DVD target");
        throw std::runtime_error(where + " audio codec is not supported by the selected output target");
    }
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
    if (!allow_exceeding_format_limits && target == DiscTarget::Avchd && e.audio_codec == AudioCodec::Lpcm && e.lpcm_bit_depth != 16)
        throw std::runtime_error(where + " AVCHD Linear PCM must use 16-bit samples in the conservative optical-disc profile");
    try { validate_advanced_codec_options(e,target); }
    catch (const std::exception& error) { throw std::runtime_error(where + " " + error.what()); }
}

void validate_codec_for_output_mode(const EncodingProfile& e, DiscTarget target, const VideoTimingMode& timing, const std::string& where) {
    if (target != DiscTarget::UltraHdBluRay2160 || !video_codec_is_h264(e.video_codec)) return;
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

fs::path prepare_menu_background(const ToolPaths& tools, const Menu& menu, const fs::path& work,
                                 const VideoTimingMode& timing) {
    // The scene's still background and its text/buttons share one *display*
    // rectangle. DVD's coded pixels are not square: fitting 16:9 artwork to
    // 720x480 as square pixels would add bars under otherwise correctly placed
    // text. Rasterize the background in output display-aspect space first.
    // Do this even without overlays so all still menus match the GUI preview.
    if (menu.overlays.empty() && menu.background_image.empty()) return {};
    const int W = menu.width, H = menu.height;
    std::vector<std::uint8_t> canvas(static_cast<std::size_t>(W) * static_cast<std::size_t>(H) * 4U);
    for (std::size_t i = 0; i < canvas.size(); i += 4U) {
        canvas[i] = menu.background_color.r; canvas[i+1] = menu.background_color.g;
        canvas[i+2] = menu.background_color.b; canvas[i+3] = 255U;
    }
    if (!menu.background_image.empty()) {
        const auto base = work / "menu-background-base.rgba";
        const std::string filter = menu_still_background_fit_filter(
            W, H, timing.aspect_ratio, timing.sample_aspect_ratio);
        const std::string command = shq_s(tools.ffmpeg) +
            " -hide_banner -loglevel error -y -i " + shq(menu.background_image) +
            " -frames:v 1 -an -sn -dn -vf " + shq_s(filter) +
            " -pix_fmt rgba -f rawvideo " + shq(base);
        run(command);
        const auto expected = static_cast<std::uintmax_t>(W) * static_cast<std::uintmax_t>(H) * 4U;
        std::error_code ec;
        if (fs::file_size(base, ec) != expected || ec)
            throw std::runtime_error("FFmpeg produced an invalid menu background raster: " + base.string());
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
    bool hdr10_to_sdr = false;
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
    const bool force_bt709 = target != DiscTarget::UltraHdBluRay2160 || video_codec_is_h264(codec) || has_overlay;

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
            plan.hdr10_to_sdr = true;
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

struct VideoInputHardwarePlan {
    std::string decode_args;
    std::string device_args;
    std::string filter_suffix;
    bool hardware_frames = false;
};

// Hardware filters consume GPU surfaces but always download before the CPU-only
// padding, interlacing, menu-overlay and encoder stages. This deliberately does
// not inherit the decoder or encoder backend. FFmpeg will upload again for a
// hardware encoder (or hand ordinary frames to a software encoder).
bool non_default_video_filter_requested(const ToolPaths& tools) {
    // Native scale+pad must have a different cache key from the Lanczos default
    // even though both operate entirely in system memory.
    return tools.scaling_filter_backend != "software" || tools.tonemap_filter_backend != "software";
}
bool hardware_filter_requested(const ToolPaths& tools) {
    return (tools.scaling_filter_backend != "software" && tools.scaling_filter_backend != "native") || tools.tonemap_filter_backend == "vaapi";
}
bool active_hardware_video_filter(const ToolPaths& tools, const VideoColorPlan& color, bool still) {
    return (color.hdr10_to_sdr && tools.tonemap_filter_backend == "vaapi") ||
           (!still && tools.scaling_filter_backend != "software" && tools.scaling_filter_backend != "native");
}
// Do not reject a hardware filter based on a preliminary `ffmpeg -filters`
// listing. An empty/truncated listing, wrapper executable, or failed probe is
// not proof of absence. The exact FFmpeg process used for the encode is the
// authority: it either accepts the filter or emits its real diagnostic, which
// encode_video() already reports before retrying with software filtering.
// This is especially important for tonemap_vaapi on DVD HDR10 input.

std::string selected_filter_device_options(const ToolPaths& tools, const VideoColorPlan& color) {
    if (!hardware_filter_requested(tools)) return {};
    std::string backend = color.hdr10_to_sdr && tools.tonemap_filter_backend == "vaapi" ? "vaapi" : tools.scaling_filter_backend;
    if (backend == "software") return {};
    if (backend != "vaapi" && backend != "vulkan" && backend != "cuda" && backend != "qsv")
        throw std::runtime_error("unsupported hardware filtering backend: " + backend);
    std::string spec = backend + "=bdmvauthor_filter";
    if (!tools.filter_hardware_device.empty()) {
        if (backend == "qsv") spec += ":hw,child_device=" + tools.filter_hardware_device;
        else spec += ":" + tools.filter_hardware_device;
    }
    const std::string option_name = "-init_hw_device " + shq_s(spec);
    std::string option = option_name;
    // hwupload_cuda initializes its own context; the other backends need the
    // chosen filter device to receive frames from software decoders.
    if (backend != "cuda") option += " -filter_hw_device bdmvauthor_filter";
    return option;
}
std::string video_scale_fit_expression(int width, int height, const std::string& sar,
                                       const std::string& target_dar) {
    return "w='if(gt(dar," + target_dar + ")," + std::to_string(width) +
        ",trunc((" + std::to_string(height) + "*dar/(" + sar + "))/2)*2)'" +
        ":h='if(gt(dar," + target_dar + "),trunc((" + std::to_string(width) + "*(" + sar + ")/dar)/2)*2," +
        std::to_string(height) + ")'";
}
std::string software_scale_fit(int width, int height, const std::string& sar,
                               const std::string& target_dar, const std::string& backend) {
    // The native choice uses FFmpeg scale + pad with swscale's default algorithm;
    // the pre-0.1.195 software default retains Lanczos interpolation.
    return "scale=" + video_scale_fit_expression(width,height,sar,target_dar) +
        (backend == "native" ? "," : ":flags=lanczos,");
}
std::string filter_software_format(DiscTarget target, VideoCodec codec) {
    return target_is_uhd(target) && codec == VideoCodec::Hevc ? "p010le" : "nv12";
}
// Returned chain has no trailing format/encoder transfer: the rest of the
// existing video pipeline receives ordinary CPU frames.
std::string hardware_processing_filters(const ToolPaths& tools, const VideoColorPlan& color,
        DiscTarget target, VideoCodec codec, int width, int height,
        const std::string& sar, const std::string& dar, bool still) {
    const bool tone = color.hdr10_to_sdr && tools.tonemap_filter_backend == "vaapi";
    const bool scale = !still && tools.scaling_filter_backend != "software" && tools.scaling_filter_backend != "native";
    const auto gpu = tools.scaling_filter_backend;
    if (tone && scale && gpu != "vaapi")
        throw std::runtime_error("VAAPI tone mapping combined with a different GPU scaling backend is not supported; select VAAPI scaling or software scaling");
    // Fast HDR processing intentionally reduces the active HDR picture before
    // the expensive full-float zscale + Hable path. This changes resampling
    // order, so keep quality-first as the default. The 'still' flag also means
    // pre-scaled remote FFVHUFF input, which must NOT be scaled a second time.
    const bool fast_hdr = color.hdr10_to_sdr && tools.tonemap_filter_backend == "software-fast" && !still;
    if (fast_hdr && !scale)
        return software_scale_fit(width,height,sar,dar,tools.scaling_filter_backend) + color.filter_prefix;
    if (fast_hdr && scale) {
        // HDR10 remains 10-bit BT.2020/PQ through the GPU scaler; only after
        // downloading the smaller raster do we linearize and tone-map on CPU.
        // Avoid NV12/8-bit intermediates before HDR->SDR processing.
        const auto dims = video_scale_fit_expression(width,height,sar,dar);
        std::string chain = "format=p010le,";
        chain += gpu == "cuda" ? "hwupload_cuda," : "hwupload,";
        if (gpu == "vaapi") chain += "scale_vaapi=" + dims + ":format=p010:mode=hq,";
        else if (gpu == "vulkan") chain += "scale_vulkan=" + dims + ",";
        else if (gpu == "cuda") chain += "scale_cuda=" + dims + ":interp_algo=lanczos,";
        else if (gpu == "qsv") chain += "scale_qsv=" + dims + ",";
        else throw std::runtime_error("unsupported fast HDR scaling backend: " + gpu);
        return chain + "hwdownload,format=p010le," + color.filter_prefix;
    }
    if (!tone && !scale) return color.filter_prefix;
    if (tone) {
        // Never prepend color.filter_prefix (the software zscale/Hable graph)
        // to this path: VAAPI alone must do HDR10 -> SDR or trigger a visible
        // software fallback rather than silently using the CPU.
        // HDR10/PQ BT.2020 metadata must survive the upload; the VAAPI filter
        // supplies BT.709 primaries, transfer and matrix for the output.
        std::string chain = "format=p010le,hwupload,tonemap_vaapi=format=nv12:p=bt709:t=bt709:m=bt709,";
        if (scale) chain += "scale_vaapi=" + video_scale_fit_expression(width,height,sar,dar) + ":format=nv12,";
        return chain + "hwdownload,format=nv12,";
    }
    const auto format = filter_software_format(target,codec);
    std::string chain = color.filter_prefix + "format=" + format + ",";
    if (gpu == "cuda") chain += "hwupload_cuda,";
    else chain += "hwupload,";
    const auto dims = video_scale_fit_expression(width,height,sar,dar);
    if (gpu == "vaapi") chain += "scale_vaapi=" + dims + ":format=" + (format=="p010le"?"p010":"nv12") + ":mode=hq,";
    else if (gpu == "vulkan") chain += "scale_vulkan=" + dims + ",";
    else if (gpu == "cuda") chain += "scale_cuda=" + dims + ":interp_algo=lanczos,";
    else if (gpu == "qsv") chain += "scale_qsv=" + dims + ",";
    else throw std::runtime_error("unsupported scaling filter backend: " + gpu);
    return chain + "hwdownload,format=" + format + ",";
}

std::string video_input(const ToolPaths& tools, const fs::path& input, const Project& p, VideoCodec codec,
                        const VideoTimingMode& timing, bool source_interlaced,
                        bool still, double seconds, const fs::path& progress_file,
                        const Rgba& background_color, const VideoColorPlan& color_plan, bool loop_input = false,
                        const fs::path& overlay_image = {}, bool hold_last_frame = false,
                        const VideoInputHardwarePlan& hardware = {}) {
    std::ostringstream c;
    c << shq_s(tools.ffmpeg) << " -hide_banner -loglevel error -y -nostats -stats_period 0.25 -progress "
      << shq(progress_file) << " ";
    if (!hardware.device_args.empty()) c << hardware.device_args << " ";
    if (active_hardware_video_filter(tools,color_plan,still)) {
        if (hardware.hardware_frames) {
            const auto& encoder = codec==VideoCodec::H264Hardware ? tools.h264_hardware_encoder : tools.hevc_hardware_encoder;
            const auto pos = encoder.rfind('_');
            const std::string encoder_backend = pos==std::string::npos ? "" : encoder.substr(pos+1);
            const std::string filter_backend = color_plan.hdr10_to_sdr && tools.tonemap_filter_backend=="vaapi"
                ? "vaapi" : tools.scaling_filter_backend;
            if (filter_backend != "cuda" && filter_backend != encoder_backend)
                throw std::runtime_error("hardware filter and hardware encoder require incompatible upload devices; retrying software filtering");
        }
        c << selected_filter_device_options(tools,color_plan) << " ";
    }
    const auto geometry = target_geometry_for_timing(p.target, timing);
    const int output_width = geometry.video_width, output_height = geometry.video_height;
    if (still) {
        if (input.empty()) c << "-f lavfi -i \"color=c=" << rgb_hex(background_color) << ":s=" << output_width << "x" << output_height << ":r=" << timing.temporal_rate << "\" ";
        else c << "-loop 1 -i " << shq(input) << " ";
    } else {
        if (loop_input) c << "-stream_loop -1 ";
        if (!hardware.decode_args.empty()) c << hardware.decode_args << " ";
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
        base_filter += hardware_processing_filters(tools,color_plan,p.target,codec,output_width,output_height,sar,target_dar,true)
            + "scale=" + size + (tools.scaling_filter_backend=="native" ? "," : ":flags=lanczos,") + "setsar=" + sar + ",";
    } else {
        // Fit in display-aspect space, not coded-pixel space. This is required
        // for anamorphic 1440x1080 Blu-ray and every SD/DVD mode, including 4:3.
        const bool gpu_scale = tools.scaling_filter_backend != "software" && tools.scaling_filter_backend != "native";
        base_filter += hardware_processing_filters(tools,color_plan,p.target,codec,output_width,output_height,sar,target_dar,false);
        if (!gpu_scale && !(color_plan.hdr10_to_sdr && tools.tonemap_filter_backend == "software-fast"))
            base_filter += software_scale_fit(output_width,output_height,sar,target_dar,tools.scaling_filter_backend);
        base_filter += "pad=" + size + ":(ow-iw)/2:(oh-ih)/2:color=" + rgb_hex(background_color) + ",setsar=" + sar + ",";
    }
    if (timing.interlaced)
        base_filter += "fps=" + timing.temporal_rate + ",tinterlace=mode=interleave_top,setfield=tff";
    else
        base_filter += "fps=" + timing.frame_rate;
    const std::string pixel_format = codec == VideoCodec::HevcHardware ? "p010le"
        : codec == VideoCodec::H264Hardware ? "nv12"
        : (target_is_uhd(p.target) && codec == VideoCodec::Hevc) ? "yuv420p10le" : "yuv420p";
    // It must be impossible to produce a zscale/Hable command while the active
    // HDR10 VAAPI selection is being executed; only the explicit fallback
    // ToolPaths (tonemap_filter_backend=software) may generate that graph.
    if (color_plan.hdr10_to_sdr && tools.tonemap_filter_backend == "vaapi" &&
        (base_filter.find("tonemap_vaapi=") == std::string::npos ||
         base_filter.find("tonemap=tonemap=hable") != std::string::npos))
        throw std::runtime_error("internal HDR10 filter selection error: VAAPI requested but software tone mapping constructed");
    const std::string output_filter = ",format=" + pixel_format + hardware.filter_suffix;
    const std::string pix_fmt_arg = hardware.hardware_frames ? std::string{} : ("-pix_fmt " + pixel_format + " ");
    if (overlay_image.empty()) {
        c << "-map 0:v:0 -vf \"" << base_filter << output_filter << "\" " << pix_fmt_arg
          << color_plan.ffmpeg_output_flags << "-an ";
    } else {
        c << "-filter_complex \"[0:v]" << base_filter
          << "[base];[1:v]scale=" << output_width << ":" << output_height
          << ":flags=lanczos,format=rgba[overlay];[base][overlay]overlay=0:0:format=auto" << output_filter << "[v]\" "
             "-map \"[v]\" " << pix_fmt_arg << color_plan.ffmpeg_output_flags << "-an ";
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
         " --vbv-maxrate " << peak_bitrate_limit_kbps << " --vbv-bufsize " << kUhdHevcVbvBufsizeKbits << " --keyint " << keyint << " --min-keyint 1"
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

std::string vc1enc_options(const EncodingProfile& e, DiscTarget target, const VideoTimingMode& timing,int peak_bitrate_limit_kbps) {
    std::ostringstream out;
    if (e.vc1_preset == "fast") out << " --fast";
    const int keyint = effective_keyframe_interval_frames(e,target,timing);
    out << " --bluray-compat --bitrate " << (static_cast<long long>(e.video_bitrate_kbps) * 1000LL)
        << " --max-bitrate " << (static_cast<long long>(std::min(40000,peak_bitrate_limit_kbps))*1000LL)
        << " --buffer-size 30000000 --keyint " << keyint;
    // Keep codec-private options last: vc1enc documents that later options
    // override speed-preset choices. Disc-critical names are rejected by the
    // model validator, so advanced settings cannot escape Blu-ray constraints.
    for (const auto& option : parse_advanced_codec_options(e.vc1_advanced_options)) {
        out << " --" << option.name;
        if (option.has_value) out << " " << shq_s(option.value);
    }
    return out.str();
}

bool remote_encoding_enabled(const ToolPaths& tools) {
    return tools.remote_encoding && !tools.remote_host.empty();
}

std::string posix_shell_quote(const std::string& value) {
    std::string out = "'";
    for (const char ch : value) {
        if (ch == '\'') out += "'\\''";
        else out += ch;
    }
    out += "'";
    return out;
}

std::string windows_cmd_quote(const std::string& value) {
    // Remote Windows OpenSSH normally invokes cmd.exe. Paths may contain
    // spaces/backslashes (for example C:\\Program Files\\ffmpeg\\ffmpeg.exe).
    // BDMVauthor never accepts embedded quotes in executable paths/options, so
    // conventional cmd double quoting is sufficient and preserves backslashes.
    if (value.find('\"') != std::string::npos)
        throw std::runtime_error("remote Windows command arguments may not contain a double quote");
    return "\"" + value + "\"";
}

bool remote_is_windows(const ToolPaths& tools) {
    return tools.remote_operating_system == RemoteOperatingSystem::Windows;
}

std::string remote_shell_quote(const ToolPaths& tools, const std::string& value) {
    return remote_is_windows(tools) ? windows_cmd_quote(value) : posix_shell_quote(value);
}

std::string remote_null_device(const ToolPaths& tools) {
    return remote_is_windows(tools) ? std::string("NUL") : std::string("/dev/null");
}

std::string local_shell_arg(const std::string& value) {
#if defined(_WIN32)
    // OpenSSH for Windows accepts a conventional quoted remote command string;
    // the local cmd.exe layer needs double-quote escaping rather than POSIX
    // single quotes. Tool/host fields are validated by the GUI/CLI and the
    // remote command itself uses POSIX quoting for Unix/macOS encoding hosts.
    return shq_s(value);
#else
    return posix_shell_quote(value);
#endif
}

std::string remote_ssh_target(const ToolPaths& tools) {
    return tools.remote_user.empty() ? tools.remote_host : tools.remote_user + "@" + tools.remote_host;
}

std::string ssh_remote_prefix(const ToolPaths& tools) {
    if (!remote_encoding_enabled(tools)) throw std::runtime_error("remote encoding server is not configured");
    if (tools.remote_ssh_port < 1 || tools.remote_ssh_port > 65535)
        throw std::runtime_error("remote SSH port must be 1..65535");
    std::ostringstream out;
    out << shq_s(tools.ssh.empty() ? std::string("ssh") : tools.ssh)
        << " -T -o BatchMode=yes -o ConnectTimeout=10 -o ServerAliveInterval=15 -p " << tools.remote_ssh_port << " ";
    if (!tools.remote_identity_file.empty()) out << "-i " << shq_s(tools.remote_identity_file) << " ";
    out << "-- " << local_shell_arg(remote_ssh_target(tools)) << " ";
    return out.str();
}

std::string ssh_remote_command(const ToolPaths& tools, const std::string& remote_command) {
    return ssh_remote_prefix(tools) + local_shell_arg(remote_command);
}

std::string create_remote_passlog(const ToolPaths& tools,const fs::path& local_capture) {
    std::error_code ec;fs::remove(local_capture,ec);
    std::string remote;
    if(remote_is_windows(tools)) {
        // Windows OpenSSH has no /tmp or mktemp. Use the remote OS temporary
        // directory while preserving the same single pass-log exception.
        remote = R"CMD(powershell.exe -NoProfile -NonInteractive -Command "$p=Join-Path $env:TEMP ('bdmvauthor-2pass-'+[guid]::NewGuid().ToString('N')); New-Item -ItemType File -Path $p -Force | Out-Null; [Console]::Out.Write($p)")CMD";
    } else {
        remote = "umask 077; mktemp /tmp/bdmvauthor-2pass-XXXXXXXX";
    }
    const std::string command=ssh_remote_command(tools,remote)+" > "+shq(local_capture);
    const int rc=shell_system(fail_on_interrupt_command(command));
    if(rc!=0)throw std::runtime_error("remote two-pass setup failed while creating temporary statistics file");
    const std::string stats=trim_copy(read_small_text(local_capture));fs::remove(local_capture,ec);
    if(stats.empty())throw std::runtime_error("remote temporary-file allocator returned an empty pass-log path");
    if(!remote_is_windows(tools) && stats.rfind("/tmp/bdmvauthor-2pass-",0)!=0)
        throw std::runtime_error("remote mktemp returned an unexpected statistics path");
    return stats;
}

void remove_remote_passlog(const ToolPaths& tools,const std::string& stats) noexcept {
    if(stats.empty())return;
    try {
        std::string command;
        if(remote_is_windows(tools)) {
            const auto q=[&](const std::string& v){return remote_shell_quote(tools,v);};
            command="del /F /Q "+q(stats)+" "+q(stats+".mbtree")+" "+q(stats+".cutree")+" "+q(stats+"-0.log")+" "+q(stats+"-0.log.mbtree")+" "+q(stats+"-0.log.cutree")+" 2>NUL";
        } else {
            const auto q=[&](const std::string& v){return remote_shell_quote(tools,v);};
            command="rm -f -- "+q(stats)+" "+q(stats+".mbtree")+" "+q(stats+".cutree")+" "+q(stats+"-0.log")+" "+q(stats+"-0.log.mbtree")+" "+q(stats+"-0.log.cutree");
        }
        (void)shell_system(ssh_remote_command(tools,command)+" >"+null_device()+" 2>&1");
    } catch (...) {}
}

struct RemotePasslogCleanup {
    const ToolPaths* tools=nullptr;std::string stats;
    ~RemotePasslogCleanup(){if(tools)remove_remote_passlog(*tools,stats);}
};

std::string remote_program(const std::string& configured, const char* fallback) {
    return configured.empty() ? std::string(fallback) : configured;
}

struct EncodingCapabilityRequirement {
    std::string executable;
    std::string description;
    std::vector<std::string> ffmpeg_encoders;
    std::vector<std::string> help_tokens;
};

EncodingCapabilityRequirement encoding_capability_requirement(const ToolPaths& tools, const std::string& codec) {
    const bool remote=remote_encoding_enabled(tools);
    const auto ffmpeg=[&]{return remote?remote_program(tools.remote_ffmpeg,"ffmpeg"):tools.ffmpeg;};
    const auto x264=[&]{return remote?remote_program(tools.remote_x264,"x264"):tools.x264;};
    const auto x265=[&]{return remote?remote_program(tools.remote_x265,"x265"):tools.x265;};
    const auto vc1enc=[&]{return remote?remote_program(tools.remote_vc1enc,"vc1enc"):tools.vc1enc;};
    const auto hdmaenc=[&]{return remote?remote_program(tools.remote_hdmaenc,"hdmaenc"):tools.hdmaenc;};

    if(codec=="eac371") {
        if(remote) throw UnavailableEncodingCapability("7.1 AC-3/E-AC-3 is currently provided by the local builder-supplied eac371enc helper");
        return {tools.eac371enc,"eac371enc raw-PCM 7.1 AC-3/E-AC-3 frontend",{}, {"--raw-pcm","--bitrate","--sample-format"}};
    }
    if(codec=="hdma") return {hdmaenc(),"hdmaenc",{}, {}};
    if(codec=="vc1") return {vc1enc(),"vc1enc",{}, {}};
    if(codec=="x264") {
        if(tools.h264_provider==VideoEncoderProvider::Standalone) return {x264(),"x264",{}, {}};
        return {ffmpeg(),"FFmpeg/libx264",{"libx264"}, {}};
    }
    if(codec=="hevc") {
        if(tools.hevc_provider==VideoEncoderProvider::Standalone) return {x265(),"x265",{}, {}};
        return {ffmpeg(),"FFmpeg/libx265",{"libx265"}, {}};
    }
    if(codec=="mpeg2") return {ffmpeg(),"FFmpeg MPEG-2",{"mpeg2video"}, {}};
    if(codec=="h264-hardware") {
        if(tools.h264_hardware_encoder.empty())
            throw UnavailableEncodingCapability("H.264 hardware encoder is not configured on this worker");
        return {ffmpeg(),"FFmpeg hardware encoder "+tools.h264_hardware_encoder,{tools.h264_hardware_encoder}, {}};
    }
    if(codec=="hevc-hardware") {
        if(tools.hevc_hardware_encoder.empty())
            throw UnavailableEncodingCapability("H.265 hardware encoder is not configured on this worker");
        return {ffmpeg(),"FFmpeg hardware encoder "+tools.hevc_hardware_encoder,{tools.hevc_hardware_encoder}, {}};
    }
    if(codec=="audio-ac3") return {ffmpeg(),"FFmpeg AC-3 encoder",{"ac3"}, {}};
    if(codec=="audio-dca") return {ffmpeg(),"FFmpeg DCA encoder",{"dca"}, {}};
    if(codec=="audio-truehd-ac3") return {ffmpeg(),"FFmpeg THD/AC-3 encoders",{"truehd","ac3"}, {}};
    // PCM encoders are part of normal FFmpeg builds. Probe the executable here;
    // if an unusually stripped build omits a particular PCM encoder, the normal
    // FFmpeg "unknown encoder" error remains a hard capability miss.
    if(codec=="audio-lpcm") return {ffmpeg(),"FFmpeg LPCM encoder",{}, {}};
    if(codec.rfind("audio-",0)==0) return {ffmpeg(),"FFmpeg audio encoder",{}, {}};
    return {};
}

bool process_exited_with_code(int status,int code) {
#if defined(_WIN32)
    return status==code;
#else
    return status!=-1 && WIFEXITED(status) && WEXITSTATUS(status)==code;
#endif
}

std::string capability_probe_body(const ToolPaths& tools,const EncodingCapabilityRequirement& requirement) {
    const bool windows=remote_encoding_enabled(tools)?remote_is_windows(tools):
#if defined(_WIN32)
        true;
#else
        false;
#endif
    const auto quote=[&](const std::string& value){
        if(remote_encoding_enabled(tools))return remote_shell_quote(tools,value);
#if defined(_WIN32)
        return windows_cmd_quote(value);
#else
        return posix_shell_quote(value);
#endif
    };
    std::ostringstream probe;
    if(windows) probe<<"(if exist "<<quote(requirement.executable)<<" (ver >NUL) else (where "<<quote(requirement.executable)<<" >NUL 2>&1 || exit /b 42))";
    else probe<<"command -v "<<quote(requirement.executable)<<" >/dev/null 2>&1 || exit 42";
    for(const auto& encoder:requirement.ffmpeg_encoders){
        if(windows){
            probe<<" & "<<quote(requirement.executable)<<" -hide_banner -encoders 2>NUL | findstr /L /C:"
                 <<windows_cmd_quote(encoder)<<" >NUL || exit /b 43";
        }else{
            probe<<"; case \"$("<<quote(requirement.executable)<<" -hide_banner -encoders 2>/dev/null)\" in *"
                 <<posix_shell_quote(encoder)<<"*) ;; *) exit 43 ;; esac";
        }
    }
    for(const auto& token:requirement.help_tokens){
        if(windows){
            probe<<" & "<<quote(requirement.executable)<<" --help 2>NUL | findstr /L /C:"
                 <<windows_cmd_quote(token)<<" >NUL || exit /b 43";
        }else{
            probe<<"; case \"$("<<quote(requirement.executable)<<" --help 2>&1)\" in *"
                 <<posix_shell_quote(token)<<"*) ;; *) exit 43 ;; esac";
        }
    }
    return probe.str();
}

void verify_encoding_worker_capability(const ToolPaths& tools,const std::string& codec) {
    const auto requirement=encoding_capability_requirement(tools,codec);
    if(requirement.executable.empty())return;
    const std::string body=capability_probe_body(tools,requirement);
    const std::string command=remote_encoding_enabled(tools)?ssh_remote_command(tools,body):body;
    const int rc=shell_system(command+" >"+null_device()+" 2>&1");
    if(rc==0){
        if(remote_encoding_enabled(tools) && (codec=="h264-hardware" || codec=="hevc-hardware"))
            verify_remote_hardware_encoder_device(tools,codec);
        return;
    }
    const std::string location=remote_encoding_enabled(tools)
        ? ("remote server "+remote_ssh_target(tools))
        : std::string("Local");
    if(process_exited_with_code(rc,42))
        throw UnavailableEncodingCapability(location+" does not have required executable "+requirement.executable+" ("+requirement.description+")");
    if(process_exited_with_code(rc,43))
        throw UnavailableEncodingCapability(location+" does not provide required encoder "+requirement.description);
    // A capability probe can itself fail because of a transient SSH/network
    // problem. Do not turn that into a false missing-tool verdict; the real
    // encode attempt will preserve the scheduler's normal transient/fatal
    // classification and retry/failover policy.
}

std::string remote_encoder_backend(const std::string& encoder) {
    const auto underscore = encoder.find('_');
    return underscore == std::string::npos ? std::string{} : encoder.substr(underscore + 1);
}

std::string remote_hardware_device_spec(const ToolPaths& tools, const std::string& encoder,
                                        const std::string& device_name) {
    const std::string backend = remote_encoder_backend(encoder);
    std::string spec = backend + "=" + device_name;
    if (!tools.remote_hardware_device.empty()) {
        if (backend == "qsv") spec += ",child_device=" + tools.remote_hardware_device;
        else if (backend == "vaapi") spec += ":" + tools.remote_hardware_device;
        // Other backends keep FFmpeg's normal device selection for now; the
        // render-node field is specifically intended for Linux QSV/VAAPI.
    }
    return spec;
}

std::string remote_hardware_upload_prefix(const ToolPaths& tools, const std::string& encoder,
                                          const std::string& device_name = "bdmvauthor_encode") {
    const std::string backend = remote_encoder_backend(encoder);
    if (backend == "vaapi" || backend == "qsv" || backend == "vulkan") {
        std::ostringstream out;
        out << "-init_hw_device " << remote_shell_quote(tools, remote_hardware_device_spec(tools,encoder,device_name))
            << " -filter_hw_device " << device_name << " ";
        return out.str();
    }
    return {};
}

std::string remote_hardware_filter_suffix(const std::string& encoder, bool h264) {
    const std::string backend = remote_encoder_backend(encoder);
    const std::string format = h264 ? "nv12" : "p010le";
    if (backend == "vaapi" || backend == "qsv" || backend == "vulkan")
        return ",format=" + format + (backend == "qsv" ? ",hwupload=extra_hw_frames=64" : ",hwupload");
    return {};
}

struct CommandTextResult {
    int status = -1;
    std::string output;
    bool started = false;
};

CommandTextResult capture_command_text_status(const std::string& command) {
    CommandTextResult result;
#if defined(_WIN32)
    FILE* pipe=shell_popen((command+" 2>&1").c_str(),"rb");
#else
    FILE* pipe=shell_popen((command+" 2>&1").c_str(),"r");
#endif
    if(!pipe)return result;
    result.started=true;
    std::array<char,8192> buffer{};
    for(;;){
        const auto n=std::fread(buffer.data(),1,buffer.size(),pipe);
        if(n>0)result.output.append(buffer.data(),n);
        if(n<buffer.size()){
            if(std::feof(pipe))break;
            if(std::ferror(pipe))break;
        }
    }
    result.status=shell_pclose(pipe);
    return result;
}

void verify_remote_hardware_encoder_device(const ToolPaths& tools, const std::string& codec) {
    const bool h264=codec=="h264-hardware";
    const std::string encoder=h264?tools.h264_hardware_encoder:tools.hevc_hardware_encoder;
    if(encoder.empty())throw UnavailableEncodingCapability("remote hardware encoder is not configured");
    const std::string pixel_format=h264?"nv12":"p010le";
    std::ostringstream body;
    body<<remote_shell_quote(tools,remote_program(tools.remote_ffmpeg,"ffmpeg"))
        <<" -hide_banner -loglevel error -y "
        <<remote_hardware_upload_prefix(tools,encoder,"bdmvauthor_probe")
        <<"-f lavfi -i "<<remote_shell_quote(tools,"color=c=black:s=64x64:r=1")
        <<" -frames:v 1 ";
    const auto upload=remote_hardware_filter_suffix(encoder,h264);
    if(!upload.empty())body<<"-vf "<<remote_shell_quote(tools,upload.substr(1))<<" ";
    else body<<"-pix_fmt "<<pixel_format<<" ";
    body<<"-c:v "<<remote_shell_quote(tools,encoder)<<" ";
    if(!h264)body<<"-profile:v main10 ";
    body<<"-f null "<<remote_null_device(tools);
    const auto result=capture_command_text_status(ssh_remote_command(tools,body.str()));
    if(result.started&&result.status==0)return;
    std::ostringstream message;
    message<<"remote hardware preflight failed for "<<encoder;
    if(!tools.remote_hardware_device.empty())message<<" using device "<<tools.remote_hardware_device;
    if(result.started)message<<" ("<<command_status(result.status)<<")";
    else message<<" (could not start SSH command)";
    if(!result.output.empty())message<<"\nFFmpeg/SSH: "<<result.output;
    const auto kind=classify_encode_failure(message.str());
    if(kind==EncodeFailureKind::Transient)throw std::runtime_error(message.str());
    throw UnavailableEncodingCapability(message.str());
}

std::string remote_ffmpeg_ffvhuff_input(const ToolPaths& tools, VideoCodec codec,
                                        const std::string& hardware_encoder = {}) {
    const bool h264_hw=codec==VideoCodec::H264Hardware;
    const bool hevc_hw=codec==VideoCodec::HevcHardware;
    const std::string pixel_format=hevc_hw?"p010le":h264_hw?"nv12":
        (codec==VideoCodec::Hevc?"yuv420p10le":"yuv420p");
    std::ostringstream in;
    in<<remote_shell_quote(tools,remote_program(tools.remote_ffmpeg,"ffmpeg"))
      <<" -hide_banner -loglevel error -y ";
    if(!hardware_encoder.empty())in<<remote_hardware_upload_prefix(tools,hardware_encoder);
    in<<"-f matroska -i pipe:0 -map 0:v:0 -an ";
    if(!hardware_encoder.empty()){
        const auto suffix=remote_hardware_filter_suffix(hardware_encoder,h264_hw);
        if(!suffix.empty())in<<"-vf "<<remote_shell_quote(tools,suffix.substr(1))<<" ";
        else in<<"-pix_fmt "<<pixel_format<<" ";
    }else in<<"-pix_fmt "<<pixel_format<<" ";
    return in.str();
}

bool remote_compressed_relay_eligible(bool still, bool loop_input, const fs::path& overlay_image,
                                      bool hold_last_frame) {
    // A plain title can remain compressed across the network: local FFmpeg
    // only demuxes it into a streamable Matroska pipe and remote FFmpeg does
    // the expensive decode/filter/encode work. Generated/still/looping menu
    // composition cannot use source-copy relay because synthetic/overlay inputs
    // do not exist remotely. Menu jobs instead use the lossless FFVHUFF relay.
    return !still && !loop_input && overlay_image.empty() && !hold_last_frame;
}

std::string remote_compressed_feeder(const ToolPaths& tools, const fs::path& input,
                                     const fs::path& progress_file) {
    std::ostringstream c;
    c << shq_s(tools.ffmpeg)
      << " -hide_banner -loglevel error -y -nostats -stats_period 0.25 -progress " << shq(progress_file)
      << " -i " << shq(input)
      << " -map 0:v:0 -c:v copy -an -sn -dn -f matroska - | ";
    return c.str();
}

struct RemoteSourceVideoInfo {
    std::string codec_name;
    int width = 0;
    int height = 0;
};

RemoteSourceVideoInfo remote_source_video_info(const ToolPaths& tools, const fs::path& input) {
    RemoteSourceVideoInfo info;
    if (input.empty()) return info;
    const std::string command = shq_s(tools.ffprobe) +
        " -v error -select_streams v:0 -show_entries stream=codec_name,width,height -of default=nw=1:nk=0 " + shq(input);
    const auto result = capture_command_text_status(command);
    if (!result.started || result.status != 0) return info;
    std::istringstream lines(result.output);
    std::string line;
    while (std::getline(lines,line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto equals=line.find('=');
        if (equals==std::string::npos) continue;
        const std::string key=trim_copy(line.substr(0,equals));
        const std::string value=trim_copy(line.substr(equals+1));
        if (key=="codec_name") info.codec_name=value;
        else if (key=="width") info.width=static_cast<int>(integer_value(value).value_or(0));
        else if (key=="height") info.height=static_cast<int>(integer_value(value).value_or(0));
    }
    std::transform(info.codec_name.begin(), info.codec_name.end(), info.codec_name.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return info;
}

bool remote_ffvhuff_downscale_locally(const RemoteSourceVideoInfo& source, const Project& p,
                                       const VideoTimingMode& timing) {
    if (source.width <= 0 || source.height <= 0) return false;
    const auto geometry=target_geometry_for_timing(p.target,timing);
    // If either source dimension exceeds the selected output raster, fitting the
    // picture to the target necessarily reduces the coded image in that axis.
    // Doing that reduction before FFVHUFF lowers the lossless SSH payload.
    return source.width > geometry.video_width || source.height > geometry.video_height;
}

std::string remote_source_ffvhuff_feeder(const ToolPaths& tools, const fs::path& input,
                                         const fs::path& progress_file, const Project& p,
                                         const VideoTimingMode& timing, bool source_interlaced,
                                         bool downscale_locally) {
    // Fallback transport for uncompressed sources or source codecs the remote
    // FFmpeg cannot decode.  Downscale before lossless compression only when it
    // reduces the transmitted raster.  Same-size and upscale jobs preserve the
    // source raster so scaling remains remote.  Other processing stays remote.
    std::ostringstream c;
    c << shq_s(tools.ffmpeg)
      << " -hide_banner -loglevel error -y -nostats -stats_period 0.25 -progress " << shq(progress_file)
      << " -i " << shq(input)
      << " -map 0:v:0 -an -sn -dn ";
    if (downscale_locally) {
        const auto geometry=target_geometry_for_timing(p.target,timing);
        const int output_width=geometry.video_width, output_height=geometry.video_height;
        const std::string sar=timing.sample_aspect_ratio.empty()?"1/1":timing.sample_aspect_ratio;
        const std::string target_dar=timing.aspect_ratio=="4:3"?"4/3":"16/9";
        std::string filter=
            "scale=w='if(gt(dar,"+target_dar+"),"+std::to_string(output_width)+
            ",trunc(("+std::to_string(output_height)+"*dar/("+sar+"))/2)*2)'"
            ":h='if(gt(dar,"+target_dar+"),trunc(("+std::to_string(output_width)+"*("+sar+")/dar)/2)*2,"+
            std::to_string(output_height)+")'"+
            (tools.scaling_filter_backend=="native"?std::string{}:std::string(":flags=lanczos"));
        if (source_interlaced) filter += ":interl=1";
        // Do not send target padding across SSH.  Only the active picture is
        // reduced locally; the worker adds any letterbox/pillarbox area.
        filter += ",setsar="+sar;
        c << "-vf " << shq_s(filter) << " ";
    }
    c << "-c:v ffvhuff -pred median -f matroska - | ";
    return c.str();
}

bool source_video_codec_is_uncompressed(const std::string& codec) {
    // FFmpeg exposes several packed/YUV/RGB uncompressed formats as named
    // codecs rather than rawvideo.  These are deliberately converted to
    // FFVHUFF locally instead of being copied verbatim across SSH.
    static const std::unordered_set<std::string> uncompressed = {
        "rawvideo", "v210", "v210x", "r210", "r10k", "avrp", "bitpacked",
        "v308", "v408", "v410", "y41p", "yuv4", "ayuv", "yuv4", "012v"
    };
    return uncompressed.find(codec) != uncompressed.end();
}

std::string remote_ffmpeg_compressed_input(const ToolPaths& tools, VideoCodec codec, const Project& p,
                                           const VideoTimingMode& timing, bool source_interlaced,
                                           const Rgba& background_color, const VideoColorPlan& color_plan,
                                           const std::string& hardware_encoder = {},
                                           bool input_pre_scaled_for_target = false) {
    const auto geometry = target_geometry_for_timing(p.target,timing);
    const int output_width=geometry.video_width, output_height=geometry.video_height;
    std::string filter;
    if(source_interlaced)filter+="bwdif=mode=send_field:parity=auto:deint=all,";
    const std::string sar=timing.sample_aspect_ratio.empty()?"1/1":timing.sample_aspect_ratio;
    const std::string target_dar=timing.aspect_ratio=="4:3"?"4/3":"16/9";
    // The remote worker owns its processing device. No dependency on its
    // decoder's hardware API or encoder's codec choice.
    const bool gpu_scale=!input_pre_scaled_for_target && tools.scaling_filter_backend!="software" && tools.scaling_filter_backend!="native";
    filter+=hardware_processing_filters(tools,color_plan,p.target,codec,output_width,output_height,
        sar,target_dar,input_pre_scaled_for_target);
    if(input_pre_scaled_for_target) {
        // The lossless feeder already reduced the active picture locally to save
        // SSH bandwidth.  The worker still adds target padding and performs all
        // color/cadence/encoder-format processing.
        filter+="pad="+std::to_string(output_width)+":"+std::to_string(output_height)+
            ":(ow-iw)/2:(oh-ih)/2:color="+rgb_hex(background_color)+",setsar="+sar+",";
    } else {
        if (!gpu_scale && !(color_plan.hdr10_to_sdr && tools.tonemap_filter_backend == "software-fast"))
            filter += software_scale_fit(output_width,output_height,sar,target_dar,tools.scaling_filter_backend);
        filter += "pad=" + std::to_string(output_width) + ":" + std::to_string(output_height) +
            ":(ow-iw)/2:(oh-ih)/2:color=" + rgb_hex(background_color) + ",setsar=" + sar + ",";
    }
    if(timing.interlaced)filter+="fps="+timing.temporal_rate+",tinterlace=mode=interleave_top,setfield=tff";
    else filter+="fps="+timing.frame_rate;

    const bool h264_hw=codec==VideoCodec::H264Hardware;
    const bool hevc_hw=codec==VideoCodec::HevcHardware;
    const std::string pixel_format=hevc_hw?"p010le":h264_hw?"nv12":
        (target_is_uhd(p.target)&&codec==VideoCodec::Hevc)?"yuv420p10le":"yuv420p";
    const auto hw_suffix=(h264_hw||hevc_hw)?remote_hardware_filter_suffix(hardware_encoder,h264_hw):std::string{};
    filter+=hw_suffix.empty()?(",format="+pixel_format):hw_suffix;
    const bool hardware_frames=!hw_suffix.empty();

    std::ostringstream c;
    c << remote_shell_quote(tools,remote_program(tools.remote_ffmpeg,"ffmpeg"))
      << " -hide_banner -loglevel error -y ";
    if(!hardware_encoder.empty())c<<remote_hardware_upload_prefix(tools,hardware_encoder);
    if(active_hardware_video_filter(tools,color_plan,input_pre_scaled_for_target))c<<selected_filter_device_options(tools,color_plan) << " ";
    if(!tools.hardware_decoder.empty())c<<"-hwaccel "<<remote_shell_quote(tools,tools.hardware_decoder)<<" ";
    c << "-i pipe:0 -map 0:v:0 -vf " << remote_shell_quote(tools,filter) << " ";
    if(!hardware_frames)c<<"-pix_fmt "<<pixel_format<<" ";
    c << color_plan.ffmpeg_output_flags << "-an ";
    return c.str();
}

std::string ffmpeg_hardware_backend_from_encoder(const std::string& encoder) {
    const auto underscore = encoder.find('_');
    return underscore == std::string::npos ? std::string{} : encoder.substr(underscore + 1);
}

bool ffmpeg_hardware_decode_works(const ToolPaths& tools, const fs::path& input) {
    if (tools.hardware_decoder.empty() || input.empty()) return false;
    std::ostringstream probe;
    probe << shq_s(tools.ffmpeg)
          << " -hide_banner -loglevel quiet -y -hwaccel " << shq_s(tools.hardware_decoder)
          << " -i " << shq(input)
          << " -map 0:v:0 -frames:v 1 -vf scale=16:16,format=yuv420p -an -sn -dn -f null " << null_device()
          << " >" << null_device() << " 2>&1";
    return shell_system(probe.str()) == 0;
}

VideoInputHardwarePlan video_input_hardware_plan(const ToolPaths& tools, const fs::path& input,
                                                 const EncodingProfile& e, bool still) {
    VideoInputHardwarePlan plan;
    // Hardware decoding is an optimization only. Probe one decoded frame first;
    // unsupported profiles, unavailable devices, or backend initialization
    // failures transparently fall back to the normal software decoder.
    if (!still && ffmpeg_hardware_decode_works(tools, input))
        plan.decode_args = "-hwaccel " + shq_s(tools.hardware_decoder);

    if (!video_codec_is_hardware(e.video_codec) || remote_encoding_enabled(tools)) return plan;
    const std::string encoder = e.video_codec == VideoCodec::H264Hardware
        ? tools.h264_hardware_encoder : tools.hevc_hardware_encoder;
    if (encoder.empty())
        throw std::runtime_error(e.video_codec == VideoCodec::H264Hardware
            ? "hardware H.264 encoding is selected but no usable FFmpeg H.264 hardware encoder is configured"
            : "hardware H.265 encoding is selected but no usable FFmpeg H.265 hardware encoder is configured");

    const std::string backend = ffmpeg_hardware_backend_from_encoder(encoder);
    // Encoders whose AVCodec pixel format is a hardware frame type need an
    // explicit upload device. Encoders such as NVENC, AMF, VideoToolbox,
    // MediaFoundation and V4L2M2M also accept software frames directly, so
    // FFmpeg can perform any required transfer internally.
    if (backend == "vaapi" || backend == "qsv" || backend == "vulkan") {
        plan.device_args = "-init_hw_device " + backend + "=bdmvauthor_encode -filter_hw_device bdmvauthor_encode";
        plan.filter_suffix = backend == "qsv" ? ",hwupload=extra_hw_frames=64" : ",hwupload";
        plan.hardware_frames = true;
    }
    return plan;
}

std::string ffmpeg_hardware_video_options(const ToolPaths& tools, const EncodingProfile& e,
                                          DiscTarget target, const VideoTimingMode& timing,
                                          int peak_bitrate_limit_kbps) {
    const bool h264 = e.video_codec == VideoCodec::H264Hardware;
    const std::string encoder = h264 ? tools.h264_hardware_encoder : tools.hevc_hardware_encoder;
    if (encoder.empty()) throw std::runtime_error("selected FFmpeg hardware video encoder is unavailable");
    const int keyint = effective_keyframe_interval_frames(e,target,timing);
    std::ostringstream opts;
    opts << "-c:v " << shq_s(encoder)
         << " -b:v " << e.video_bitrate_kbps << "k"
         << " -minrate " << e.video_min_bitrate_kbps << "k"
         << " -maxrate " << peak_bitrate_limit_kbps << "k"
         << " -bufsize " << (h264 ? 30000 : kUhdHevcVbvBufsizeKbits) << "k"
         << " -g " << keyint << " -keyint_min 1 -bf 3 ";
    if (h264) {
        opts << "-profile:v high -level:v 4.1 -bsf:v h264_metadata=aud=insert ";
        opts << ffmpeg_private_options(e.h264_hardware_advanced_options) << " ";
    } else {
        opts << "-profile:v main10 -level:v 5.1 -bsf:v hevc_metadata=aud=insert ";
        opts << ffmpeg_private_options(e.hevc_hardware_advanced_options) << " ";
    }
    return opts.str();
}

std::string encode_video_once(const ToolPaths& tools, const fs::path& input, const fs::path& output_base,
                         const EncodingProfile& e, const Project& p, const VideoTimingMode& timing,
                         bool source_interlaced, bool still, double seconds,
                         double expected_duration, const VideoProgressCallback& progress,
                         const Rgba& background_color = {0,0,0,255}


, bool loop_input = false,
                         const fs::path& overlay_image = {}, bool hold_last_frame = false,
                         int peak_bitrate_limit_kbps = 0, bool prefer_ffvhuff_remote_relay = false) {
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
        run_with_ffmpeg_progress(command, progress_file, expected_duration, {}, [&](double pass_fraction,double fps,double speed) {
            if (progress) {
                const double overall = (static_cast<double>(pass - 1) + pass_fraction) /
                                       static_cast<double>(passes);
                progress(overall, pass, passes, fps, speed);
            }
        });
    };

    const auto color_plan = video_color_plan(tools, input, p.target, e.video_codec, still, !overlay_image.empty());
    if (color_plan.hdr10_to_sdr) {
        // This diagnostic is emitted *before* either MPEG-2 pass. Without it,
        // the command printed by a subsequent software retry looks as though
        // the user's VAAPI preference was ignored.
        std::cerr << "BDMV Author: HDR10-to-SDR "
                  << (tools.tonemap_filter_backend == "vaapi"
                        ? "VAAPI tonemap_vaapi attempt (not zscale/Hable)"
                        : (tools.tonemap_filter_backend == "software-fast"
                            ? "fast software zscale/Hable (downscale HDR first) selected"
                            : "software zscale/Hable selected"))
                  << "; scaling=" << tools.scaling_filter_backend
                  << "; decoder=" << (tools.hardware_decoder.empty() ? "software" : tools.hardware_decoder)
                  << "; target=" << (target_is_dvd(p.target) ? "DVD" : "other")
                  << "; ffmpeg=" << tools.ffmpeg << '\n';
    }
    const auto hardware_input = video_input_hardware_plan(tools, input, e, still);

    if (remote_encoding_enabled(tools)) {
        const fs::path progress_file = output_base.string() + ".progress";
        const bool source_relay_candidate=remote_compressed_relay_eligible(still,loop_input,overlay_image,hold_last_frame);
        const RemoteSourceVideoInfo source_info=source_relay_candidate?remote_source_video_info(tools,input):RemoteSourceVideoInfo{};
        const bool source_uncompressed=source_relay_candidate&&source_video_codec_is_uncompressed(source_info.codec_name);
        const bool compressed_relay=source_relay_candidate&&!source_uncompressed;
        const bool ffvhuff_downscale_local=source_relay_candidate && (tools.scaling_filter_backend=="software" || tools.scaling_filter_backend=="native") &&
            remote_ffvhuff_downscale_locally(source_info,p,timing);
        ToolPaths software_remote_decode=tools;
        software_remote_decode.hardware_decoder.clear();

        auto local_ffvhuff_feeder = [&] {
            if(source_relay_candidate)
                return remote_source_ffvhuff_feeder(tools,input,progress_file,p,timing,source_interlaced,
                    ffvhuff_downscale_local);
            // Synthetic/still/looping/overlay menu inputs do not exist on the
            // remote worker, so compose those locally before the lossless relay.
            const VideoCodec relay_codec=(e.video_codec==VideoCodec::HevcHardware)?VideoCodec::Hevc:
                (e.video_codec==VideoCodec::H264Hardware?VideoCodec::X264:e.video_codec);
            const VideoInputHardwarePlan relay_input{};
            return video_input(tools,input,p,relay_codec,timing,source_interlaced,still,seconds,
                progress_file,background_color,color_plan,loop_input,overlay_image,hold_last_frame,relay_input) +
                "-c:v ffvhuff -pred median -f matroska - | ";
        };
        auto ffvhuff_remote_input = [&](const std::string& hardware_encoder = std::string{}) {
            if(source_relay_candidate)
                return remote_ffmpeg_compressed_input(software_remote_decode,e.video_codec,p,timing,
                    source_interlaced,background_color,color_plan,hardware_encoder,ffvhuff_downscale_local);
            return remote_ffmpeg_ffvhuff_input(tools,e.video_codec,hardware_encoder);
        };
        auto remote_to_local = [&](const fs::path& output, const std::string& compressed_remote_command,
                                   const std::string& compressed_software_command,
                                   const std::string& ffvhuff_remote_command) {
            std::vector<std::string> failed_attempts;
            auto verify_output=[&] {
                std::error_code ec;
                if (!fs::is_regular_file(output,ec) || ec || fs::file_size(output,ec)==0)
                    throw std::runtime_error("remote encoder did not return an elementary stream");
            };
            auto attempt=[&](const char* label,const std::string& feeder,const std::string& command) {
                try {
                    std::error_code ec;fs::remove(output,ec);
                    run_pass(1, progress_file, feeder + ssh_remote_command(tools,command) + " > " + shq(output));
                    verify_output();
                } catch (const AuthorCancelled&) { throw; }
                catch (const std::exception& ex) {
                    failed_attempts.push_back(std::string(label)+":\n"+ex.what());
                    throw;
                }
            };
            auto attempt_ffvhuff=[&] {
                const char* label=source_uncompressed
                    ? "lossless FFVHUFF relay for uncompressed source"
                    : (source_relay_candidate
                        ? "lossless FFVHUFF relay after remote source-decoder fallback"
                        : "lossless FFVHUFF menu/composition relay");
                attempt(label,local_ffvhuff_feeder(),ffvhuff_remote_command);
            };

            // Menu composition and uncompressed source video deliberately skip
            // source-copy transport.  Plain compressed titles always try the
            // original compressed bitstream first so decode/resize/filtering
            // happens on the remote worker.
            if(prefer_ffvhuff_remote_relay || !compressed_relay){
                attempt_ffvhuff();
                return;
            }
            try{
                attempt("original compressed bitstream with configured remote decoder",
                    remote_compressed_feeder(tools,input,progress_file),compressed_remote_command);
                return;
            }catch(const AuthorCancelled&){throw;}catch(const std::exception&){}
            if(!tools.hardware_decoder.empty()&&!compressed_software_command.empty()){
                try{
                    attempt("original compressed bitstream with remote software decoder",
                        remote_compressed_feeder(tools,input,progress_file),compressed_software_command);
                    return;
                }catch(const AuthorCancelled&){throw;}catch(const std::exception&){}
            }
            try{
                attempt_ffvhuff();
                return;
            }catch(const AuthorCancelled&){throw;}
            catch(const std::exception&){
                if(failed_attempts.size()==1)throw;
                std::ostringstream diagnostic;
                diagnostic<<"remote encoding failed after "<<failed_attempts.size()<<" attempted input/decoder paths";
                for(std::size_t n=0;n<failed_attempts.size();++n)
                    diagnostic<<"\n\n--- attempt "<<(n+1)<<" ---\n"<<failed_attempts[n];
                throw std::runtime_error(diagnostic.str());
            }
        };
        auto remote_two_pass = [&](const fs::path& output, const auto& compressed_pass_command,
                                   const auto& ffvhuff_pass_command) {
            std::vector<std::string> failed_attempts;
            auto run_attempt=[&](const char* label,const std::string& feeder,const auto& command_for) {
                try{
                    const std::string stats=create_remote_passlog(tools,progress_file.string()+".remote-passlog");
                    RemotePasslogCleanup cleanup{&tools,stats};
                    std::error_code ec;fs::remove(output,ec);
                    run_pass(1,progress_file,feeder+ssh_remote_command(tools,command_for(1,stats)));
                    run_pass(2,progress_file,feeder+ssh_remote_command(tools,command_for(2,stats))+" > "+shq(output));
                    if(!fs::is_regular_file(output,ec)||ec||fs::file_size(output,ec)==0)
                        throw std::runtime_error("remote encoder did not return an elementary stream");
                }catch(const AuthorCancelled&){throw;}
                catch(const std::exception& ex){
                    failed_attempts.push_back(std::string(label)+":\n"+ex.what());
                    throw;
                }
            };
            auto run_ffvhuff=[&] {
                const char* label=source_uncompressed
                    ? "two-pass lossless FFVHUFF relay for uncompressed source"
                    : (source_relay_candidate
                        ? "two-pass lossless FFVHUFF relay after remote source-decoder fallback"
                        : "two-pass lossless FFVHUFF menu/composition relay");
                run_attempt(label,local_ffvhuff_feeder(),ffvhuff_pass_command);
            };
            if(prefer_ffvhuff_remote_relay || !compressed_relay){
                run_ffvhuff();
                return;
            }
            try{
                run_attempt("two-pass original compressed bitstream with configured remote decoder",
                    remote_compressed_feeder(tools,input,progress_file),
                    [&](int pass,const std::string& stats){return compressed_pass_command(tools,pass,stats);});
                return;
            }catch(const AuthorCancelled&){throw;}catch(const std::exception&){}
            if(!tools.hardware_decoder.empty()){
                try{
                    run_attempt("two-pass original compressed bitstream with remote software decoder",
                        remote_compressed_feeder(tools,input,progress_file),
                        [&](int pass,const std::string& stats){return compressed_pass_command(software_remote_decode,pass,stats);});
                    return;
                }catch(const AuthorCancelled&){throw;}catch(const std::exception&){}
            }
            try{
                run_ffvhuff();
                return;
            }catch(const AuthorCancelled&){throw;}
            catch(const std::exception&){
                std::ostringstream diagnostic;
                diagnostic<<"remote two-pass encoding failed after "<<failed_attempts.size()<<" attempted input/decoder paths";
                for(std::size_t n=0;n<failed_attempts.size();++n)
                    diagnostic<<"\n\n--- attempt "<<(n+1)<<" ---\n"<<failed_attempts[n];
                throw std::runtime_error(diagnostic.str());
            }
        };

        if (e.video_codec == VideoCodec::H264Hardware || e.video_codec == VideoCodec::HevcHardware) {
            const bool h264=e.video_codec==VideoCodec::H264Hardware;
            const std::string encoder=h264?tools.h264_hardware_encoder:tools.hevc_hardware_encoder;
            if(encoder.empty())throw std::runtime_error("remote FFmpeg hardware encoder is not configured");
            const fs::path output=output_base.string()+(h264?".264":".265");
            const std::string encode_options=ffmpeg_hardware_video_options(tools,e,p.target,timing,peak_bitrate_limit_kbps)+
                (h264?"-f h264 pipe:1":"-f hevc pipe:1");
            const std::string compressed=remote_ffmpeg_compressed_input(tools,e.video_codec,p,timing,
                source_interlaced,background_color,color_plan,encoder)+encode_options;
            const std::string compressed_sw=remote_ffmpeg_compressed_input(software_remote_decode,e.video_codec,p,timing,
                source_interlaced,background_color,color_plan,encoder)+encode_options;
            const std::string ffvhuff=ffvhuff_remote_input(encoder)+encode_options;
            remote_to_local(output,compressed,compressed_sw,ffvhuff);
            return video_meta_for(output,e,timing);
        }

        if (e.video_codec == VideoCodec::X264) {
            const fs::path output=output_base.string()+".264";
            if(tools.h264_provider==VideoEncoderProvider::Standalone){
                if(e.video_min_bitrate_kbps>0)throw std::runtime_error("standalone remote x264 requires video minrate 0");
                const std::string common=remote_shell_quote(tools,remote_program(tools.remote_x264,"x264"))+x264_options(e,p.target,timing,peak_bitrate_limit_kbps);
                auto standalone_command=[&](int pass,const std::string& stats){
                    if(pass==1)return common+(e.fast_first_pass?std::string{}:std::string(" --slow-firstpass"))+" --pass 1 --stats "+remote_shell_quote(tools,stats)+" --output "+remote_shell_quote(tools,remote_null_device(tools))+" -";
                    return common+" --pass 2 --stats "+remote_shell_quote(tools,stats)+" --output - -";
                };
                if(e.two_pass){
                    remote_two_pass(output,
                        [&](const ToolPaths& decode_tools,int pass,const std::string& stats){
                            return remote_ffmpeg_compressed_input(decode_tools,e.video_codec,p,timing,source_interlaced,background_color,color_plan)+
                                "-f yuv4mpegpipe pipe:1 | "+standalone_command(pass,stats);
                        },
                        [&](int pass,const std::string& stats){
                            return ffvhuff_remote_input()+"-f yuv4mpegpipe pipe:1 | "+standalone_command(pass,stats);
                        });
                }else{
                    const std::string encoder_command=common+" --output - -";
                    const std::string compressed=remote_ffmpeg_compressed_input(tools,e.video_codec,p,timing,source_interlaced,background_color,color_plan)+"-f yuv4mpegpipe pipe:1 | "+encoder_command;
                    const std::string compressed_sw=remote_ffmpeg_compressed_input(software_remote_decode,e.video_codec,p,timing,source_interlaced,background_color,color_plan)+"-f yuv4mpegpipe pipe:1 | "+encoder_command;
                    const std::string ffvhuff=ffvhuff_remote_input()+"-f yuv4mpegpipe pipe:1 | "+encoder_command;
                    remote_to_local(output,compressed,compressed_sw,ffvhuff);
                }
            }else{
                const int keyint=effective_keyframe_interval_frames(e,p.target,timing);
                std::ostringstream params;params<<"bluray-compat=1:aud=1:nal-hrd=vbr:force-cfr=1:bframes=3:ref=4"
                    ":colorprim=bt709:transfer=bt709:colormatrix=bt709:range=tv";
                if(timing.interlaced)params<<":tff=1";else if(timing.fake_interlaced)params<<":fake-interlaced=1";
                params<<x264_private_params(e.x264_advanced_options);
                const std::string tail="-c:v libx264 -preset "+e.x264_preset+" -b:v "+std::to_string(e.video_bitrate_kbps)+"k -minrate "+std::to_string(e.video_min_bitrate_kbps)+
                    "k -maxrate "+std::to_string(peak_bitrate_limit_kbps)+"k -bufsize 30000k -profile:v high -level:v 4.1 -pix_fmt yuv420p -g "+std::to_string(keyint)+
                    " -keyint_min 1 -bf 3 -refs 4 -x264-params "+remote_shell_quote(tools,params.str())+" -f h264 pipe:1";
                auto pass_tail=[&](int pass,const std::string& stats){
                    std::string value=tail;
                    const auto marker=std::string(" -f h264 pipe:1");
                    const auto at=value.rfind(marker);if(at!=std::string::npos)value.erase(at);
                    value+=" -pass "+std::to_string(pass)+" -passlogfile "+remote_shell_quote(tools,stats);
                    if(pass==1){if(!e.fast_first_pass)value+=" -fastfirstpass 0";value+=" -f null -";}
                    else value+=" -f h264 pipe:1";
                    return value;
                };
                if(e.two_pass){
                    remote_two_pass(output,
                        [&](const ToolPaths& decode_tools,int pass,const std::string& stats){
                            return remote_ffmpeg_compressed_input(decode_tools,e.video_codec,p,timing,source_interlaced,background_color,color_plan)+pass_tail(pass,stats);
                        },
                        [&](int pass,const std::string& stats){return ffvhuff_remote_input()+pass_tail(pass,stats);});
                }else{
                    const std::string compressed=remote_ffmpeg_compressed_input(tools,e.video_codec,p,timing,source_interlaced,background_color,color_plan)+tail;
                    const std::string compressed_sw=remote_ffmpeg_compressed_input(software_remote_decode,e.video_codec,p,timing,source_interlaced,background_color,color_plan)+tail;
                    const std::string ffvhuff=ffvhuff_remote_input()+tail;
                    remote_to_local(output,compressed,compressed_sw,ffvhuff);
                }
            }
            return video_meta_for(output,e,timing);
        }

        if (e.video_codec == VideoCodec::Hevc) {
            const fs::path output=output_base.string()+".265";
            if(tools.hevc_provider==VideoEncoderProvider::Standalone){
                if(e.video_min_bitrate_kbps>0)throw std::runtime_error("standalone remote x265 requires video minrate 0");
                const std::string common=remote_shell_quote(tools,remote_program(tools.remote_x265,"x265"))+x265_options(e,p.target,timing,color_plan,peak_bitrate_limit_kbps);
                auto standalone_command=[&](int pass,const std::string& stats){
                    if(pass==1)return common+(e.fast_first_pass?std::string(" --no-slow-firstpass"):std::string(" --slow-firstpass"))+" --pass 1 --stats "+remote_shell_quote(tools,stats)+" --output "+remote_shell_quote(tools,remote_null_device(tools))+" -";
                    return common+" --pass 2 --stats "+remote_shell_quote(tools,stats)+" --output - -";
                };
                if(e.two_pass){
                    remote_two_pass(output,
                        [&](const ToolPaths& decode_tools,int pass,const std::string& stats){
                            return remote_ffmpeg_compressed_input(decode_tools,e.video_codec,p,timing,source_interlaced,background_color,color_plan)+
                                "-f yuv4mpegpipe pipe:1 | "+standalone_command(pass,stats);
                        },
                        [&](int pass,const std::string& stats){
                            return ffvhuff_remote_input()+"-f yuv4mpegpipe pipe:1 | "+standalone_command(pass,stats);
                        });
                }else{
                    const std::string encoder_command=common+" --output - -";
                    const std::string compressed=remote_ffmpeg_compressed_input(tools,e.video_codec,p,timing,source_interlaced,background_color,color_plan)+"-f yuv4mpegpipe pipe:1 | "+encoder_command;
                    const std::string compressed_sw=remote_ffmpeg_compressed_input(software_remote_decode,e.video_codec,p,timing,source_interlaced,background_color,color_plan)+"-f yuv4mpegpipe pipe:1 | "+encoder_command;
                    const std::string ffvhuff=ffvhuff_remote_input()+"-f yuv4mpegpipe pipe:1 | "+encoder_command;
                    remote_to_local(output,compressed,compressed_sw,ffvhuff);
                }
            }else{
                const int keyint=effective_keyframe_interval_frames(e,p.target,timing);
                std::ostringstream params;params<<"uhd-bd=1:aud=1:repeat-headers=1:hrd=1:vbv-maxrate="<<peak_bitrate_limit_kbps
                    <<":vbv-bufsize="<<kUhdHevcVbvBufsizeKbits<<":keyint="<<keyint<<":min-keyint=1:"<<color_plan.x265_params
                    <<x265_private_params(e.x265_advanced_options);
                const std::string tail="-c:v libx265 -preset "+e.x265_preset+" -b:v "+std::to_string(e.video_bitrate_kbps)+"k -minrate "+std::to_string(e.video_min_bitrate_kbps)+
                    "k -maxrate "+std::to_string(peak_bitrate_limit_kbps)+"k -bufsize "+std::to_string(kUhdHevcVbvBufsizeKbits)+
                    "k -profile:v main10 -level:v 5.1 -pix_fmt yuv420p10le -x265-params "+remote_shell_quote(tools,params.str())+" -f hevc pipe:1";
                auto pass_tail=[&](int pass,const std::string& stats){
                    std::string value=tail;
                    const auto marker=std::string(" -f hevc pipe:1");
                    const auto at=value.rfind(marker);if(at!=std::string::npos)value.erase(at);
                    if(pass==1&&e.fast_first_pass)value+=" -x265-params no-slow-firstpass=1:pass=1";
                    value+=" -pass "+std::to_string(pass)+" -passlogfile "+remote_shell_quote(tools,stats);
                    value+=(pass==1)?" -f null -":" -f hevc pipe:1";
                    return value;
                };
                if(e.two_pass){
                    remote_two_pass(output,
                        [&](const ToolPaths& decode_tools,int pass,const std::string& stats){
                            return remote_ffmpeg_compressed_input(decode_tools,e.video_codec,p,timing,source_interlaced,background_color,color_plan)+pass_tail(pass,stats);
                        },
                        [&](int pass,const std::string& stats){return ffvhuff_remote_input()+pass_tail(pass,stats);});
                }else{
                    const std::string compressed=remote_ffmpeg_compressed_input(tools,e.video_codec,p,timing,source_interlaced,background_color,color_plan)+tail;
                    const std::string compressed_sw=remote_ffmpeg_compressed_input(software_remote_decode,e.video_codec,p,timing,source_interlaced,background_color,color_plan)+tail;
                    const std::string ffvhuff=ffvhuff_remote_input()+tail;
                    remote_to_local(output,compressed,compressed_sw,ffvhuff);
                }
            }
            return video_meta_for(output,e,timing);
        }

        if (e.video_codec == VideoCodec::Vc1) {
            if(p.target!=DiscTarget::BluRay1080)throw std::runtime_error("remote vc1enc is available only for standard Blu-ray");
            if(e.video_min_bitrate_kbps!=0)throw std::runtime_error("remote vc1enc requires video minrate 0");
            const fs::path output=output_base.string()+".vc1";
            const std::string common=remote_shell_quote(tools,remote_program(tools.remote_vc1enc,"vc1enc"))+" -i - --format raw"+vc1enc_options(e,p.target,timing,peak_bitrate_limit_kbps);
            auto vc1_command=[&](int pass,const std::string& stats){
                if(pass==1)return common+" --pass 1 --pass-stats "+remote_shell_quote(tools,stats);
                return common+" -o - --pass 2 --pass-stats "+remote_shell_quote(tools,stats);
            };
            if(e.two_pass){
                remote_two_pass(output,
                    [&](const ToolPaths& decode_tools,int pass,const std::string& stats){
                        return remote_ffmpeg_compressed_input(decode_tools,e.video_codec,p,timing,source_interlaced,background_color,color_plan)+
                            "-f yuv4mpegpipe pipe:1 | "+vc1_command(pass,stats);
                    },
                    [&](int pass,const std::string& stats){return ffvhuff_remote_input()+"-f yuv4mpegpipe pipe:1 | "+vc1_command(pass,stats);});
            }else{
                const std::string encoder_command=remote_shell_quote(tools,remote_program(tools.remote_vc1enc,"vc1enc"))+" -i - -o - --format raw"+vc1enc_options(e,p.target,timing,peak_bitrate_limit_kbps);
                const std::string compressed=remote_ffmpeg_compressed_input(tools,e.video_codec,p,timing,source_interlaced,background_color,color_plan)+"-f yuv4mpegpipe pipe:1 | "+encoder_command;
                const std::string compressed_sw=remote_ffmpeg_compressed_input(software_remote_decode,e.video_codec,p,timing,source_interlaced,background_color,color_plan)+"-f yuv4mpegpipe pipe:1 | "+encoder_command;
                const std::string ffvhuff=ffvhuff_remote_input()+"-f yuv4mpegpipe pipe:1 | "+encoder_command;
                remote_to_local(output,compressed,compressed_sw,ffvhuff);
            }
            return video_meta_for(output,e,timing);
        }

        const fs::path output=output_base.string()+".m2v";
        const bool dvd_video=target_is_dvd(p.target);
        const int gop=effective_keyframe_interval_frames(e,p.target,timing);
        std::ostringstream opts;
        opts<<"-c:v mpeg2video -b:v "<<e.video_bitrate_kbps<<"k -minrate "<<e.video_min_bitrate_kbps<<"k -maxrate "<<peak_bitrate_limit_kbps
            <<"k -bufsize "<<(dvd_video?1835008:9781248)<<" -g "<<gop<<" -bf 2 -mpv_flags +strict_gop -aspect "<<timing.aspect_ratio
            <<" -color_primaries bt709 -color_trc bt709 -colorspace bt709 -color_range tv ";
        if(timing.interlaced)opts<<"-flags +ildct+ilme -field_order tt ";
        opts<<ffmpeg_private_options(e.mpeg2_advanced_options);
        const std::string single_tail=opts.str()+" -f mpeg2video pipe:1";
        auto pass_tail=[&](int pass,const std::string& stats){
            std::string value=opts.str()+" -pass "+std::to_string(pass)+" -passlogfile "+remote_shell_quote(tools,stats);
            value+=(pass==1)?" -f null -":" -f mpeg2video pipe:1";
            return value;
        };
        if(e.two_pass){
            remote_two_pass(output,
                [&](const ToolPaths& decode_tools,int pass,const std::string& stats){
                    return remote_ffmpeg_compressed_input(decode_tools,e.video_codec,p,timing,source_interlaced,background_color,color_plan)+pass_tail(pass,stats);
                },
                [&](int pass,const std::string& stats){return ffvhuff_remote_input()+pass_tail(pass,stats);});
        }else{
            const std::string compressed=remote_ffmpeg_compressed_input(tools,e.video_codec,p,timing,source_interlaced,background_color,color_plan)+single_tail;
            const std::string compressed_sw=remote_ffmpeg_compressed_input(software_remote_decode,e.video_codec,p,timing,source_interlaced,background_color,color_plan)+single_tail;
            const std::string ffvhuff=ffvhuff_remote_input()+single_tail;
            remote_to_local(output,compressed,compressed_sw,ffvhuff);
        }
        return video_meta_for(output,e,timing);
    }

    if (e.video_codec == VideoCodec::H264Hardware || e.video_codec == VideoCodec::HevcHardware) {
        const bool h264 = e.video_codec == VideoCodec::H264Hardware;
        const fs::path output = output_base.string() + (h264 ? ".264" : ".265");
        const fs::path progress_file = output_base.string() + ".progress";
        const std::string base = video_input(tools, input, p, e.video_codec, timing, source_interlaced, still,
            seconds, progress_file, background_color, color_plan, loop_input, overlay_image, hold_last_frame,
            hardware_input);
        const std::string options = ffmpeg_hardware_video_options(tools,e,p.target,timing,peak_bitrate_limit_kbps);
        run_pass(1, progress_file, base + options + (h264 ? "-f h264 " : "-f hevc ") + shq(output));
        return video_meta_for(output,e,timing);
    }

    if (e.video_codec == VideoCodec::X264) {
        const fs::path output = output_base.string() + ".264";
        if (tools.h264_provider == VideoEncoderProvider::Standalone) {
            if (e.video_min_bitrate_kbps > 0)
                throw std::runtime_error("standalone x264 does not expose a minrate control; select the FFmpeg/libx264 provider or set minrate to 0");
            const std::string common = shq_s(tools.x264) + x264_options(e, p.target, timing, peak_bitrate_limit_kbps);
            if (e.two_pass) {
                const fs::path stats = output_base.string() + ".x264.stats";
                const fs::path progress_file = output_base.string() + ".progress";
                const std::string y4m = video_input(tools, input, p, e.video_codec, timing, source_interlaced, still, seconds, progress_file, background_color, color_plan, loop_input, overlay_image, hold_last_frame, hardware_input) + "-f yuv4mpegpipe - | ";
                run_pass(1, progress_file, y4m + common + (e.fast_first_pass ? "" : " --slow-firstpass") + " --pass 1 --stats " + shq(stats) + " --output " + shq_s(null_device()) + " -");
                run_pass(2, progress_file, y4m + common + " --pass 2 --stats " + shq(stats) + " --output " + shq(output) + " -");
                std::error_code ec;
                fs::remove(stats, ec);
                fs::remove(stats.string() + ".mbtree", ec);
            } else {
                const fs::path progress_file = output_base.string() + ".progress";
                const std::string y4m = video_input(tools, input, p, e.video_codec, timing, source_interlaced, still, seconds, progress_file, background_color, color_plan, loop_input, overlay_image, hold_last_frame, hardware_input) + "-f yuv4mpegpipe - | ";
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
                if(pass==1 && !e.fast_first_pass) opts << "-fastfirstpass 0 ";
                return opts.str();
            };
            if (e.two_pass) {
                const fs::path stats = output_base.string() + ".x264-ffmpeg.stats";
                const fs::path progress_file = output_base.string() + ".progress";
                const std::string base = video_input(tools, input, p, e.video_codec, timing, source_interlaced, still, seconds, progress_file, background_color, color_plan, loop_input, overlay_image, hold_last_frame, hardware_input);
                run_pass(1, progress_file, base + h264_options(1,stats) + "-f null -");
                run_pass(2, progress_file, base + h264_options(2,stats) + "-f h264 " + shq(output));
                std::error_code ec;
                fs::remove(stats.string()+"-0.log",ec); fs::remove(stats.string()+"-0.log.mbtree",ec);
            } else {
                const fs::path progress_file = output_base.string() + ".progress";
                const std::string base = video_input(tools, input, p, e.video_codec, timing, source_interlaced, still, seconds, progress_file, background_color, color_plan, loop_input, overlay_image, hold_last_frame, hardware_input);
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
                const fs::path progress_file = output_base.string() + ".progress";
                const std::string y4m = video_input(tools, input, p, e.video_codec, timing, source_interlaced, still, seconds, progress_file, background_color, color_plan, loop_input, overlay_image, hold_last_frame, hardware_input) + "-f yuv4mpegpipe - | ";
                run_pass(1, progress_file, y4m + common + (e.fast_first_pass ? " --no-slow-firstpass" : " --slow-firstpass") + " --pass 1 --stats " + shq(stats) + " --output " + shq_s(null_device()) + " -");
                run_pass(2, progress_file, y4m + common + " --pass 2 --stats " + shq(stats) + " --output " + shq(output) + " -");
                std::error_code ec;
                fs::remove(stats,ec); fs::remove(stats.string()+".cutree",ec);
            } else {
                const fs::path progress_file = output_base.string() + ".progress";
                const std::string y4m = video_input(tools, input, p, e.video_codec, timing, source_interlaced, still, seconds, progress_file, background_color, color_plan, loop_input, overlay_image, hold_last_frame, hardware_input) + "-f yuv4mpegpipe - | ";
                run_pass(1, progress_file, y4m + common + " --output " + shq(output) + " -");
            }
        } else {
            auto hevc_options = [&](int pass, const fs::path& stats) {
                std::ostringstream opts;
                opts << "-c:v libx265 -preset " << e.x265_preset
                     << " -b:v " << e.video_bitrate_kbps << "k -minrate " << e.video_min_bitrate_kbps
                     << "k -maxrate " << peak_bitrate_limit_kbps << "k -bufsize " << kUhdHevcVbvBufsizeKbits << "k"
                        " -profile:v main10 -level:v 5.1 -pix_fmt yuv420p10le ";
                std::ostringstream params;
                const int keyint = effective_keyframe_interval_frames(e,p.target,timing);
                params << "uhd-bd=1:aud=1:repeat-headers=1:hrd=1:vbv-maxrate=" << peak_bitrate_limit_kbps << ":vbv-bufsize=" << kUhdHevcVbvBufsizeKbits
                          << ":keyint=" << keyint << ":min-keyint=1:" << color_plan.x265_params
                       << x265_private_params(e.x265_advanced_options);
                if (pass > 0) params << ":pass=" << pass << ":stats=" << stats.string();
                if (pass == 1) params << (e.fast_first_pass ? ":no-slow-firstpass=1" : ":slow-firstpass=1");
                opts << "-x265-params " << shq_s(params.str()) << " ";
                return opts.str();
            };
            if (e.two_pass) {
                const fs::path stats = output_base.string() + ".x265.stats";
                const fs::path progress_file = output_base.string() + ".progress";
                const std::string base = video_input(tools, input, p, e.video_codec, timing, source_interlaced, still, seconds, progress_file, background_color, color_plan, loop_input, overlay_image, hold_last_frame, hardware_input);
                run_pass(1, progress_file, base + hevc_options(1,stats) + "-f null -");
                run_pass(2, progress_file, base + hevc_options(2,stats) + "-f hevc " + shq(output));
                std::error_code ec;
                fs::remove(stats,ec); fs::remove(stats.string()+".cutree",ec);
            } else {
                const fs::path progress_file = output_base.string() + ".progress";
                const std::string base = video_input(tools, input, p, e.video_codec, timing, source_interlaced, still, seconds, progress_file, background_color, color_plan, loop_input, overlay_image, hold_last_frame, hardware_input);
                run_pass(1, progress_file, base + hevc_options(0,{}) + "-f hevc " + shq(output));
            }
        }
        return video_meta_for(output,e,timing);
    }

    if (e.video_codec == VideoCodec::Vc1) {
        if (p.target != DiscTarget::BluRay1080)
            throw std::runtime_error("experimental libvc1 VC-1 encoding is available only for standard Blu-ray");
        if (e.video_min_bitrate_kbps != 0)
            throw std::runtime_error("experimental libvc1 VC-1 encoding requires video minrate 0");
        const fs::path output = output_base.string() + ".vc1";
        // Pass 1 writes stats only (no temporary VC-1 video); pass 2 feeds
        // identical FFmpeg input and emits the sole raw elementary output.
        // Keep the stats until pass 2 has succeeded, then remove them.
        const fs::path stats = output_base.string() + ".vc1-pass.stats";
        if(e.two_pass){
            const fs::path progress_file=output_base.string()+".progress";
            const auto feeder=[&]{
                return video_input(tools,input,p,e.video_codec,timing,source_interlaced,still,seconds,
                    progress_file,background_color,color_plan,loop_input,overlay_image,hold_last_frame,hardware_input)+"-f yuv4mpegpipe - | ";
            };
            run_pass(1,progress_file,feeder()+shq_s(tools.vc1enc)+" -i - --pass 1 --pass-stats "+shq(stats)+
                " --format raw"+vc1enc_options(e,p.target,timing,peak_bitrate_limit_kbps));
            std::error_code stats_ec;
            if(!fs::is_regular_file(stats,stats_ec) || stats_ec || fs::file_size(stats,stats_ec)==0)
                throw std::runtime_error("vc1enc pass 1 did not produce two-pass statistics");
            run_pass(2,progress_file,feeder()+shq_s(tools.vc1enc)+" -i - -o "+shq(output)+
                " --pass 2 --pass-stats "+shq(stats)+" --format raw"+vc1enc_options(e,p.target,timing,peak_bitrate_limit_kbps));
            fs::remove(stats,stats_ec);
        }else{
            const fs::path progress_file=output_base.string()+".progress";
            const std::string y4m=video_input(tools,input,p,e.video_codec,timing,source_interlaced,still,seconds,
                progress_file,background_color,color_plan,loop_input,overlay_image,hold_last_frame,hardware_input)+"-f yuv4mpegpipe - | ";
            run_pass(1,progress_file,y4m+shq_s(tools.vc1enc)+" -i - -o "+shq(output)+
                " --format raw"+vc1enc_options(e,p.target,timing,peak_bitrate_limit_kbps));
        }
        std::error_code ec;
        if (!fs::is_regular_file(output, ec) || ec || fs::file_size(output, ec) == 0)
            throw std::runtime_error("vc1enc did not produce the requested raw VC-1 elementary stream");
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
        const fs::path progress_file = output_base.string() + ".progress";
        const std::string base = video_input(tools, input, p, e.video_codec, timing, source_interlaced, still, seconds, progress_file, background_color, color_plan, loop_input, overlay_image, hold_last_frame, hardware_input);
        run_pass(1, progress_file, base + opts.str() + "-pass 1 -passlogfile " + shq(stats) + " -f null -");
        run_pass(2, progress_file, base + opts.str() + "-pass 2 -passlogfile " + shq(stats) + " -f mpeg2video " + shq(output));
        std::error_code ec;
        fs::remove(stats.string() + "-0.log", ec);
        fs::remove(stats.string() + "-0.log.mbtree", ec);
    } else {
        const fs::path progress_file = output_base.string() + ".progress";
        const std::string base = video_input(tools, input, p, e.video_codec, timing, source_interlaced, still, seconds, progress_file, background_color, color_plan, loop_input, overlay_image, hold_last_frame, hardware_input);
        run_pass(1, progress_file, base + opts.str() + "-f mpeg2video " + shq(output));
    }
    return video_meta_for(output,e,timing);
}

// Retry the *entire* encode with the unchanged software processing pipeline.
// Preserve the first FFmpeg/driver diagnostic even if the fallback succeeds.
// Encoder and decoder selections do not change on fallback.
std::string encode_video(const ToolPaths& tools, const fs::path& input, const fs::path& output_base,
                         const EncodingProfile& e, const Project& p, const VideoTimingMode& timing,
                         bool source_interlaced, bool still, double seconds,
                         double expected_duration, const VideoProgressCallback& progress,
                         const Rgba& background_color = {0,0,0,255}, bool loop_input = false,
                         const fs::path& overlay_image = {}, bool hold_last_frame = false,
                         int peak_bitrate_limit_kbps = 0, bool prefer_ffvhuff_remote_relay = false) {
    auto attempt = [&](const ToolPaths& selected) {
        return encode_video_once(selected,input,output_base,e,p,timing,source_interlaced,still,
            seconds,expected_duration,progress,background_color,loop_input,overlay_image,
            hold_last_frame,peak_bitrate_limit_kbps,prefer_ffvhuff_remote_relay);
    };
    if (!hardware_filter_requested(tools)) return attempt(tools);
    // A VAAPI tone-map preference does nothing to SDR or HDR pass-through.
    // Do not retry unrelated encoder failures as hardware-filter failures.
    if (tools.scaling_filter_backend == "software" || tools.scaling_filter_backend == "native") {
        const auto source_color = video_color_plan(tools,input,p.target,e.video_codec,still,!overlay_image.empty());
        if (!source_color.hdr10_to_sdr || tools.tonemap_filter_backend != "vaapi")
            return attempt(tools);
    }
    try { return attempt(tools); }
    catch (const AuthorCancelled&) { throw; }
    catch (const std::exception& original) {
        // A failed hardware *decode* must not automatically disable a selected
        // hardware *filter*. Retry with software decoding while retaining the
        // exact same tonemap_vaapi / GPU-scale settings and the same encoder.
        // In particular, a VAAPI decoding failure on HDR10 DVD MPEG-2 need not
        // imply that VAAPI video processing is unavailable.
        std::string hardware_failures = original.what();
        const bool retry_software_decode = !tools.hardware_decoder.empty() &&
            !remote_encoding_enabled(tools);
        if (retry_software_decode) {
            ToolPaths decoding_fallback = tools;
            decoding_fallback.hardware_decoder.clear();
            std::cerr << "BDMV Author: hardware filter first attempt failed; retrying "
                         "the same GPU filters with software decoding before using zscale.\n"
                      << hardware_failures << '\n';
            try {
                const auto output = attempt(decoding_fallback);
                const auto warning = "Hardware filtering succeeded with software decoding. "
                    "The selected GPU filter was preserved (scaling=" + tools.scaling_filter_backend +
                    ", tonemap=" + tools.tonemap_filter_backend +
                    "); the hardware decoding attempt failed:\n" + hardware_failures;
                if (tools.filter_fallback_warning) tools.filter_fallback_warning(warning);
                else std::cerr << "BDMV Author: " << warning << '\n';
                return output;
            } catch (const AuthorCancelled&) { throw; }
            catch (const std::exception& without_hardware_decode) {
                hardware_failures += "\n\nVAAPI filtering with software decoding also failed:\n";
                hardware_failures += without_hardware_decode.what();
            }
        }
        ToolPaths fallback = tools;
        fallback.scaling_filter_backend = tools.scaling_filter_backend == "native" ? "native" : "software";
        fallback.tonemap_filter_backend = "software";
        // Preserve the user's fast HDR ordering when only GPU scaling failed.
        if (tools.tonemap_filter_backend == "software-fast")
            fallback.tonemap_filter_backend = "software-fast";
        const auto warning = "Requested FFmpeg hardware filtering could not be used (scaling=" +
            tools.scaling_filter_backend + ", tonemap=" + tools.tonemap_filter_backend +
            "). Software scaling / zscale-Hable tone mapping was used instead. "
            "This is a fallback, NOT the selected filter.\n"
            "Original hardware failure:\n" + hardware_failures;
        // Report the hardware failure BEFORE a potentially hours-long software
        // retry. The earlier code only notified the GUI after the entire build.
        std::cerr << "BDMV Author: " << warning << '\n';
        if (tools.filter_fallback_warning) tools.filter_fallback_warning(warning);
        try {
            const auto output = attempt(fallback);
            // Never cache CPU-filtered frames under the original GPU key.
            {
                std::ofstream marker(fallback_filter_marker(output_base));
                if (!marker) throw std::runtime_error("could not record software-filter fallback for " + output_base.string());
            }
            return output;
        } catch (const AuthorCancelled&) { throw; }
        catch (const std::exception& software) {
            throw std::runtime_error("Hardware FFmpeg filtering failed:\n" + hardware_failures +
                "\n\nSoftware filtering fallback also failed:\n" + software.what());
        }
    }
}

void write_binary(const fs::path& p, const std::vector<unsigned char>& b) {
    std::ofstream f(p, std::ios::binary);
    if (!f) throw std::runtime_error("cannot write " + p.string());
    f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    if (!f) throw std::runtime_error("failed while writing " + p.string());
}

std::string audio_input(const ToolPaths& tools, const fs::path& source, bool loop, double seconds, int ordinal,
                        int input_stream_index = -1) {
    std::ostringstream c;
    c << shq_s(tools.ffmpeg) << " -hide_banner -loglevel error -y ";
    if (loop) c << "-stream_loop -1 ";
    c << "-i " << shq(source) << " -map " << ffmpeg_audio_map_spec(ordinal, input_stream_index) << " ";
    if (loop) c << "-t " << seconds << " ";
    else if (seconds > 0.0) c << "-af apad -t " << seconds << " ";
    return c.str();
}

struct EncodedAudioResult {
    std::string meta;
    fs::path path;
    int peak_bitrate_kbps = 0;
};

#if defined(BDMVAUTHOR_HAVE_EAC371ENC)
std::string eac371_private_options(const std::string& text) {
    std::ostringstream out;
    for (const auto& option : parse_advanced_codec_options(text)) {
        const std::string name = normalized_codec_option_name(option.name);
        const bool value_option = name == "cutoff" || name == "dialnorm" || name == "coupling" ||
            name == "coupling-start-band" || name == "cpl-start-band" || name == "downmix-mode";
        const bool flag_option = name == "no-compand";
        if (!value_option && !flag_option)
            throw std::runtime_error("7.1 AC-3/E-AC-3 does not support advanced AC-3 option '" + option.name +
                                     "'; supported options are cutoff, dialnorm, coupling, coupling-start-band, downmix-mode, and no-compand");
        if (value_option && !option.has_value)
            throw std::runtime_error("7.1 AC-3/E-AC-3 option '" + option.name + "' requires a value");
        if (flag_option && option.has_value)
            throw std::runtime_error("7.1 AC-3/E-AC-3 option '" + option.name + "' is a flag and does not take a value");
        out << " --" << name;
        if (option.has_value) out << " " << shq_s(option.value);
    }
    return out.str();
}
#endif

EncodedAudioResult encode_audio(const ToolPaths& tools, const fs::path& source, const fs::path& dir,
                                const EncodingProfile& e, DiscTarget target, const std::string& language,
                                bool loop, double seconds, int ordinal = 0, int requested_channels = 0,
                                double expected_duration = 0.0, const std::function<void(double)>& progress = {},
                                int input_stream_index = -1) {
    const int src_channels = audio_channels(tools, source, dir, ordinal, input_stream_index);
    const auto source_info = probe_audio_stream(tools, source, dir / "lossless-format-probe", ordinal, seconds, input_stream_index);
    EncodingProfile resolved = e;
    if (lossless_audio_codec(resolved.audio_codec)) {
        resolved.lpcm_sample_rate_hz = resolved_lossless_sample_rate_hz(target, e.lpcm_sample_rate_hz, source_info.sample_rate);
        resolved.lpcm_bit_depth = resolved_lossless_bit_depth(target, e.lpcm_bit_depth, source_info.bits_per_sample);
    }
    const int channels = resolved_audio_output_channels(src_channels, resolved, target, requested_channels, "audio stream");
    const fs::path progress_file = dir / "audio-encode.progress";
    const std::string base = audio_input(tools, source, loop, seconds, ordinal, input_stream_index) +
        (progress ? "-progress " + shq(progress_file) + " -nostats " : std::string{});
    auto run_audio = [&](const std::string& command, double start = 0.0, double portion = 1.0) {
        if (progress) run_with_ffmpeg_progress(command, progress_file, expected_duration,
            [&](double value) { progress(start + portion * value); });
        else run(command);
    };
    if (resolved.audio_codec == AudioCodec::Ac3 && channels <= 6 && resolved.ac3_bitrate_kbps > 640) {
        throw std::runtime_error("AC-3 output at 5.1 or fewer channels may not exceed 640 kb/s; rates above 640 kb/s are available only for 7.1 core+extension output");
    }
#if defined(BDMVAUTHOR_HAVE_EAC371ENC)
    if (resolved.audio_codec == AudioCodec::Ac3 && channels == 8 && target_supports_ac3_71_extension(target)) {
        if (remote_encoding_enabled(tools))
            throw UnavailableEncodingCapability("7.1 AC-3/E-AC-3 requires the local builder-supplied eac371enc helper");
        const fs::path out = dir / "audio.eac3";
        const std::string pcm = base + "-vn -sn -dn -ac 8 -channel_layout 7.1 -ar 48000 -f f32le - | ";
        const std::string command = pcm + shq_s(tools.eac371enc) +
            " --raw-pcm --sample-rate 48000 --channels 8 --sample-format f32le --channel-layout 7.1 --bitrate " +
            std::to_string(resolved.ac3_bitrate_kbps) + "k -q" + eac371_private_options(resolved.ac3_advanced_options) +
            " - " + shq(out);
        run_audio(command);
        std::error_code ec;
        if (!fs::is_regular_file(out, ec) || ec || fs::file_size(out, ec) == 0)
            throw std::runtime_error("eac371enc did not produce 7.1 AC-3/E-AC-3 elementary output");
        return {"A_AC3, " + shq(out) + ", lang=" + language + "\n", out, resolved.ac3_bitrate_kbps};
    }
#endif
    if (resolved.audio_codec == AudioCodec::Lpcm) {
        const fs::path out = dir / "audio.wav";
        const std::string pcm = resolved.lpcm_bit_depth == 24 ? "pcm_s24le" : "pcm_s16le";
        run_audio(base + "-c:a " + pcm + " -ar " + std::to_string(resolved.lpcm_sample_rate_hz) + " -ac " + std::to_string(channels) +
            ffmpeg_private_options(resolved.lpcm_advanced_options) + " " + shq(out));
        return {"A_LPCM, " + shq(out) + ", lang=" + language + "\n", out, fixed_encoded_audio_peak_kbps(resolved, target, channels)};
    }
    if(resolved.audio_codec == AudioCodec::Libhdma && remote_encoding_enabled(tools)){
        const fs::path out=dir/"audio.dca";
        const int rate=resolved.lpcm_sample_rate_hz;
        const int depth=resolved.lpcm_bit_depth;
        const std::string layout=channels==1?"1.0":channels==2?"2.0":
            channels==3 && (source_info.channel_layout=="2.1" || source_info.channel_layout=="FL+FR+LFE") ? "2.1" : channels==3?"3.0":
            channels==4?"2.2":channels==5?"5.0":channels==6?"5.1":channels==7?"7.0":"7.1";
        const std::string ffmpeg_layout=channels==1?"mono":channels==2?"stereo":channels==4?"quad(side)":layout;
        const std::string ffmpeg_pcm=base+"-vn -sn -dn -ac "+std::to_string(channels)+
            " -channel_layout "+shq_s(ffmpeg_layout)+" -ar "+std::to_string(rate)+
            " -f "+(depth==24?std::string("s24le"):std::string("s16le"))+" - | ";
        bool debug_core_melody=false;
        std::ostringstream opts;
        for(const auto& option:parse_advanced_codec_options(e.hdma_advanced_options)){
            const std::string name=normalized_codec_option_name(option.name);
            if(name=="blu-ray-compat" || name=="observed-5.1")
                throw std::runtime_error("retired libhdma experiment: "+name+"; remove it from advanced audio options");
            if(name=="debug-core-melody"){
                if(option.has_value || debug_core_melody)
                    throw std::runtime_error("libhdma debug-core-melody is a flag, not a valued/repeated option");
                debug_core_melody=true;continue;
            }
            opts<<" --"<<option.name;if(option.has_value)opts<<" "<<posix_shell_quote(option.value);
        }
        if(debug_core_melody)
            std::fprintf(stderr,"WARNING: libhdma debug-core-melody enabled: synthetic chime in DCA core, original source in lossless XLL. TEST DISC ONLY.\n");
        std::ostringstream remote;
        remote<<remote_shell_quote(tools,remote_program(tools.remote_hdmaenc,"hdmaenc"))
              <<" - --raw-pcm --sample-rate "<<rate<<" --channels "<<channels<<" --bit-depth "<<depth
              <<" --channel-layout "<<layout<<" --lossless --bitrate 1536000 --xll-decorrelation on"
              <<(debug_core_melody?" --debug-core-melody":"")<<" --no-progress -o -"<<opts.str();
        run_audio(ffmpeg_pcm+ssh_remote_command(tools,remote.str())+" > "+shq(out));
        std::error_code ec;if(!fs::is_regular_file(out,ec)||ec||fs::file_size(out,ec)==0)
            throw std::runtime_error("remote hdmaenc did not return lossless DCA elementary output");
        const int peak=probe_elementary_audio_peak_kbps(tools,out,dir/"hdma-peak-bitrate.txt");
        return {"A_DTS, "+shq(out)+", lang="+language+"\n",out,peak};
    }

    // All FFmpeg-backed audio codecs can be encoded on a remote worker without
    // creating remote media files.  Local FFmpeg decodes/demuxes the selected
    // source track to raw PCM on stdout; SSH feeds that PCM to remote FFmpeg,
    // whose elementary output is streamed directly back into the local work
    // tree.  The only permitted remote files remain two-pass video statistics.
    if(remote_encoding_enabled(tools) && resolved.audio_codec != AudioCodec::Libhdma){
        const auto remote_ffmpeg=remote_shell_quote(tools,remote_program(tools.remote_ffmpeg,"ffmpeg"));
        auto feeder=[&](int rate){
            return base+"-vn -sn -dn -ac "+std::to_string(channels)+" -ar "+std::to_string(rate)+" -f s32le - | ";
        };
        auto remote_to=[&](const std::string& args,const fs::path& out,int rate,double start=0.0,double portion=1.0){
            const std::string remote=remote_ffmpeg+" -hide_banner -loglevel error -y -f s32le -ar "+std::to_string(rate)+
                " -ac "+std::to_string(channels)+" -i - "+args+" -";
            run_audio(feeder(rate)+ssh_remote_command(tools,remote)+" > "+shq(out),start,portion);
            std::error_code ec;if(!fs::is_regular_file(out,ec)||ec||fs::file_size(out,ec)==0)
                throw std::runtime_error("remote FFmpeg audio encoder returned no output");
        };
        if(resolved.audio_codec==AudioCodec::Lpcm){
            const fs::path out=dir/"audio.wav";const std::string pcm=resolved.lpcm_bit_depth==24?"pcm_s24le":"pcm_s16le";
            remote_to("-c:a "+pcm+" -ar "+std::to_string(resolved.lpcm_sample_rate_hz)+" -ac "+std::to_string(channels)+
                ffmpeg_private_options(resolved.lpcm_advanced_options)+" -f wav",out,resolved.lpcm_sample_rate_hz);
            return {"A_LPCM, "+shq(out)+", lang="+language+"\n",out,fixed_encoded_audio_peak_kbps(resolved,target,channels)};
        }
        if(resolved.audio_codec==AudioCodec::Dca){
            const fs::path out=dir/"audio.dts";
            remote_to("-strict -2 -c:a dca -b:a "+std::to_string(resolved.dca_bitrate_kbps)+"k -ar 48000 -ac "+std::to_string(channels)+
                ffmpeg_private_options(resolved.dca_advanced_options)+" -f dts",out,48000);
            return {"A_DTS, "+shq(out)+", lang="+language+"\n",out,fixed_encoded_audio_peak_kbps(resolved,target,channels)};
        }
        if(resolved.audio_codec==AudioCodec::TrueHdAc3){
            const fs::path thd=dir/"audio.truehd",core=dir/"audio-core.ac3",merged=dir/"audio.thd+ac3";
            remote_to("-strict -2 -c:a truehd -ar "+std::to_string(resolved.lpcm_sample_rate_hz)+" -sample_fmt "+
                std::string(resolved.lpcm_bit_depth==24?"s32p":"s16p")+" -ac "+std::to_string(channels)+
                ffmpeg_private_options(resolved.truehd_advanced_options)+" -f truehd",thd,resolved.lpcm_sample_rate_hz,0.0,0.5);
            remote_to("-c:a ac3 -b:a "+std::to_string(resolved.ac3_bitrate_kbps)+"k -ar 48000 -ac "+std::to_string(channels)+
                ffmpeg_private_options(resolved.ac3_advanced_options)+" -f ac3",core,48000,0.5,0.5);
            const int peak=detail::merge_truehd_ac3_core(thd,core,merged);
            return {"A_AC3, "+shq(merged)+", lang="+language+"\n",merged,peak};
        }
        const fs::path out=dir/"audio.ac3";
        remote_to("-c:a ac3 -b:a "+std::to_string(resolved.ac3_bitrate_kbps)+"k -ar 48000 -ac "+std::to_string(channels)+
            ffmpeg_private_options(resolved.ac3_advanced_options)+" -f ac3",out,48000);
        return {"A_AC3, "+shq(out)+", lang="+language+"\n",out,fixed_encoded_audio_peak_kbps(resolved,target,channels)};
    }

    if(resolved.audio_codec == AudioCodec::Libhdma){
        const fs::path out=dir/"audio.dca";
        const int rate=resolved.lpcm_sample_rate_hz;
        const int depth=resolved.lpcm_bit_depth;
        const std::string layout=channels==1?"1.0":channels==2?"2.0":
            channels==3 && (source_info.channel_layout=="2.1" || source_info.channel_layout=="FL+FR+LFE") ? "2.1" : channels==3?"3.0":
            channels==4?"2.2":channels==5?"5.0":channels==6?"5.1":channels==7?"7.0":"7.1";
        // No intermediate WAV, libsndfile dependency or library linkage.
        // Explicit channel layout avoids ambiguity for 7-channel audio (6.1 vs 7.0).
        const std::string ffmpeg_layout=channels==1?"mono":channels==2?"stereo":channels==4?"quad(side)":layout;
        const std::string ffmpeg_pcm=base+"-vn -sn -dn -ac "+std::to_string(channels)+
            " -channel_layout "+shq_s(ffmpeg_layout)+" -ar "+std::to_string(rate)+
            " -f "+(depth==24?std::string("s24le"):std::string("s16le"))+" - | ";
        // Retired hardware experiments are intentionally unavailable.
        // The diagnostic changes only the core; XLL is forced to independent
        // full-PCM by hdmaenc, preserving the original source exactly.
        bool debug_core_melody=false;
        std::ostringstream opts;
        for(const auto& option:parse_advanced_codec_options(e.hdma_advanced_options)){
            const std::string name=normalized_codec_option_name(option.name);
            if(name=="blu-ray-compat" || name=="observed-5.1")
                throw std::runtime_error("retired libhdma experiment: "+name+"; remove it from advanced audio options");
            if(name=="debug-core-melody"){
                if(option.has_value || debug_core_melody)
                    throw std::runtime_error("libhdma debug-core-melody is a flag, not a valued/repeated option");
                debug_core_melody=true;
                continue;
            }
            opts<<" --"<<option.name;
            if(option.has_value)opts<<" "<<shq_s(option.value);
        }
        if(debug_core_melody)
            std::fprintf(stderr,"WARNING: libhdma debug-core-melody enabled: synthetic chime in DCA core, original source in lossless XLL. TEST DISC ONLY.\n");
        run_audio(ffmpeg_pcm+shq_s(tools.hdmaenc)+" - --raw-pcm --sample-rate "+std::to_string(rate)+
            " --channels "+std::to_string(channels)+" --bit-depth "+std::to_string(depth)+
            " --channel-layout "+layout+" --lossless --bitrate 1536000 --xll-decorrelation on"+
            (debug_core_melody ? " --debug-core-melody" : "")+" --no-progress -o "+shq(out)+opts.str());
        std::error_code ec;
        if(!fs::is_regular_file(out,ec)||ec||fs::file_size(out,ec)==0)
            throw std::runtime_error("hdmaenc did not produce lossless DCA elementary output");
        const int peak=probe_elementary_audio_peak_kbps(tools,out,dir/"hdma-peak-bitrate.txt");
        return {"A_DTS, "+shq(out)+", lang="+language+"\n",out,peak};
    }
    if (resolved.audio_codec == AudioCodec::Dca) {
        const fs::path out = dir / "audio.dts";
        run_audio(base + "-strict -2 -c:a dca -b:a " + std::to_string(e.dca_bitrate_kbps) +
            "k -ar 48000 -ac " + std::to_string(channels) + ffmpeg_private_options(e.dca_advanced_options) + " -f dts " + shq(out));
        return {"A_DTS, " + shq(out) + ", lang=" + language + "\n", out, fixed_encoded_audio_peak_kbps(e, target, channels)};
    }
    if (resolved.audio_codec == AudioCodec::TrueHdAc3) {
        const fs::path thd = dir / "audio.truehd";
        const fs::path core = dir / "audio-core.ac3";
        const fs::path merged = dir / "audio.thd+ac3";
        run_audio(base + "-strict -2 -c:a truehd -ar " + std::to_string(resolved.lpcm_sample_rate_hz) +
            " -sample_fmt " + std::string(resolved.lpcm_bit_depth == 24 ? "s32p" : "s16p") + " -ac " + std::to_string(channels) +
            ffmpeg_private_options(resolved.truehd_advanced_options) + " -f truehd " + shq(thd), 0.0, 0.5);
        run_audio(base + "-c:a ac3 -b:a " + std::to_string(resolved.ac3_bitrate_kbps) +
            "k -ar 48000 -ac " + std::to_string(channels) + ffmpeg_private_options(resolved.ac3_advanced_options) + " -f ac3 " + shq(core), 0.5, 0.5);
        const int peak_bitrate_kbps = detail::merge_truehd_ac3_core(thd, core, merged);
        return {"A_AC3, " + shq(merged) + ", lang=" + language + "\n", merged, peak_bitrate_kbps};
    }

    const fs::path out = dir / "audio.ac3";
    run_audio(base + "-c:a ac3 -b:a " + std::to_string(e.ac3_bitrate_kbps) +
        "k -ar 48000 -ac " + std::to_string(channels) + ffmpeg_private_options(e.ac3_advanced_options) + " -f ac3 " + shq(out));
    return {"A_AC3, " + shq(out) + ", lang=" + language + "\n", out, fixed_encoded_audio_peak_kbps(e, target, channels)};
}

// Poll actual encoded/muxed output bytes rather than guessing from wall time.
// The final fraction is emitted only after the command succeeds.
std::uint64_t output_bytes(const fs::path& path) {
    std::error_code ec;
    if (fs::is_regular_file(path, ec)) {
        const auto bytes = fs::file_size(path, ec);
        return ec ? 0 : bytes;
    }
    if (!fs::is_directory(path, ec)) return 0;
    std::uint64_t sum = 0;
    for (fs::recursive_directory_iterator it(path, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec)) {
            const auto bytes = it->file_size(ec);
            if (!ec) sum += bytes;
        }
    }
    return sum;
}

void run_with_output_progress(const std::string& command, const fs::path& output,
                              std::uint64_t expected_bytes, const std::function<void(double)>& progress) {
    if (!progress) { run(command); return; }
    const std::string guarded = fail_on_interrupt_command(command);
    auto process = std::async(std::launch::async, [guarded] { return shell_system(guarded); });
    double last = 0.0;
    while (process.wait_for(std::chrono::milliseconds(250)) != std::future_status::ready) {
        const auto bytes = output_bytes(output);
        if (expected_bytes == 0 || bytes == 0) continue;
        const double fraction = std::min(0.985, static_cast<double>(bytes) / static_cast<double>(expected_bytes));
        if (fraction > last + 0.0005) {
            last = fraction;
            progress(fraction);
        }
    }
    const int rc = process.get();
    if (rc != 0) throw std::runtime_error("command failed (" + command_status(rc) + "): " + command);
    progress(1.0);
}

std::uint64_t mux_input_bytes(const fs::path& meta) {
    std::ifstream input(meta);
    std::string line;
    std::uint64_t bytes = 0;
    while (std::getline(input, line)) {
        // The first quoted filename on each stream line is generated by shq().
        if (line.empty() || line.starts_with("MUXOPT")) continue;
        const auto start = line.find('"');
        if (start == std::string::npos) continue;
        const auto finish = line.find('"', start + 1U);
        if (finish == std::string::npos) continue;
        const fs::path path = line.substr(start + 1U, finish - start - 1U);
        std::error_code ec;
        if (fs::is_regular_file(path, ec)) {
            const auto size = fs::file_size(path, ec);
            if (!ec) bytes += size;
        }
    }
    // Transport packet headers, timestamp padding and PSI add overhead. If
    // minbitrate stuffing exceeds this guess, show <=98.5% until exit.
    return bytes + bytes / 20U;
}

void tsmux(const ToolPaths& t, const fs::path& meta, const fs::path& out,
           const std::function<void(double)>& progress = {}) {
    fs::create_directories(out);
    const auto log = out.parent_path() / (out.filename().string() + ".tsmuxer.log");
    std::string c = shq_s(tsmuxer_command(t)) + " " + shq(meta) + " " + shq(out) + " > " + shq(log) + " 2>&1";
    const auto expected_bytes = mux_input_bytes(meta);
    // Preserve tsMuxer's log on failure, including when run asynchronously.
    const auto guarded = fail_on_interrupt_command(c);
    auto process = std::async(std::launch::async, [guarded] { return shell_system(guarded); });
    double last_progress = 0.0;
    while (process.wait_for(std::chrono::milliseconds(250)) != std::future_status::ready) {
        if (!progress || expected_bytes == 0) continue;
        const auto size = output_bytes(out / "BDMV" / "STREAM");
        const auto fraction = std::min(0.985, static_cast<double>(size) / static_cast<double>(expected_bytes));
        if (fraction > last_progress + 0.0005) {
            last_progress = fraction;
            progress(fraction);
        }
    }
    const int rc = process.get();
    if (rc != 0) {
        auto detail = read_small_text(log);
        throw std::runtime_error("tsMuxer failed (" + command_status(rc) + "): " + shq_s(tsmuxer_command(t)) +
                                 " " + shq(meta) + " " + shq(out) +
                                 (detail.empty() ? std::string() : "\n\n--- tsMuxer output ---\n" + detail));
    }
    // Validate every Blu-ray/UHD output even though no mux option is needed:
    // 96-sector alignment is now the tsMuxer default for ordinary M2TS.
    // This catches stale external/helper binaries before publishing an ISO.
    std::ifstream mux_options(meta);
    std::string mux_option_line;
    std::getline(mux_options, mux_option_line);
    if (mux_option_line.find(" --blu-ray") != std::string::npos) {
        constexpr std::uintmax_t kBdcmfSegmentBytes = 96ULL * 2048ULL;
        constexpr std::uintmax_t kM2tsSourcePacketBytes = 192ULL;
        const fs::path stream_dir = out / "BDMV/STREAM";
        if (!fs::is_directory(stream_dir))
            throw std::runtime_error("BDCMF preflight: tsMuxer did not create BDMV/STREAM at " + stream_dir.string());
        bool found_stream = false;
        for (const auto& entry : fs::directory_iterator(stream_dir)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".m2ts") continue;
            found_stream = true;
            const auto bytes = entry.file_size();
            if (bytes == 0 || bytes % kM2tsSourcePacketBytes != 0 || bytes % kBdcmfSegmentBytes != 0)
                throw std::runtime_error("BDCMF preflight: " + entry.path().filename().string() + " contains " +
                    std::to_string(bytes) + " bytes; the Blu-ray premastering segment length must be a nonzero " +
                    "multiple of 196608 bytes (96 logical sectors / 1024 M2TS source packets). " +
                    "The selected tsMuxer does not produce automatically aligned M2TS output. "
                    "Rebuild the bundled tsMuxer and verify the configured tsMuxer path.");
            // Verify the emitted TS program/clock packets and matching CLPI,
            // not just the M2TS length.  A stale tsMuxer may still build a
            // plausible ISO with PCR on 0x1011 or 0x1400.
            const auto clpi = out / "BDMV" / "CLIPINF" / (entry.path().stem().string() + ".clpi");
            validate_bluray_pcr_clip(entry.path(), clpi);
        }
        if (!found_stream) throw std::runtime_error("BDCMF preflight: tsMuxer produced no M2TS clips");
    }
    if (progress) progress(1.0);
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
                                                 int requested_channels = 0, double expected_duration = 0.0,
                                                 const std::function<void(double)>& progress = {},
                                                 int input_stream_index = -1) {
    fs::create_directories(dir);
    const int source_channels = audio_channels(tools, source, dir, ordinal, input_stream_index);
    const int channels = resolved_audio_output_channels(source_channels, encoding, DiscTarget::DvdVideo480p,
                                                        requested_channels, "DVD audio stream");
    std::ostringstream command;
    command << shq_s(tools.ffmpeg) << " -hide_banner -loglevel error -y ";
    if (loop) command << "-stream_loop -1 ";
    command << "-i " << shq(source) << " -map " << ffmpeg_audio_map_spec(ordinal, input_stream_index) << ' ';
    const fs::path progress_file = dir / "dvd-audio-encode.progress";
    if (progress) command << "-progress " << shq(progress_file) << " -nostats ";
    if (loop) command << "-t " << std::fixed << std::setprecision(3) << seconds << ' ';
    else if (seconds > 0.0) command << "-af apad -t " << std::fixed << std::setprecision(3) << seconds << ' ';
    if(remote_encoding_enabled(tools)){
        const int rate=encoding.audio_codec==AudioCodec::Lpcm?encoding.lpcm_sample_rate_hz:48000;
        command<<"-vn -sn -dn -ac "<<channels<<" -ar "<<rate<<" -f s32le - | ";
        std::ostringstream remote;remote<<remote_shell_quote(tools,remote_program(tools.remote_ffmpeg,"ffmpeg"))
            <<" -hide_banner -loglevel error -y -f s32le -ar "<<rate<<" -ac "<<channels<<" -i - ";
        if(encoding.audio_codec==AudioCodec::Lpcm){const bool depth24=encoding.lpcm_bit_depth==24;remote<<"-c:a "<<(depth24?"pcm_s24be":"pcm_s16be")<<" -ar "<<rate<<" -ac "<<channels<<ffmpeg_private_options(encoding.lpcm_advanced_options)<<" -f "<<(depth24?"s24be":"s16be")<<" -";}
        else remote<<"-c:a ac3 -b:a "<<encoding.ac3_bitrate_kbps<<"k -ar 48000 -ac "<<channels<<ffmpeg_private_options(encoding.ac3_advanced_options)<<" -f ac3 -";
        const std::string full=command.str()+ssh_remote_command(tools,remote.str())+" > "+shq(dvd_audio_file(dir,encoding.audio_codec));
        if(progress)run_with_ffmpeg_progress(full,progress_file,expected_duration,progress);else run(full);
        return {dvd_audio_file(dir,encoding.audio_codec),channels,encoding.audio_codec,rate,encoding.audio_codec==AudioCodec::Lpcm?encoding.lpcm_bit_depth:16};
    }
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
    if (progress) run_with_ffmpeg_progress(command.str(), progress_file, expected_duration, progress);
    else run(command.str());
    return {dvd_audio_file(dir, encoding.audio_codec), channels, encoding.audio_codec,
            encoding.audio_codec == AudioCodec::Lpcm ? encoding.lpcm_sample_rate_hz : 48000,
            encoding.audio_codec == AudioCodec::Lpcm ? encoding.lpcm_bit_depth : 16};
}

DvdAudioElementary encode_dvd_silence(const ToolPaths& tools, const fs::path& dir, const EncodingProfile& encoding,
                                       double duration_seconds, const std::function<void(double)>& progress = {}) {
    fs::create_directories(dir);
    std::ostringstream command;
    const fs::path progress_file = dir / "dvd-silence-encode.progress";
    command << shq_s(tools.ffmpeg) << " -hide_banner -loglevel error -y -f lavfi -i "
            << shq_s("anullsrc=r=48000:cl=stereo") << ' ';
    if (progress) command << "-progress " << shq(progress_file) << " -nostats ";
    command << "-t " << std::fixed << std::setprecision(3)
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
    if (progress) run_with_ffmpeg_progress(command.str(), progress_file, duration_seconds, progress);
    else run(command.str());
    return {dvd_audio_file(dir, encoding.audio_codec), 2, encoding.audio_codec,
            encoding.audio_codec == AudioCodec::Lpcm ? encoding.lpcm_sample_rate_hz : 48000,
            encoding.audio_codec == AudioCodec::Lpcm ? encoding.lpcm_bit_depth : 16};
}

void mux_dvd_program_stream(const ToolPaths& tools, const fs::path& video,
                            const std::vector<DvdAudioElementary>& audio,
                            const fs::path& output, int mux_bitrate_kbps,
                            const std::function<void(double)>& progress = {}) {
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
    std::uint64_t expected_bytes = output_bytes(video);
    for (const auto& a : audio) {
        command << ' ' << shq(a.path);
        expected_bytes += output_bytes(a.path);
    }
    run_with_output_progress(command.str(), output, expected_bytes + expected_bytes / 20U, progress);
}

fs::path extract_dvd_text_subtitle(const ToolPaths& tools, const fs::path& source, const fs::path& dir,
                                   const SourceTrackDescriptor& track) {
    if (is_bitmap_subtitle(track.codec_name))
        throw std::runtime_error("DVD bitmap subtitles must be rendered through the bitmap conversion path");
    fs::create_directories(dir);
    const auto out = dir / "subtitle.srt";
    const fs::path subtitle_source = track.external_source.empty() ? source : track.external_source;
    const int subtitle_ordinal = track.external_source.empty() ? track.source_ordinal : 0;
    run(shq_s(tools.ffmpeg) + " -hide_banner -loglevel error -y -i " + shq(subtitle_source) +
        " -map 0:s:" + std::to_string(subtitle_ordinal) + " -vn -an -dn -c:s srt -f srt " + shq(out));
    return out;
}

// BDSup2Sub exports Sony BDN XML and one or more palettized PNG graphics
// per event. Turn them into native DVD subpictures with spumux rather than
// trying to pass bitmap PGS through FFmpeg's text-only SRT encoder.
std::map<std::string, std::string> bdn_xml_attributes(const std::string& value) {
    std::map<std::string, std::string> out;
    static const std::regex attribute(R"attr(([A-Za-z]+)\s*=\s*"([^"]*)")attr");
    for (auto it = std::sregex_iterator(value.begin(), value.end(), attribute);
         it != std::sregex_iterator(); ++it)
        out[(*it)[1].str()] = (*it)[2].str();
    return out;
}

std::string bdn_xml_unescape(std::string value) {
    constexpr std::array<std::pair<const char*, const char*>, 5> entities = {{{"&amp;", "&"},
        {"&quot;", "\""}, {"&apos;", "'"}, {"&lt;", "<"}, {"&gt;", ">"}}};
    for (const auto& [escape, replacement] : entities) {
        std::size_t pos = 0;
        while ((pos = value.find(escape, pos)) != std::string::npos) {
            value.replace(pos, std::strlen(escape), replacement);
            pos += std::strlen(replacement);
        }
    }
    return value;
}

fs::path bdn_graphic_file(const fs::path& bdn_xml, const std::string& filename) {
    auto cleaned = bdn_xml_unescape(filename);
    const auto begin = cleaned.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) throw std::runtime_error("Empty BDN graphic filename");
    const auto end = cleaned.find_last_not_of(" \t\r\n");
    const auto name = fs::path(cleaned.substr(begin, end - begin + 1));
    if (name.is_absolute()) throw std::runtime_error("BDN graphic uses an absolute filename");
    for (const auto& component : name)
        if (component == "..") throw std::runtime_error("BDN graphic escapes its conversion directory");
    const auto path = bdn_xml.parent_path() / name;
    if (!fs::is_regular_file(path))
        throw std::runtime_error("BDSup2Sub did not create subtitle graphic: " + path.string());
    return path;
}

std::string bdn_spumux_timestamp(const std::string& stamp, double fps) {
    // BDN uses hh:mm:ss:frame, where hh:mm:ss is measured in nominal integer
    // frames; for 23.976/29.97 signals the clock must be scaled by 1001/1000.
    unsigned hh = 0, mm = 0, ss = 0, ff = 0;
    char c1 = 0, c2 = 0, c3 = 0;
    std::istringstream in(stamp);
    if (!(in >> hh >> c1 >> mm >> c2 >> ss >> c3 >> ff) ||
        c1 != ':' || c2 != ':' || c3 != ':' || mm > 59 || ss > 59 ||
        hh > 999 || fps < 1 || !std::isfinite(fps))
        throw std::runtime_error("Invalid BDN subtitle timecode: " + stamp);
    const auto nominal = static_cast<unsigned>(std::lround(fps));
    if (ff >= nominal) throw std::runtime_error("BDN subtitle frame exceeds format frame rate: " + stamp);
    const double seconds = (((hh * 60U + mm) * 60U + ss) * nominal + ff) / fps;
    const auto hundredths = static_cast<std::uint64_t>(std::llround(seconds * 100.0));
    std::ostringstream out;
    out << std::setfill('0') << std::setw(2) << hundredths / 360000U << ':'
        << std::setw(2) << (hundredths / 6000U) % 60U << ':'
        << std::setw(2) << (hundredths / 100U) % 60U << '.'
        << std::setw(2) << hundredths % 100U;
    return out.str();
}

int bdn_int_attribute(const std::map<std::string, std::string>& attrs, const char* key) {
    const auto it = attrs.find(key);
    if (it == attrs.end()) throw std::runtime_error(std::string("Missing BDN graphic attribute: ") + key);
    int value = 0;
    const auto& text = it->second;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        throw std::runtime_error(std::string("Invalid BDN graphic attribute: ") + key);
    return value;
}

struct BdnGraphic { fs::path image; int x = 0, y = 0; };

fs::path composite_bdn_graphics(const ToolPaths& tools, const std::vector<BdnGraphic>& graphics,
                                const fs::path& output, int width, int height) {
    if (graphics.size() == 1) return graphics.front().image;
    // Each BDN Event may contain multiple independently positioned PGS objects.
    // Merge all of them; dropping the second and subsequent objects corrupts
    // subtitles that use separate sign or karaoke objects.
    std::ostringstream cmd;
    cmd << shq_s(tools.ffmpeg) << " -hide_banner -loglevel error -y -f lavfi -i "
        << shq_s("color=c=black@0.0:s=" + std::to_string(width) + "x" + std::to_string(height) +
                 ":d=1,format=rgba");
    for (const auto& graphic : graphics) cmd << " -i " << shq(graphic.image);
    std::ostringstream filter;
    std::string previous = "[0:v]";
    for (std::size_t i = 0; i < graphics.size(); ++i) {
        const auto current = "[v" + std::to_string(i) + "]";
        if (i > 0) filter << ';';
        filter << previous << '[' << i + 1 << ":v]overlay=x=" << graphics[i].x << ":y=" << graphics[i].y
               << ":format=auto:shortest=1" << current;
        previous = current;
    }
    cmd << " -filter_complex " << shq_s(filter.str()) << " -map " << shq_s(previous)
        << " -frames:v 1 " << shq(output);
    run(cmd.str());
    if (!fs::is_regular_file(output) || fs::file_size(output) == 0)
        throw std::runtime_error("Cannot combine BDN subtitle bitmaps into " + output.string());
    return output;
}

std::pair<int, int> dvd_subpicture_png_dimensions(const fs::path& image) {
    // BDSup2Sub BDN exports and composites are PNGs.  Read only the IHDR size
    // so FFmpeg may decode the image into a dimension-checked RGBA buffer.
    std::ifstream input(image, std::ios::binary);
    std::array<unsigned char, 24> header{};
    input.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    static constexpr std::array<unsigned char, 8> signature{137,80,78,71,13,10,26,10};
    if (!input || !std::equal(signature.begin(), signature.end(), header.begin()) ||
        std::string_view(reinterpret_cast<const char*>(header.data()+12),4) != "IHDR")
        throw std::runtime_error("DVD subtitle graphic is not a valid PNG: " + image.string());
    const auto dim = [&](int i) -> std::uint32_t {
        return (static_cast<std::uint32_t>(header[static_cast<std::size_t>(i)]) << 24U) |
               (static_cast<std::uint32_t>(header[static_cast<std::size_t>(i)+1U]) << 16U) |
               (static_cast<std::uint32_t>(header[static_cast<std::size_t>(i)+2U]) << 8U) |
                static_cast<std::uint32_t>(header[static_cast<std::size_t>(i)+3U]);
    };
    const auto width = dim(16), height = dim(20);
    if (width == 0U || height == 0U || width > 720U || height > 576U)
        throw std::runtime_error("DVD subtitle PNG has invalid dimensions: " + image.string());
    return {static_cast<int>(width), static_cast<int>(height)};
}

fs::path dvd_quantize_bitmap(const ToolPaths& tools, const fs::path& original, const fs::path& output) {
    // Four distinct RGBA entries at most, including transparency, *per event*;
    // at most eight RGB entries in the title-wide DVD CLUT.  Do not use
    // palettegen: it may discard rare white text in favor of a larger black
    // outline/backdrop, causing formerly white captions to appear black.
    const auto [width, height] = dvd_subpicture_png_dimensions(original);
    const auto decoded = output.parent_path() / (output.stem().string() + "-decoded.rgba");
    const auto quantized = output.parent_path() / (output.stem().string() + "-quantized.rgba");
    run(shq_s(tools.ffmpeg) + " -hide_banner -loglevel error -y -i " + shq(original) +
        " -frames:v 1 -pix_fmt rgba -f rawvideo " + shq(decoded));
    const std::size_t expected = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U;
    if (!fs::is_regular_file(decoded) || fs::file_size(decoded) != expected)
        throw std::runtime_error("Cannot decode subtitle image as RGBA: " + original.string());
    std::ifstream input(decoded, std::ios::binary);
    std::vector<std::uint8_t> rgba(expected);
    input.read(reinterpret_cast<char*>(rgba.data()), static_cast<std::streamsize>(rgba.size()));
    if (!input) throw std::runtime_error("Cannot read subtitle RGBA frame: " + original.string());
    const auto reduced = detail::dvd_subpicture_rgba_palette(rgba);
    std::ofstream raw(quantized, std::ios::binary | std::ios::trunc);
    raw.write(reinterpret_cast<const char*>(reduced.data()), static_cast<std::streamsize>(reduced.size()));
    raw.close();
    if (!raw) throw std::runtime_error("Cannot write DVD subtitle RGBA frame: " + quantized.string());
    run(shq_s(tools.ffmpeg) + " -hide_banner -loglevel error -y -f rawvideo -pixel_format rgba -video_size " +
        std::to_string(width) + "x" + std::to_string(height) + " -i " + shq(quantized) +
        " -frames:v 1 -update 1 " + shq(output));
    if (!fs::is_regular_file(output) || fs::file_size(output) == 0)
        throw std::runtime_error("Cannot reduce subtitle image to DVD's four-color palette: " + output.string());
    std::error_code ignored;
    fs::remove(decoded, ignored);
    fs::remove(quantized, ignored);
    return output;
}

fs::path spumux_dvd_bitmap_subtitle(const ToolPaths& tools, const fs::path& input, const fs::path& output,
                                    const fs::path& dir, const fs::path& title_source,
                                    const SourceTrackDescriptor& track, std::size_t stream_index,
                                    const VideoTimingMode& timing) {
    fs::create_directories(dir);
    const auto source = normalized_bitmap_input(tools, title_source, track, dir);
    const auto bdn = dir / "bitmap-bdn.xml";
    const bool pal = dvd_timing_is_pal(timing);
    const int height = pal ? 576 : 480;
    const int width = 720;
    convert_bitmap_with_bdsup2sub(tools, source, bdn, height, pal ? "25" : "29.97");
    std::ifstream file(bdn);
    const std::string xml((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    static const std::regex framerate(R"rx(FrameRate\s*=\s*"([0-9.]+)")rx", std::regex_constants::icase);
    std::smatch matched;
    double fps = pal ? 25.0 : 29.97;
    if (std::regex_search(xml, matched, framerate)) fps = std::stod(matched[1].str());
    static const std::regex event(R"rx(<Event\b([^>]*)>([\s\S]*?)</Event>)rx", std::regex_constants::icase);
    static const std::regex image(R"rx(<Graphic\b([^>]*)>([\s\S]*?)</Graphic>)rx", std::regex_constants::icase);
    std::ostringstream spec;
    spec << "<subpictures format=\"" << dvd_spumux_format(timing) << "\"><stream>\n";
    std::size_t events = 0;
    for (auto it = std::sregex_iterator(xml.begin(), xml.end(), event);
         it != std::sregex_iterator(); ++it) {
        const auto attrs = bdn_xml_attributes((*it)[1].str());
        const auto from = attrs.find("InTC"), to = attrs.find("OutTC");
        if (from == attrs.end() || to == attrs.end())
            throw std::runtime_error("BDSup2Sub BDN subtitle event has no timing");
        const auto start = bdn_spumux_timestamp(from->second, fps);
        const auto end = bdn_spumux_timestamp(to->second, fps);
        if (start >= end) throw std::runtime_error("BDSup2Sub BDN subtitle event has a zero or negative duration");
        std::vector<BdnGraphic> graphics;
        const std::string content = (*it)[2].str();
        for (auto gi = std::sregex_iterator(content.begin(), content.end(), image);
             gi != std::sregex_iterator(); ++gi) {
            auto graphic_attrs = bdn_xml_attributes((*gi)[1].str());
            const int x = bdn_int_attribute(graphic_attrs, "X");
            const int y = bdn_int_attribute(graphic_attrs, "Y");
            if (x < 0 || x >= width || y < 0 || y >= height)
                throw std::runtime_error("BDSup2Sub generated out-of-bounds DVD subtitle graphics");
            graphics.push_back({bdn_graphic_file(bdn, (*gi)[2].str()), x, y});
        }
        if (graphics.empty()) throw std::runtime_error("BDN subtitle event contains no graphic");
        const auto graphic = composite_bdn_graphics(tools, graphics,
            dir / ("event-" + std::to_string(events) + ".png"), width, height);
        const auto dvd_graphic = dvd_quantize_bitmap(tools, graphic,
            dir / ("dvd-event-" + std::to_string(events) + ".png"));
        const bool forced = attrs.contains("Forced") &&
            (attrs.at("Forced") == "True" || attrs.at("Forced") == "true" || attrs.at("Forced") == "1");
        spec << "<spu start=\"" << start << "\" end=\"" << end << "\" image=\""
             << xml_escape(dvd_graphic.string()) << "\"";
        if (graphics.size() == 1)
            spec << " xoffset=\"" << graphics.front().x << "\" yoffset=\"" << graphics.front().y << "\"";
        if (forced) spec << " force=\"yes\"";
        spec << "/>\n";
        ++events;
    }
    if (events == 0) throw std::runtime_error("BDSup2Sub produced no DVD subtitle events");
    spec << "</stream></subpictures>\n";
    const auto spumux_xml = dir / "bitmap-spumux.xml";
    write_text(spumux_xml, spec.str());
    run(shq_s(tools.spumux) + " -s " + std::to_string(stream_index) + " " + shq(spumux_xml) +
        " < " + shq(input) + " > " + shq(output));
    return output;
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
    const char* mux_mode = target_is_uhd(target) ? "--blu-ray-v3" : (target_is_avchd(target) ? "--avchd" : "--blu-ray");
    s << "MUXOPT " << mux_mode
      << " --new-audio-pes"
      // Main A/V clips and standalone preloaded IG subclips must share the
      // Blu-ray dedicated PCR PID (0x1001), not place PCR on the video or
      // graphics elementary PID.  MuxerManager propagates the option to
      // *both* its main muxer and separate subClip muxer before intAddStream().
      // AVCHD retains tsMuxer's previous behavior.
      << (target_is_avchd(target) ? "" : " --no-pcr-on-video-pid")
      << " --start-time=" << (hdmv::kBluRayMuxStartPts90k / 2U);
    // tsMuxer aligns ordinary M2TS/MTS clips to 96-sector segments by default.
    // No private option is required; BDMV Author validates the actual output.
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
        if (source.inherit_encoding) { effective.encoding = parent->encoding; effective.uhd_encoding = parent->uhd_encoding; effective.dvd_encoding = parent->dvd_encoding; effective.encoding_worker = parent->encoding_worker; effective.encoding_worker_mode = parent->encoding_worker_mode; }
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

AuthorEngine::AuthorEngine(ToolPaths t, AuthoringLimits limits, StoragePaths storage) : tools_(std::move(t)), limits_(limits), storage_(std::move(storage)) {
#if defined(_WIN32) || defined(__APPLE__)
    // spumux is a child process and must inherit the private, relocatable
    // Fontconfig configuration before it performs text-subtitle rendering.
    detail::configure_fontconfig_runtime_environment();
#endif
#if defined(_WIN32) || defined(__APPLE__)
    // Windows distributions and macOS .app bundles keep the encoding and DVD
    // helper executables beside the frontend.  Resolve the bare defaults to
    // those private copies so command-line and GUI authoring behave the same
    // when launched without a developer/Nix shell PATH.
    tools_.ffmpeg=resolve_sibling_default_tool(std::move(tools_.ffmpeg),"ffmpeg");
    tools_.ffprobe=resolve_sibling_default_tool(std::move(tools_.ffprobe),"ffprobe");
    tools_.x264=resolve_sibling_default_tool(std::move(tools_.x264),"x264");
    tools_.x265=resolve_sibling_default_tool(std::move(tools_.x265),"x265");
    tools_.dvdauthor=resolve_sibling_default_tool(std::move(tools_.dvdauthor),"dvdauthor");
    tools_.spumux=resolve_sibling_default_tool(std::move(tools_.spumux),"spumux");
    tools_.mplex=resolve_sibling_default_tool(std::move(tools_.mplex),"mplex");
#endif
    // vc1enc/hdmaenc are external process frontends. Bundled siblings are a
    // packaging convenience, not a compile-time requirement; separately
    // installed frontends and Local fallback must work as well.
    tools_.vc1enc=resolve_sibling_default_tool(std::move(tools_.vc1enc),"vc1enc");
    tools_.hdmaenc=resolve_sibling_default_tool(std::move(tools_.hdmaenc),"hdmaenc");
    if (tools_.bdsup2sub.empty()) tools_.bdsup2sub="bdsup2sub";
    tools_.bdsup2sub=resolve_sibling_default_tool(std::move(tools_.bdsup2sub),"bdsup2sub");
#if defined(BDMVAUTHOR_HAVE_EAC371ENC)
    tools_.eac371enc=resolve_sibling_default_tool(std::move(tools_.eac371enc),"eac371enc");
#endif
    // mkisofs is a BDMV Author private sibling on every packaged platform.
    // Explicit paths remain authoritative because resolve_sibling_default_tool()
    // only substitutes the bare default command name.
    tools_.mkisofs=resolve_sibling_default_tool(std::move(tools_.mkisofs),"mkisofs");
}


void AuthorEngine::validate_project(const Project& p, bool allow_exceeding_format_limits) {
    if (p.titles.empty()) throw std::runtime_error("add at least one title");
    if (p.titles.size() > 999) throw std::runtime_error("too many titles");
    if (target_is_avchd(p.target)) {
        if (p.titles.size() != 1U) throw std::runtime_error("AVCHD export currently requires exactly one title");
        if (project_has_menu(p)) throw std::runtime_error("AVCHD export currently requires a menuless project");
        if (!p.first_play_actions.empty() && !is_default_menuless_startup_sequence(p.first_play_actions, 1U))
            throw std::runtime_error("AVCHD export does not support custom First Playback navigation");
        if (!p.titles.front().menu_button_actions.empty())
            throw std::runtime_error("AVCHD export does not support custom title Menu-button navigation");
        if (p.titles.front().prohibited_user_operations != 0U)
            throw std::runtime_error("AVCHD export does not support custom prohibited-user-operation masks");
    }
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
            // Resolve the stream's effective audio profile even when it inherits the
            // title profile. This lets target/channel legality be rejected before any
            // source probing or encoding starts. In particular, the dependent E-AC-3
            // 7.1 extension is Blu-ray/UHD-only; DVD-Video and AVCHD remain plain AC-3.
            auto effective = encoding_for_target(t, p.target);
            if (stream.override_encoding)
                copy_audio_encoding_settings(effective, encoding_for_target(stream, p.target));
            if (stream.output_channels > max_audio_channels_for_encoding(effective, p.target)) {
                if (effective.audio_codec == AudioCodec::Ac3 && stream.output_channels == 8 &&
                    !target_supports_ac3_71_extension(p.target))
                    throw std::runtime_error("7.1 AC-3 core+E-AC-3 extension output is not permitted for " +
                        std::string(p.target == DiscTarget::DvdVideo480p ? "DVD-Video" : "AVCHD") +
                        "; use plain AC-3 at 5.1 or fewer channels");
                throw std::runtime_error("audio stream output channel count exceeds the selected codec/target limit");
            }
            if (stream.override_encoding) {
                // Audio-stream overrides carry per-target EncodingProfile objects for
                // serialization symmetry, but only their audio fields are effective.
                // Validate those fields against the currently selected target while
                // inheriting video fields from the title. This also prevents inactive
                // Blu-ray/DVD profiles from rejecting an Ultra HD Blu-ray build.
                const std::string target_name = p.target == DiscTarget::UltraHdBluRay2160 ? "UHD" :
                    (p.target == DiscTarget::Avchd ? "AVCHD" : (p.target == DiscTarget::DvdVideo480p ? "DVD" : "standard Blu-ray"));
                validate_encoding_for_target(effective, p.target,
                                             "audio stream " + target_name + " override",
                                             allow_exceeding_format_limits);
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
        validate_encoding_for_target(encoding_for_target(t, p.target), p.target,
                                     title_where + " selected-target fallback",
                                     allow_exceeding_format_limits);
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
        validate_encoding_for_target(encoding_for_target(menu, p.target), p.target,
                                     where + " selected-target fallback",
                                     allow_exceeding_format_limits);
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

void AuthorEngine::author(const Project& p, ProgressCallback cb, CancellationCallback cancelled, EncodeJobProgressCallback encode_progress) {
    CancellationRegistration cancellation_registration(cancelled);
    authoring_cancellation_point();
    validate_project(p, limits_.allow_exceeding_format_limits);
    const int combined_transport_limit_kbps = limits_.combined_transport_bitrate_kbps(p.target);
    if (!target_is_dvd(p.target) && combined_transport_limit_kbps < 1000)
        throw std::runtime_error("combined transport bitrate limit must be at least 1000 kb/s");

    struct ProgressReporter {
        ProgressCallback callback;
        double last_percent = 0.0;
        std::mutex mutex;
        explicit ProgressReporter(ProgressCallback cb_in):callback(std::move(cb_in)){}
        void emit(double percent, const std::string& message) {
            std::lock_guard lock(mutex);
            percent = std::clamp(percent, last_percent, 100.0);
            last_percent = percent;
            if (callback) callback(percent, message);
        }
    } reporter{std::move(cb)};

    EncodingWorkerPool encoding_pool(tools_, encode_progress);
    auto encode_on_pool = [&](const std::string& job, const fs::path& input, const fs::path& output_base,
                              const EncodingProfile& encoding, const VideoTimingMode& timing,
                              bool source_interlaced, bool still, double seconds, double expected_duration,
                              const VideoProgressCallback& overall_progress, const Rgba& background_color,
                              bool loop_input, const fs::path& overlay_image, bool hold_last_frame,
                              int peak_bitrate_limit_kbps, const std::string& requested_worker = {},
                              EncodingWorkerAffinityMode affinity_mode = EncodingWorkerAffinityMode::Automatic,
                              bool prefer_ffvhuff_remote_relay = false) -> std::string {
        const std::string codec=video_codec_name(encoding.video_codec);
        return encoding_pool.run(job,codec,requested_worker,affinity_mode,[&](const ToolPaths& worker_tools,const std::string& worker_name){
            return encode_video(worker_tools,input,output_base,encoding,p,timing,source_interlaced,still,seconds,expected_duration,
                [&](double fraction,int pass,int passes,double fps,double speed){
                    if(overall_progress)overall_progress(fraction,pass,passes,fps,speed);
                    if(encode_progress){EncodeJobProgress ev;ev.worker=worker_name;ev.job=job;ev.codec=codec;ev.status="running";ev.pass=pass;ev.passes=passes;ev.fraction=fraction;ev.fps=fps;ev.speed=speed;encode_progress(ev);}
                },background_color,loop_input,overlay_image,hold_last_frame,peak_bitrate_limit_kbps,prefer_ffvhuff_remote_relay);
        });
    };
    auto encode_audio_on_pool = [&](const std::string& job, const fs::path& source, const fs::path& dir,
                                    const EncodingProfile& encoding, DiscTarget target, const std::string& language,
                                    bool loop, double seconds, int ordinal, int requested_channels,
                                    double expected_duration, const std::function<void(double)>& overall_progress,
                                    const std::string& requested_worker = {},
                                    EncodingWorkerAffinityMode affinity_mode = EncodingWorkerAffinityMode::Automatic,
                                    int input_stream_index = -1) -> EncodedAudioResult {
        std::string audio_worker_codec=encoding.audio_codec==AudioCodec::Libhdma?"hdma":"audio-"+audio_codec_name(encoding.audio_codec);
#if defined(BDMVAUTHOR_HAVE_EAC371ENC)
        if(encoding.audio_codec==AudioCodec::Ac3){
            const int source_channels=audio_channels(tools_,source,dir/"eac371-worker-channels",ordinal,input_stream_index);
            const int effective_channels=resolved_audio_output_channels(source_channels,encoding,target,requested_channels,"audio stream");
            if(effective_channels==8 && target_supports_ac3_71_extension(target))audio_worker_codec="eac371";
        }
#endif
        return encoding_pool.run(job,audio_worker_codec,requested_worker,affinity_mode,[&](const ToolPaths& worker_tools,const std::string& worker_name){
            return encode_audio(worker_tools,source,dir,encoding,target,language,loop,seconds,ordinal,requested_channels,expected_duration,
                [&](double fraction){
                    if(overall_progress)overall_progress(fraction);
                    if(encode_progress){EncodeJobProgress ev;ev.worker=worker_name;ev.job=job;ev.codec=audio_worker_codec;ev.status="running";ev.pass=1;ev.passes=1;ev.fraction=fraction;encode_progress(ev);}
                },input_stream_index);
        });
    };
    auto encode_dvd_audio_on_pool = [&](const std::string& job, const fs::path& source, const fs::path& dir,
                                        const EncodingProfile& encoding, bool loop, double seconds, int ordinal,
                                        int requested_channels, double expected_duration,
                                        const std::function<void(double)>& overall_progress,
                                        const std::string& requested_worker = {},
                                        EncodingWorkerAffinityMode affinity_mode = EncodingWorkerAffinityMode::Automatic,
                                        int input_stream_index = -1) -> DvdAudioElementary {
        const std::string audio_worker_codec="audio-"+audio_codec_name(encoding.audio_codec);
        return encoding_pool.run(job,audio_worker_codec,requested_worker,affinity_mode,[&](const ToolPaths& worker_tools,const std::string& worker_name){
            return encode_dvd_audio_elementary(worker_tools,source,dir,encoding,loop,seconds,ordinal,requested_channels,expected_duration,
                [&](double fraction){
                    if(overall_progress)overall_progress(fraction);
                    if(encode_progress){EncodeJobProgress ev;ev.worker=worker_name;ev.job=job;ev.codec=audio_worker_codec;ev.status="running";ev.pass=1;ev.passes=1;ev.fraction=fraction;encode_progress(ev);}
                },input_stream_index);
        });
    };


    fs::path work = p.work_directory;
    if (work.empty()) {
        fs::path temporary_root = storage_.temporary_root.empty() ? default_temporary_root() : storage_.temporary_root;
        if (temporary_root.empty()) throw std::runtime_error("temporary-files directory could not be determined");
        std::error_code temp_ec;
        fs::create_directories(temporary_root, temp_ec);
        if (temp_ec) throw std::runtime_error("cannot create temporary-files directory: " + temporary_root.string());
        auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        work = temporary_root / ("bdmvauthor-" + std::to_string(stamp));
    }
    fs::remove_all(work);
    fs::create_directories(work);

    const auto output_stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path output_parent = p.output_image.parent_path().empty() ? fs::path(".") : p.output_image.parent_path();
    fs::create_directories(output_parent);
    const fs::path temporary_output_image = output_parent /
        ("." + p.output_image.filename().string() + ".rendering-" + std::to_string(output_stamp));
    { std::error_code ec; fs::remove(temporary_output_image, ec); }
    RenderTempCleanup render_cleanup{work, temporary_output_image, p.keep_work_directory};

    const bool application_cache_enabled = storage_.encode_cache_policy != EncodeCachePolicy::Disabled;
    const bool wants_any_cache = application_cache_enabled && (p.use_encode_cache || p.use_compliance_cache);
    const fs::path shared_cache_root = wants_any_cache
        ? (storage_.cache_root.empty() ? default_encode_cache_root() : storage_.cache_root)
        : fs::path{};
    const fs::path encode_cache_root = (p.use_encode_cache && application_cache_enabled) ? shared_cache_root : fs::path{};
    const fs::path compliance_cache_root = (p.use_compliance_cache && application_cache_enabled) ? shared_cache_root : fs::path{};
    auto video_encode_cache_root = [&](const EncodingProfile& encoding) -> fs::path {
        if (encode_cache_root.empty()) return {};
        if (storage_.encode_cache_policy == EncodeCachePolicy::DisableForHardwareEncodes &&
            video_codec_is_hardware(encoding.video_codec)) return {};
        return encode_cache_root;
    };
    if (wants_any_cache && shared_cache_root.empty())
        reporter.emit(0.5, "Cache unavailable because the configured cache directory could not be determined; continuing without persistent cache");
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
            const auto codec = external_subtitle_codec(attached.source);
            const auto style = attached.style.override_style ? attached.style : p.titles[i].subtitle_default_style;
            title_tracks.back().subtitles.push_back({-1, codec,
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
        // Validate source ordinals against the unfiltered probe catalog first.
        // Removal settings are keyed by source ordinal, while authored stream
        // numbers are compacted after removed rows are erased below.
        for (const auto& stream : p.titles[i].audio_stream_settings) {
            const bool found = std::any_of(title_tracks[i].audio.begin(), title_tracks[i].audio.end(),
                [&](const SourceTrackDescriptor& track) { return track.source_ordinal == stream.source_ordinal; });
            if (!found) throw std::runtime_error("title " + std::to_string(i + 1U) +
                " audio stream setting refers to missing source stream " + std::to_string(stream.source_ordinal + 1));
        }
        for (const auto& stream : p.titles[i].subtitle_stream_settings) {
            const bool found = std::any_of(title_tracks[i].subtitles.begin(), title_tracks[i].subtitles.end(),
                [&](const SourceTrackDescriptor& track) { return track.source_ordinal == stream.source_ordinal; });
            if (!found) throw std::runtime_error("title " + std::to_string(i + 1U) +
                " subtitle stream setting refers to a missing or unsupported source subtitle stream " +
                std::to_string(stream.source_ordinal + 1));
        }

        auto& catalog = title_tracks[i];
        catalog.audio.erase(std::remove_if(catalog.audio.begin(), catalog.audio.end(), [&](const SourceTrackDescriptor& track) {
            return audio_stream_removed(p.titles[i], track.source_ordinal);
        }), catalog.audio.end());
        catalog.subtitles.erase(std::remove_if(catalog.subtitles.begin(), catalog.subtitles.end(), [&](const SourceTrackDescriptor& track) {
            return track.source_ordinal >= 0 && subtitle_stream_removed(p.titles[i], track.source_ordinal);
        }), catalog.subtitles.end());
        if (target_is_avchd(p.target) && !catalog.subtitles.empty())
            throw std::runtime_error("AVCHD export currently supports video and audio only; remove all subtitle streams and attachments");

        if(p.titles[i].default_audio_stream<0||p.titles[i].default_audio_stream>static_cast<int>(catalog.audio.size()))
            throw std::runtime_error("title "+std::to_string(i+1U)+" default audio selection is unavailable after stream removal");
        if(p.titles[i].default_subtitle_stream<-1||p.titles[i].default_subtitle_stream>static_cast<int>(catalog.subtitles.size()))
            throw std::runtime_error("title "+std::to_string(i+1U)+" default subtitle selection is unavailable after stream removal");
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
    // Auto-size is a genuine two-stage build: finish EVERY menu and title audio
    // track first, then use measured elementary-stream bytes for the disc budget.
    // Preflight results are reused by muxing; no audio is encoded a second time.
    struct PreparedAudioAsset {
        fs::path path;
        int peak_kbps = 0;
        std::uintmax_t bytes = 0;
        int channels = 0;
        AudioCodec codec = AudioCodec::Ac3;
        int sample_rate = 48000;
        int bit_depth = 16;
    };
    std::vector<std::optional<PreparedAudioAsset>> prepared_menu_audio(menus.size());
    std::vector<std::vector<std::optional<PreparedAudioAsset>>> prepared_title_audio(p.titles.size());
    if (p.automatic_video_bitrate) {
        const bool dvd = target_is_dvd(p.target);
        const bool dvd_menu_audio = dvd && std::any_of(menus.begin(), menus.end(),
            [](const ResolvedMenu& rm) { return !rm.menu.audio_source.empty(); });
        AudioCodec dvd_menu_codec = AudioCodec::Ac3;
        bool dvd_menu_codec_chosen = false;
        for (const auto& rm : menus) {
            if (!dvd || rm.menu.audio_source.empty()) continue;
            const auto codec = encoding_for_target(rm.menu, p.target).audio_codec;
            if (!dvd_menu_codec_chosen) { dvd_menu_codec = codec; dvd_menu_codec_chosen = true; }
            else if (dvd_menu_codec != codec)
                throw std::runtime_error("all DVD VMGM menus with audio must use the same audio codec (AC-3 or LPCM)");
        }
        double total_audio_work = 0.0;
        const auto audio_work = [](double duration) { return std::max(1.0, duration); };
        for (const auto& rm : menus)
            if (!rm.menu.audio_source.empty() || dvd_menu_audio)
                total_audio_work += audio_work(rm.menu.duration_seconds);
        for (std::size_t ti = 0; ti < p.titles.size(); ++ti) {
            prepared_title_audio[ti].resize(title_tracks[ti].audio.size());
            total_audio_work += audio_work(title_durations[ti]) * static_cast<double>(title_tracks[ti].audio.size());
        }
        double completed_audio_work = 0.0;
        auto audio_percent = [&](double units) {
            return 2.0 + 16.0 * std::clamp(units / std::max(1.0, total_audio_work), 0.0, 1.0);
        };
        auto audio_progress = [&](double units, std::string label) {
            const double before = completed_audio_work;
            return [&, before, units, label = std::move(label)](double fraction) {
                std::ostringstream text;
                text << "Audio encoding: " << label << " — " << std::fixed << std::setprecision(1)
                     << 100.0 * std::clamp(fraction, 0.0, 1.0) << "%";
                reporter.emit(audio_percent(before + units * std::clamp(fraction, 0.0, 1.0)), text.str());
            };
        };
        auto finished_audio = [&](double units, const std::string& label) {
            completed_audio_work += units;
            reporter.emit(audio_percent(completed_audio_work), "Audio encoding: " + label + " — 100.0%");
        };
        auto asset = [](const fs::path& path, int peak, int channels, AudioCodec codec, int rate, int depth) {
            if (!cache_file_usable(path)) throw std::runtime_error("prepared audio is empty or missing: " + path.string());
            return PreparedAudioAsset{path, peak, fs::file_size(path), channels, codec, rate, depth};
        };
        reporter.emit(2.0, "Audio encoding: preparing all title and menu audio before sizing video — 0.0%");
        for (std::size_t mi = 0; mi < menus.size(); ++mi) {
            const auto& menu = menus[mi].menu;
            if (menu.audio_source.empty() && !dvd_menu_audio) continue;
            auto encoding = encoding_for_target(menu, p.target);
            const double units = audio_work(menu.duration_seconds);
            const std::string label = "menu " + std::to_string(mi + 1U) + "/" + std::to_string(menus.size());
            auto progress = audio_progress(units, label);
            const fs::path dir = work / "audio-preflight" / ("menu-" + std::to_string(mi + 1U));
            fs::create_directories(dir);
            if (dvd) {
                encoding.audio_codec = dvd_menu_codec;
                if (!limits_.allow_exceeding_format_limits && dvd_menu_codec == AudioCodec::Ac3)
                    encoding.ac3_bitrate_kbps = std::min(encoding.ac3_bitrate_kbps, 448);
                const int source_channels = menu.audio_source.empty() ? 2 :
                    audio_channels(tools_, menu.audio_source, dir / "channels", 0);
                const int channels = resolved_audio_output_channels(source_channels, encoding, p.target, 0, "DVD menu audio");
                fs::path output;
                if (menu.audio_source.empty()) {
                    output = encode_dvd_silence(tools_, dir, encoding, menu.duration_seconds, progress).path;
                } else {
                    fs::path cached;
                    if (!encode_cache_root.empty()) {
                        cached = encode_cache_root / "menu-audio" /
                            (menu_audio_cache_key(source_fingerprint(menu.audio_source), encoding, p.target,
                                menu.duration_seconds, true, menu.loop_media, tools_) + dvd_audio_cache_extension(encoding.audio_codec));
                    }
                    const auto meta = audio_cache_metadata("menu-audio", menu.audio_source, p.target, encoding, channels, 0);
                    if (!cached.empty() && !p.force_reencode && !p.refresh_encode_cache && cache_file_usable(cached)) {
                        touch_cache_metadata(cached, meta);
                        output = cached;
                    } else if (!cached.empty()) {
                        const auto tmp = cache_encode_temp_dir(cached);
                        CacheTempDirCleanup cleanup{tmp};
                        const auto encoded = encode_dvd_audio_on_pool("DVD menu audio "+std::to_string(mi+1U), menu.audio_source, tmp, encoding,
                            menu.loop_media, menu.duration_seconds, 0, 0, menu.duration_seconds, progress, menu.encoding_worker, menu.encoding_worker_mode);
                        finalize_encoded_cache_file(encoded.path, cached, p.force_reencode || p.refresh_encode_cache, meta);
                        output = cached;
                    } else {
                        output = encode_dvd_audio_on_pool("DVD menu audio "+std::to_string(mi+1U), menu.audio_source, dir, encoding,
                            menu.loop_media, menu.duration_seconds, 0, 0, menu.duration_seconds, progress, menu.encoding_worker, menu.encoding_worker_mode).path;
                    }
                }
                prepared_menu_audio[mi] = asset(output, configured_audio_rate_kbps(encoding, p.target, channels),
                    channels, encoding.audio_codec, encoding.audio_codec == AudioCodec::Lpcm ? encoding.lpcm_sample_rate_hz : 48000,
                    encoding.audio_codec == AudioCodec::Lpcm ? encoding.lpcm_bit_depth : 16);
            } else {
                const int source_channels = audio_channels(tools_, menu.audio_source, dir / "channels", 0);
                const int channels = resolved_audio_output_channels(source_channels, encoding, p.target, 0, "menu audio");
                const int fixed_peak = fixed_encoded_audio_peak_kbps(encoding, p.target, channels);
                const auto menu_audio_fp = source_fingerprint(menu.audio_source);
                const auto cache = encoded_cache_paths(
                    encode_cache_root, "menu-audio",
                    menu_audio_cache_key(menu_audio_fp, encoding, p.target, menu.duration_seconds, false, menu.loop_media, tools_),
                    menu_audio_cache_key_remote_location_compat(menu_audio_fp, encoding, p.target, menu.duration_seconds, false, menu.loop_media, tools_),
                    audio_extension(encoding), !p.force_reencode && !p.refresh_encode_cache);
                const auto base_meta = audio_cache_metadata("menu-audio", menu.audio_source, p.target, encoding, channels, 0);
                const int cached_peak = cache.hit.empty() ? 0 : cached_audio_peak_kbps(
                    tools_, cache.hit, fixed_peak, dir / "cached-peak-bitrate.txt", base_meta);
                const bool hit = !cache.hit.empty() && cached_peak > 0;
                fs::path output;
                int peak = cached_peak;
                if (hit) {
                    auto hit_meta = base_meta; hit_meta.peak_bitrate_kbps = peak;
                    touch_cache_metadata(cache.hit, hit_meta);
                    output = cache.hit;
                } else if (!cache.write.empty()) {
                    const auto tmp = cache_encode_temp_dir(cache.write);
                    CacheTempDirCleanup cleanup{tmp};
                    const auto encoded = encode_audio_on_pool("Menu audio "+std::to_string(mi+1U), menu.audio_source, tmp, encoding, p.target,
                        "eng", menu.loop_media, menu.duration_seconds, 0, 0, menu.duration_seconds, progress);
                    peak = encoded.peak_bitrate_kbps;
                    finalize_encoded_cache_file(encoded.path, cache.write, p.force_reencode || p.refresh_encode_cache,
                        audio_cache_metadata("menu-audio", menu.audio_source, p.target, encoding, channels, 0, peak));
                    output = cache.write;
                } else {
                    const auto encoded = encode_audio_on_pool("Menu audio "+std::to_string(mi+1U), menu.audio_source, dir, encoding, p.target,
                        "eng", menu.loop_media, menu.duration_seconds, 0, 0, menu.duration_seconds, progress);
                    output = encoded.path;
                    peak = encoded.peak_bitrate_kbps;
                }
                prepared_menu_audio[mi] = asset(output, peak, channels, encoding.audio_codec, 0, 0);
            }
            finished_audio(units, label);
        }
        for (std::size_t ti = 0; ti < p.titles.size(); ++ti) {
            const auto& title = p.titles[ti];
            const bool force_reencode = p.force_reencode || title.force_reencode;
            for (std::size_t ai = 0; ai < title_tracks[ti].audio.size(); ++ai) {
                const auto& track = title_tracks[ti].audio[ai];
                const auto encoding = effective_audio_encoding_for_stream(title, p.target, track.source_ordinal);
                validate_encoding_for_target(encoding, p.target,
                    "title " + std::to_string(ti + 1U) + " audio track " + std::to_string(ai + 1U),
                    limits_.allow_exceeding_format_limits);
                const auto* settings = find_audio_stream_settings(title, track.source_ordinal);
                const std::string audio_worker=(settings&&!settings->encoding_worker.empty())?settings->encoding_worker:title.encoding_worker;
                const auto audio_affinity=(settings&&!settings->encoding_worker.empty())?settings->encoding_worker_mode:title.encoding_worker_mode;
                const int requested = settings ? settings->output_channels : 0;
                const double units = audio_work(title_durations[ti]);
                const std::string label = "title " + std::to_string(ti + 1U) + "/" + std::to_string(p.titles.size()) +
                    " track " + std::to_string(ai + 1U) + "/" + std::to_string(title_tracks[ti].audio.size());
                auto progress = audio_progress(units, label);
                const fs::path dir = work / "audio-preflight" / ("title-" + std::to_string(ti + 1U)) /
                    ("track-" + std::to_string(ai + 1U));
                fs::create_directories(dir);
                const int src_channels = audio_channels(tools_, title.source, dir / "channels", track.source_ordinal, track.input_stream_index);
                const int channels = resolved_audio_output_channels(src_channels, encoding, p.target, requested, label);
                fs::path output;
                int peak = 0;
                if (dvd) {
                    fs::path cached;
                    if (!encode_cache_root.empty())
                        cached = encode_cache_root / "audio" /
                            (dvd_audio_cache_key(source_fingerprint(title.source), encoding,
                                track.source_ordinal, requested) + dvd_audio_cache_extension(encoding.audio_codec));
                    const auto meta = audio_cache_metadata("title-audio", title.source, p.target, encoding,
                        channels, track.source_ordinal);
                    if (!cached.empty() && !force_reencode && !p.refresh_encode_cache && cache_file_usable(cached)) {
                        touch_cache_metadata(cached, meta);
                        output = cached;
                    } else if (!cached.empty()) {
                        const auto tmp = cache_encode_temp_dir(cached);
                        CacheTempDirCleanup cleanup{tmp};
                        const auto encoded = encode_dvd_audio_on_pool("DVD title audio "+std::to_string(ti+1U)+"/"+std::to_string(ai+1U), title.source, tmp, encoding,
                            false, 0, track.source_ordinal, requested, title_durations[ti], progress, audio_worker, audio_affinity, track.input_stream_index);
                        finalize_encoded_cache_file(encoded.path, cached, force_reencode || p.refresh_encode_cache, meta);
                        output = cached;
                    } else {
                        output = encode_dvd_audio_on_pool("DVD title audio "+std::to_string(ti+1U)+"/"+std::to_string(ai+1U), title.source, dir, encoding,
                            false, 0, track.source_ordinal, requested, title_durations[ti], progress, audio_worker, audio_affinity, track.input_stream_index).path;
                    }
                    peak = configured_audio_rate_kbps(encoding, p.target, channels);
                } else {
                    const int fixed_peak = fixed_encoded_audio_peak_kbps(encoding, p.target, channels);
                    const auto title_audio_fp = source_fingerprint(title.source);
                    const auto cache = encoded_cache_paths(
                        encode_cache_root, "audio",
                        audio_cache_key(title_audio_fp, encoding, track.source_ordinal, requested, tools_),
                        audio_cache_key_remote_location_compat(title_audio_fp, encoding, track.source_ordinal, requested, tools_),
                        audio_extension(encoding), !force_reencode && !p.refresh_encode_cache);
                    const auto base_meta = audio_cache_metadata("title-audio", title.source, p.target,
                        encoding, channels, track.source_ordinal);
                    const int cached_peak = cache.hit.empty() ? 0 : cached_audio_peak_kbps(
                        tools_, cache.hit, fixed_peak, dir / "cached-peak-bitrate.txt", base_meta);
                    const bool hit = !cache.hit.empty() && cached_peak > 0;
                    peak = cached_peak;
                    if (hit) {
                        auto hit_meta = base_meta; hit_meta.peak_bitrate_kbps = peak;
                        touch_cache_metadata(cache.hit, hit_meta);
                        output = cache.hit;
                    } else if (!cache.write.empty()) {
                        const auto tmp = cache_encode_temp_dir(cache.write);
                        CacheTempDirCleanup cleanup{tmp};
                        const auto encoded = encode_audio_on_pool("Audio "+label, title.source, tmp, encoding, p.target,
                            effective_audio_language(title, track, ai), false, 0, track.source_ordinal,
                            requested, title_durations[ti], progress, audio_worker, audio_affinity, track.input_stream_index);
                        peak = encoded.peak_bitrate_kbps;
                        finalize_encoded_cache_file(encoded.path, cache.write, force_reencode || p.refresh_encode_cache,
                            audio_cache_metadata("title-audio", title.source, p.target, encoding,
                                channels, track.source_ordinal, peak));
                        output = cache.write;
                    } else {
                        const auto encoded = encode_audio_on_pool("Audio "+label, title.source, dir, encoding, p.target,
                            effective_audio_language(title, track, ai), false, 0, track.source_ordinal,
                            requested, title_durations[ti], progress, audio_worker, audio_affinity, track.input_stream_index);
                        peak = encoded.peak_bitrate_kbps;
                        output = encoded.path;
                    }
                }
                prepared_title_audio[ti][ai] = asset(output, peak, channels, encoding.audio_codec,
                    encoding.audio_codec == AudioCodec::Lpcm ? encoding.lpcm_sample_rate_hz : 48000,
                    encoding.audio_codec == AudioCodec::Lpcm ? encoding.lpcm_bit_depth : 16);
                finished_audio(units, label);
            }
        }
        reporter.emit(18.0, "Audio encoding: all title and menu audio prepared — 100.0%");
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
                                           int source_ordinal, int input_stream_index, int requested_channels, double duration,
                                           const fs::path& scratch, const std::string& where) {
        EncodingProfile resolved = configured;
        int source_channels = 2;
        if (!source.empty()) source_channels = audio_channels(tools_, source, scratch, source_ordinal, input_stream_index);
        if (lossless_audio_codec(resolved.audio_codec) && !source.empty()) {
            const auto info = probe_audio_stream(tools_, source, scratch / "format", source_ordinal, duration, input_stream_index);
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
        if (p.automatic_video_bitrate && prepared_menu_audio[mi]) {
            menu_audio_kbps[mi] = prepared_menu_audio[mi]->peak_kbps;
            capacity_plan.audio_kbit_seconds += static_cast<double>(prepared_menu_audio[mi]->bytes) * 8.0 / 1000.0;
        } else {
            if (!menu.audio_source.empty() || dvd_any_menu_audio) {
                menu_audio_kbps[mi] = estimate_encoded_audio_kbps(
                    menu.audio_source, menu_encoding, 0, -1, 0, menu.duration_seconds,
                    probe_dir / ("capacity-menu-audio-" + std::to_string(mi + 1U)),
                    "menu '" + menu.name + "' audio");
            }
            capacity_plan.audio_kbit_seconds += static_cast<double>(menu_audio_kbps[mi]) * menu.duration_seconds;
        }
    }
    for (std::size_t ti = 0; ti < p.titles.size(); ++ti) {
        const auto& title = p.titles[ti];
        int audio_total = 0;
        for (std::size_t ai = 0; ai < title_tracks[ti].audio.size(); ++ai) {
            const auto& track = title_tracks[ti].audio[ai];
            if (p.automatic_video_bitrate && prepared_title_audio[ti][ai]) {
                audio_total += prepared_title_audio[ti][ai]->peak_kbps;
                capacity_plan.audio_kbit_seconds += static_cast<double>(prepared_title_audio[ti][ai]->bytes) * 8.0 / 1000.0;
            } else {
                const auto stream_encoding = effective_audio_encoding_for_stream(title, p.target, track.source_ordinal);
                const auto* settings = find_audio_stream_settings(title, track.source_ordinal);
                const int estimated_rate = estimate_encoded_audio_kbps(
                    title.source, stream_encoding, track.source_ordinal, track.input_stream_index,
                    settings ? settings->output_channels : 0, title_durations[ti],
                    probe_dir / ("capacity-title-" + std::to_string(ti + 1U) + "-audio-" + std::to_string(track.source_ordinal + 1)),
                    "title " + std::to_string(ti + 1U) + " audio stream " + std::to_string(track.source_ordinal + 1));
                audio_total += estimated_rate;
                capacity_plan.audio_kbit_seconds += static_cast<double>(estimated_rate) * title_durations[ti];
            }
        }
        title_audio_kbps[ti] = audio_total;
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
            return dvd_mplex_safe_video_bitrate_kbps(
                combined_transport_limit_kbps, std::max(0, audio_kbps), codec_max);
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
        if (p.automatic_video_bitrate) message << " (measured all audio elementary streams)";
        reporter.emit(p.automatic_video_bitrate ? 18.0 : 1.0, message.str());
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
        reporter.emit(p.automatic_video_bitrate ? 18.0 : 2.0, "Authoring DVD-Video MPEG-2/VOB assets — " +
            std::string(dvd_disc_pal ? "PAL 625/50 family" : "NTSC/Film 525/60 family"));

        const auto pass_count = [](const EncodingProfile& e) { return e.two_pass ? 2.0 : 1.0; };
        const auto work_duration = [](double seconds) { return seconds > 0.0 ? seconds : 1.0; };
        double total_dvd_video_work = 0.0;
        constexpr double DvdMuxWorkFactor = 0.12;
        constexpr double DvdAudioWorkFactor = 0.15;
        const bool project_has_dvd_menu_audio = std::any_of(menus.begin(), menus.end(),
            [](const ResolvedMenu& rm) { return !rm.menu.audio_source.empty(); });
        for (const auto& rm : menus)
            total_dvd_video_work += work_duration(rm.menu.duration_seconds) *
                (pass_count(encoding_for_target(rm.menu, p.target)) + DvdMuxWorkFactor +
                (!p.automatic_video_bitrate && project_has_dvd_menu_audio ? DvdAudioWorkFactor : 0.0));
        for (std::size_t i = 0; i < p.titles.size(); ++i)
            total_dvd_video_work += work_duration(title_durations[i]) *
                (pass_count(encoding_for_target(p.titles[i], p.target)) + DvdMuxWorkFactor +
                (!p.automatic_video_bitrate ? DvdAudioWorkFactor * static_cast<double>(title_tracks[i].audio.size()) : 0.0));
        double completed_dvd_video_work = 0.0;
        auto dvd_video_percent = [&](double work_done) {
            const double start = p.automatic_video_bitrate ? 18.0 : 3.0;
            if (total_dvd_video_work <= 0.0) return start;
            return start + (78.0 - start) * std::clamp(work_done / total_dvd_video_work, 0.0, 1.0);
        };
        auto dvd_progress_for_video = [&](double units, std::string label) -> VideoProgressCallback {
            const double before = completed_dvd_video_work;
            return [&, before, units, label = std::move(label)](double fraction, int pass, int passes, double, double) {
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
            const auto background = motion ? menu.background_video : prepare_menu_background(tools_, menu, md, dvd_timing);
            const auto label_overlay = motion ? prepare_menu_overlay(tools_, menu, md) : fs::path{};
            const auto combined_overlay = combine_rgba_overlays(tools_, label_overlay, visuals.normal_overlay,
                                                                 geometry.video_width, geometry.video_height, md);
            VideoStreamInfo timing_source;
            if (motion) timing_source = probe_video_timing_metadata(tools_, background, md / "timing.txt");
            else { timing_source.frame_rate = 30000.0 / 1001.0; timing_source.field_order = "progressive"; }
            const auto& timing = dvd_timing;
            reporter.emit(dvd_video_percent(completed_dvd_video_work), encode_label + " — output timing " + timing.description);
            fs::path video;
            const auto menu_video_cache = encoded_cache_paths(
                encode_cache_root, "menu-video",
                menu_video_cache_key(background, combined_overlay, encoding, p, timing, tools_,
                                     timing_source_interlaced(timing_source), !motion, menu.duration_seconds,
                                     menu.background_color, menu.loop_media, dvd_video_peak_kbps),
                menu_video_cache_key_remote_location_compat(background, combined_overlay, encoding, p, timing, tools_,
                                                            timing_source_interlaced(timing_source), !motion, menu.duration_seconds,
                                                            menu.background_color, menu.loop_media, dvd_video_peak_kbps),
                video_extension(encoding), !p.force_reencode && !p.refresh_encode_cache);
            const auto menu_video_metadata = video_cache_metadata(
                "menu-video", motion ? source_menu.background_video : source_menu.background_image,
                p.target, encoding, timing, tools_);
            if (!menu_video_cache.hit.empty()) {
                video = menu_video_cache.hit;
                touch_cache_metadata(menu_video_cache.hit, menu_video_metadata);
                reporter.emit(dvd_video_percent(completed_dvd_video_work + units),
                              "Using cached encoded video for DVD menu " + std::to_string(mi + 1U));
            } else if (!menu_video_cache.write.empty()) {
                const auto cache_tmp_dir = cache_encode_temp_dir(menu_video_cache.write);
                CacheTempDirCleanup cache_tmp_cleanup{cache_tmp_dir};
                const auto cache_tmp_base = cache_tmp_dir / "video";
                (void)encode_on_pool("DVD menu "+std::to_string(mi+1U), background, cache_tmp_base, encoding, timing,
                                   timing_source_interlaced(timing_source), !motion, menu.duration_seconds,
                                   menu.duration_seconds, dvd_progress_for_video(units, encode_label),
                                   menu.background_color, motion && menu.loop_media, combined_overlay, motion && !menu.loop_media,
                                   dvd_video_peak_kbps, source_menu.encoding_worker, source_menu.encoding_worker_mode, true);
                const fs::path cache_tmp_output = cache_tmp_base.string() + video_extension(encoding);
                if (fs::exists(fallback_filter_marker(cache_tmp_base))) {
                    video = preserve_software_filter_fallback(cache_tmp_output, md / "video", video_extension(encoding));
                } else {
                    finalize_encoded_cache_file(cache_tmp_output, menu_video_cache.write, p.force_reencode || p.refresh_encode_cache, menu_video_metadata);
                    video = menu_video_cache.write;
                }
            } else {
                (void)encode_on_pool("DVD menu "+std::to_string(mi+1U), background, md / "video", encoding, timing,
                                   timing_source_interlaced(timing_source), !motion, menu.duration_seconds,
                                   menu.duration_seconds, dvd_progress_for_video(units, encode_label),
                                   menu.background_color, motion && menu.loop_media, combined_overlay, motion && !menu.loop_media,
                                   dvd_video_peak_kbps, source_menu.encoding_worker, source_menu.encoding_worker_mode, true);
                video = md / "video.m2v";
            }
            completed_dvd_video_work += units;

            std::vector<DvdAudioElementary> audio;
            const double manual_audio_units = !p.automatic_video_bitrate && any_menu_audio ?
                work_duration(menu.duration_seconds) * DvdAudioWorkFactor : 0.0;
            const double audio_progress_start = completed_dvd_video_work;
            auto dvd_menu_audio_progress = [&, audio_progress_start](double fraction) {
                std::ostringstream message;
                message << "Audio encoding: DVD menu " << mi + 1U << "/" << menus.size() << " — "
                        << std::fixed << std::setprecision(1) << fraction * 100.0 << "%";
                reporter.emit(dvd_video_percent(audio_progress_start + manual_audio_units * fraction), message.str());
            };
            if (any_menu_audio) {
                auto ad = md / "audio";
                if (p.automatic_video_bitrate && prepared_menu_audio[mi]) {
                    const auto& prepared = *prepared_menu_audio[mi];
                    audio.push_back({prepared.path, prepared.channels, prepared.codec,
                        prepared.sample_rate, prepared.bit_depth});
                } else if (menu.audio_source.empty()) {
                    audio.push_back(encode_dvd_silence(tools_, ad, menu_audio_encoding, menu.duration_seconds, dvd_menu_audio_progress));
                } else if (!encode_cache_root.empty()) {
                    const auto& audio_fp = source_fingerprint(menu.audio_source);
                    const auto audio_cache_file = encode_cache_root / "menu-audio" /
                        (menu_audio_cache_key(audio_fp, menu_audio_encoding, p.target, menu.duration_seconds, true, menu.loop_media, tools_) +
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
                        const auto encoded = encode_dvd_audio_on_pool("DVD menu audio "+std::to_string(mi+1U), menu.audio_source, cache_tmp_dir,
                                                                         menu_audio_encoding, menu.loop_media, menu.duration_seconds, 0, 0,
                                                                         menu.duration_seconds, dvd_menu_audio_progress, menu.encoding_worker, menu.encoding_worker_mode);
                        finalize_encoded_cache_file(encoded.path, audio_cache_file, p.force_reencode || p.refresh_encode_cache, menu_audio_metadata);
                        audio.push_back({audio_cache_file, encoded.channels, encoded.codec, encoded.sample_rate_hz, encoded.bit_depth});
                    }
                } else {
                    audio.push_back(encode_dvd_audio_on_pool("DVD menu audio "+std::to_string(mi+1U), menu.audio_source, ad, menu_audio_encoding,
                                                                 menu.loop_media, menu.duration_seconds, 0, 0,
                                                                 menu.duration_seconds, dvd_menu_audio_progress, menu.encoding_worker, menu.encoding_worker_mode));
                }
            }
            if (manual_audio_units > 0.0) {
                completed_dvd_video_work += manual_audio_units;
                dvd_menu_audio_progress(1.0);
            }
            const auto base_vob = md / "menu-base.mpg";
            const double mux_units = work_duration(menu.duration_seconds) * DvdMuxWorkFactor;
            reporter.emit(dvd_video_percent(completed_dvd_video_work), "Muxing DVD menu " + std::to_string(mi + 1U) + " — 0.0%");
            mux_dvd_program_stream(tools_, video, audio, base_vob, combined_transport_limit_kbps,
                [&](double fraction) {
                    reporter.emit(dvd_video_percent(completed_dvd_video_work + mux_units * 0.5 * fraction),
                        "Muxing DVD menu " + std::to_string(mi + 1U) + " — " +
                        std::to_string(static_cast<int>(50.0 * fraction)) + "%");
                });
            const auto menu_vob = md / "menu.mpg";
            reporter.emit(dvd_video_percent(completed_dvd_video_work + mux_units * 0.5),
                          "Muxing DVD menu subtitles " + std::to_string(mi + 1U));
            spumux_dvd_menu(tools_, base_vob, menu_vob, md / "spumux-menu.xml", menu, visuals, dvd_timing);
            completed_dvd_video_work += mux_units;
            reporter.emit(dvd_video_percent(completed_dvd_video_work), "Muxing DVD menu complete");
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
                const int source_channels = audio_channels(tools_, title.source, ad, track.source_ordinal, track.input_stream_index);
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
            EncodedCachePaths video_cache;
            if (fp) {
                video_cache = encoded_cache_paths(
                    encode_cache_root, "video",
                    video_cache_key(*fp, encoding, p, timing, tools_, dvd_video_peak_kbps),
                    video_cache_key_remote_location_compat(*fp, encoding, p, timing, tools_, dvd_video_peak_kbps),
                    video_extension(encoding), !force_reencode && !p.refresh_encode_cache);
            }
            const auto title_video_metadata = video_cache_metadata("title-video", title.source, p.target, encoding, timing, tools_);
            if (!video_cache.hit.empty()) {
                video = video_cache.hit;
                touch_cache_metadata(video_cache.hit, title_video_metadata);
                reporter.emit(dvd_video_percent(completed_dvd_video_work + units), "Using cached encoded video for DVD title " + std::to_string(i + 1U));
            } else if (!video_cache.write.empty()) {
                const auto temp_dir = cache_encode_temp_dir(video_cache.write);
                CacheTempDirCleanup cleanup{temp_dir};
                const auto base = temp_dir / "video";
                (void)encode_on_pool("DVD title "+std::to_string(i+1U), title.source, base, encoding, timing,
                                   timing_source_interlaced(timing_source), false, 0, title_durations[i],
                                   dvd_progress_for_video(units, encode_label), {0,0,0,255}, false, {}, false,
                                   dvd_video_peak_kbps);
                const fs::path temp_output = base.string() + video_extension(encoding);
                if (fs::exists(fallback_filter_marker(base))) {
                    video = preserve_software_filter_fallback(temp_output, td / "video", video_extension(encoding));
                } else {
                    finalize_encoded_cache_file(temp_output, video_cache.write, force_reencode || p.refresh_encode_cache, title_video_metadata);
                    video = video_cache.write;
                }
            } else {
                const auto base = td / "video";
                (void)encode_on_pool("DVD title "+std::to_string(i+1U), title.source, base, encoding, timing,
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
                const std::string audio_worker=(stream_settings&&!stream_settings->encoding_worker.empty())?stream_settings->encoding_worker:title.encoding_worker;
                const auto audio_affinity=(stream_settings&&!stream_settings->encoding_worker.empty())?stream_settings->encoding_worker_mode:title.encoding_worker_mode;
                const int channels = planned_audio_channels.at(audio_plan_index++);
                const double manual_audio_units = p.automatic_video_bitrate ? 0.0 :
                    work_duration(title_durations[i]) * DvdAudioWorkFactor;
                const double audio_progress_start = completed_dvd_video_work;
                auto dvd_title_audio_progress = [&, audio_progress_start](double fraction) {
                    std::ostringstream message;
                    message << "Audio encoding: DVD title " << i + 1U << "/" << p.titles.size()
                            << " track " << audio_plan_index << "/" << title_tracks[i].audio.size() << " — "
                            << std::fixed << std::setprecision(1) << 100.0 * fraction << "%";
                    reporter.emit(dvd_video_percent(audio_progress_start + manual_audio_units * fraction), message.str());
                };
                if (p.automatic_video_bitrate && prepared_title_audio[i][audio_plan_index - 1U]) {
                    const auto& prepared = *prepared_title_audio[i][audio_plan_index - 1U];
                    audio.push_back({prepared.path, prepared.channels, prepared.codec,
                        prepared.sample_rate, prepared.bit_depth});
                    continue;
                }
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
                    const auto encoded = encode_dvd_audio_on_pool("DVD title audio "+std::to_string(i+1U)+"/"+std::to_string(audio_plan_index), title.source, temp_dir, stream_encoding, false, 0, track.source_ordinal, stream_settings ? stream_settings->output_channels : 0,
                                                                            title_durations[i], dvd_title_audio_progress, audio_worker, audio_affinity, track.input_stream_index);
                    finalize_encoded_cache_file(encoded.path, cached, force_reencode || p.refresh_encode_cache, title_audio_metadata);
                    audio.push_back({cached, encoded.channels, encoded.codec, encoded.sample_rate_hz, encoded.bit_depth});
                } else {
                    audio.push_back(encode_dvd_audio_on_pool("DVD title audio "+std::to_string(i+1U)+"/"+std::to_string(audio_plan_index), title.source, ad, stream_encoding, false, 0, track.source_ordinal, stream_settings ? stream_settings->output_channels : 0,
                                                                            title_durations[i], dvd_title_audio_progress, audio_worker, audio_affinity, track.input_stream_index));
                }
                if (manual_audio_units > 0.0) {
                    completed_dvd_video_work += manual_audio_units;
                    dvd_title_audio_progress(1.0);
                }
            }

            auto current = td / "title-base.mpg";
            const double mux_units = work_duration(title_durations[i]) * DvdMuxWorkFactor;
            reporter.emit(dvd_video_percent(completed_dvd_video_work), "Muxing DVD title " + std::to_string(i + 1U) + " — 0.0%");
            mux_dvd_program_stream(tools_, video, audio, current, combined_transport_limit_kbps,
                [&](double fraction) {
                    reporter.emit(dvd_video_percent(completed_dvd_video_work + mux_units * 0.7 * fraction),
                        "Muxing DVD title " + std::to_string(i + 1U) + " — " +
                        std::to_string(static_cast<int>(70.0 * fraction)) + "%");
                });
            for (std::size_t si = 0; si < title_tracks[i].subtitles.size(); ++si) {
                const auto sd = td / ("subtitle-" + std::to_string(si + 1U));
                const auto next = td / ("title-sub-" + std::to_string(si + 1U) + ".mpg");
                const auto& sub = title_tracks[i].subtitles[si];
                if (is_bitmap_subtitle(sub.codec_name)) {
                    spumux_dvd_bitmap_subtitle(tools_, current, next, sd, title.source, sub, si, timing);
                } else {
                    const auto srt = extract_dvd_text_subtitle(tools_, title.source, sd, sub);
                    spumux_dvd_text_subtitle(tools_, current, next, sd / "spumux.xml", srt, si, timing, sub.subtitle_style);
                }
                current = next;
                reporter.emit(dvd_video_percent(completed_dvd_video_work + mux_units *
                    (0.7 + 0.3 * static_cast<double>(si + 1U) / static_cast<double>(title_tracks[i].subtitles.size()))),
                    "Muxing DVD title subtitles " + std::to_string(si + 1U) + "/" +
                    std::to_string(title_tracks[i].subtitles.size()));
            }
            completed_dvd_video_work += mux_units;
            reporter.emit(dvd_video_percent(completed_dvd_video_work), "Muxing DVD title complete");
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
            // Bitmap subtitles have already been quantized into a fixed RGB cube.
            // Letting dvdauthor synthesize a new IFO CLUT from the per-caption
            // spumux palettes can produce varying foreground/outline colors on
            // disc even when the PNGs are correct. Supply the same fixed CLUT
            // that the quantizer uses, so local SPU indices always resolve to
            // predictable final colors. Do not constrain mixed/text subtitle
            // titles: their explicitly styled colors may be outside the cube.
            const bool fixed_bitmap_clut = !title_tracks[i].subtitles.empty() &&
                std::all_of(title_tracks[i].subtitles.begin(), title_tracks[i].subtitles.end(),
                    [](const SourceTrackDescriptor& sub) { return is_bitmap_subtitle(sub.codec_name); });
            fs::path bitmap_clut_file;
            if (fixed_bitmap_clut) {
                bitmap_clut_file = work / ("dvd-title-" + std::to_string(i + 1U) + "-subpicture.rgb");
                write_text(bitmap_clut_file, detail::dvd_subpicture_title_palette_rgb());
            }
            xml << "<pgc";
            if (fixed_bitmap_clut) xml << " palette=\"" << xml_escape(bitmap_clut_file.string()) << "\"";
            xml << "><pre>"
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

        // Building DVD-Video UDF 1.02/ISO image with mkisofs -dvd-video.
        reporter.emit(92.0, "Building DVD-Video UDF 1.02/ISO image — 0.0%");
        const auto dvd_input_bytes = output_bytes(dvd_root);
        run_with_output_progress(shq_s(tools_.mkisofs) + " -dvd-video -V " + shq_s(p.volume_label) +
            " -o " + shq(temporary_output_image) + " " + shq(dvd_root), temporary_output_image,
            dvd_input_bytes + dvd_input_bytes / 20U, [&](double fraction) {
                std::ostringstream message;
                message << "Building DVD-Video UDF image — " << std::fixed << std::setprecision(1)
                        << fraction * 100.0 << "%";
                reporter.emit(92.0 + 7.0 * fraction, message.str());
            });
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

    const int VideoProgressStart = p.automatic_video_bitrate ? 18 : 2;
    constexpr int VideoProgressEnd = 82;
    const auto pass_count = [](const EncodingProfile& e) { return e.two_pass ? 2.0 : 1.0; };
    auto work_duration = [](double seconds) { return seconds > 0.0 ? seconds : 1.0; };
    double total_video_work = 0.0;
    // Reserve explicit, duration-weighted mux work for every title/menu.
    // Encoding + muxing remain in one monotonic 18..82% phase.
    constexpr double MuxWorkFactor = 0.12;
    constexpr double ManualAudioWorkFactor = 0.15;
    for (const auto& rm : menus)
        total_video_work += work_duration(rm.menu.duration_seconds) *
            (pass_count(encoding_for_target(rm.menu, p.target)) + MuxWorkFactor +
            (!p.automatic_video_bitrate && !rm.menu.audio_source.empty() ? ManualAudioWorkFactor : 0.0));
    for (std::size_t i = 0; i < p.titles.size(); ++i)
        total_video_work += work_duration(title_durations[i]) *
            (pass_count(encoding_for_target(p.titles[i], p.target)) + MuxWorkFactor +
            (!p.automatic_video_bitrate ? ManualAudioWorkFactor * static_cast<double>(title_tracks[i].audio.size()) : 0.0));
    double completed_video_work = 0.0;

    auto video_percent = [&](double work_done) {
        if (total_video_work <= 0.0) return static_cast<double>(VideoProgressStart);
        const double fraction = std::clamp(work_done / total_video_work, 0.0, 1.0);
        return static_cast<double>(VideoProgressStart) +
               fraction * static_cast<double>(VideoProgressEnd - VideoProgressStart);
    };
    auto progress_for_video = [&](double units, std::string label) -> VideoProgressCallback {
        const double before = completed_video_work;
        return [&, before, units, label = std::move(label)](double fraction, int pass, int passes, double, double) {
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
        const auto menu_background = motion_background ? video_menu.background_video : prepare_menu_background(tools_, video_menu, md, menu_timing);
        const auto menu_overlay = motion_background ? prepare_menu_overlay(tools_, video_menu, md) : fs::path{};
        reporter.emit(video_percent(completed_video_work), label + " — output mode " + menu_timing.description);

        // Prepare menu audio before video so VBR lossless audio can reserve its
        // measured peak transport budget before x264/x265 chooses a VBV ceiling.
        std::string audioMeta;
        int menu_audio_peak_kbps = 0;
        const double manual_audio_units = !p.automatic_video_bitrate && !menu.audio_source.empty() ?
            work_duration(menu.duration_seconds) * ManualAudioWorkFactor : 0.0;
        const double audio_progress_start = completed_video_work;
        auto manual_menu_audio_progress = [&, audio_progress_start](double fraction) {
            std::ostringstream message;
            message << "Audio encoding: menu " << mi + 1U << "/" << menus.size() << " — " << std::fixed
                    << std::setprecision(1) << fraction * 100.0 << "%";
            reporter.emit(video_percent(audio_progress_start + manual_audio_units * fraction), message.str());
        };
        if (!menu.audio_source.empty()) {
            if (p.automatic_video_bitrate && prepared_menu_audio[mi]) {
                const auto& prepared = *prepared_menu_audio[mi];
                audioMeta = audio_meta_for(prepared.path, menu_encoding, "eng");
                menu_audio_peak_kbps = prepared.peak_kbps;
            } else {
            auto ad = md / "audio";
            fs::create_directories(ad);
            const int source_channels = audio_channels(tools_, menu.audio_source, md / "menu-audio-cache-channels", 0);
            const int output_channels = resolved_audio_output_channels(source_channels, menu_encoding, p.target, 0, "menu audio");
            const int fixed_peak = fixed_encoded_audio_peak_kbps(menu_encoding, p.target, output_channels);
            if (!encode_cache_root.empty()) {
                const auto& audio_fp = source_fingerprint(menu.audio_source);
                const auto audio_cache = encoded_cache_paths(
                    encode_cache_root, "menu-audio",
                    menu_audio_cache_key(audio_fp, menu_encoding, p.target, menu.duration_seconds, false, menu.loop_media, tools_),
                    menu_audio_cache_key_remote_location_compat(audio_fp, menu_encoding, p.target, menu.duration_seconds, false, menu.loop_media, tools_),
                    audio_extension(menu_encoding), !p.force_reencode && !p.refresh_encode_cache);
                const auto base_meta = audio_cache_metadata(
                    "menu-audio", menu.audio_source, p.target, menu_encoding, output_channels, 0);
                const int cached_peak = audio_cache.hit.empty() ? 0 : cached_audio_peak_kbps(
                    tools_, audio_cache.hit, fixed_peak, ad / "cached-peak-bitrate.txt", base_meta);
                if (!audio_cache.hit.empty() && cached_peak > 0) {
                    auto hit_meta = base_meta; hit_meta.peak_bitrate_kbps = cached_peak;
                    touch_cache_metadata(audio_cache.hit, hit_meta);
                    reporter.emit(video_percent(completed_video_work), "Using cached encoded audio for menu " + menu.name);
                    audioMeta = audio_meta_for(audio_cache.hit, menu_encoding, "eng");
                    menu_audio_peak_kbps = cached_peak;
                } else {
                    reporter.emit(video_percent(completed_video_work), "Encoding menu audio before video for " + menu.name);
                    const auto cache_tmp_dir = cache_encode_temp_dir(audio_cache.write);
                    CacheTempDirCleanup cache_tmp_cleanup{cache_tmp_dir};
                    const auto encoded = encode_audio_on_pool("Menu audio "+menu.name, menu.audio_source, cache_tmp_dir, menu_encoding, p.target,
                                                      "eng", menu.loop_media, menu.duration_seconds, 0, 0,
                                                      menu.duration_seconds, manual_menu_audio_progress, menu.encoding_worker, menu.encoding_worker_mode);
                    menu_audio_peak_kbps = encoded.peak_bitrate_kbps;
                    const auto menu_audio_metadata = audio_cache_metadata(
                        "menu-audio", menu.audio_source, p.target, menu_encoding, output_channels, 0, menu_audio_peak_kbps);
                    finalize_encoded_cache_file(encoded.path, audio_cache.write, p.force_reencode || p.refresh_encode_cache, menu_audio_metadata);
                    audioMeta = audio_meta_for(audio_cache.write, menu_encoding, "eng");
                }
            } else {
                reporter.emit(video_percent(completed_video_work), "Encoding menu audio before video for " + menu.name);
                const auto encoded = encode_audio_on_pool("Menu audio "+menu.name, menu.audio_source, ad, menu_encoding, p.target, "eng", menu.loop_media, menu.duration_seconds,
                                                  0, 0, menu.duration_seconds, manual_menu_audio_progress, menu.encoding_worker, menu.encoding_worker_mode);
                audioMeta = encoded.meta;
                menu_audio_peak_kbps = encoded.peak_bitrate_kbps;
            }
        }
        }
        if (manual_audio_units > 0.0) {
            completed_video_work += manual_audio_units;
            manual_menu_audio_progress(1.0);
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
        EncodedCachePaths menu_video_cache;
        const fs::path menu_video_cache_root = video_encode_cache_root(menu_encoding);
        if (!menu_video_cache_root.empty()) {
            menu_video_cache = encoded_cache_paths(
                menu_video_cache_root, "menu-video",
                menu_video_cache_key(menu_background, menu_overlay, menu_encoding, p, menu_timing, tools_,
                                     timing_source_interlaced(menu_timing_source), !motion_background,
                                     menu.duration_seconds, menu.background_color, menu.loop_media, menu_video_peak_limit_kbps),
                menu_video_cache_key_remote_location_compat(menu_background, menu_overlay, menu_encoding, p, menu_timing, tools_,
                                                            timing_source_interlaced(menu_timing_source), !motion_background,
                                                            menu.duration_seconds, menu.background_color, menu.loop_media, menu_video_peak_limit_kbps),
                video_extension(menu_encoding), !p.force_reencode && !p.refresh_encode_cache);
        }
        const auto menu_video_metadata = video_cache_metadata(
            "menu-video", motion_background ? menu.background_video : menu.background_image,
            p.target, menu_encoding, menu_timing, tools_, menu_video_peak_limit_kbps);
        if (!menu_video_cache.hit.empty()) {
            touch_cache_metadata(menu_video_cache.hit, menu_video_metadata);
            reporter.emit(video_percent(completed_video_work + units), "Using cached encoded video for menu " + menu.name);
            videoMeta = video_meta_for(menu_video_cache.hit, menu_encoding, menu_timing);
        } else if (!menu_video_cache.write.empty()) {
            const auto cache_tmp_dir = cache_encode_temp_dir(menu_video_cache.write);
            CacheTempDirCleanup cache_tmp_cleanup{cache_tmp_dir};
            const auto cache_tmp_base = cache_tmp_dir / "video";
            (void)encode_on_pool("Menu "+menu.name, menu_background, cache_tmp_base,
                               menu_encoding, menu_timing,
                               timing_source_interlaced(menu_timing_source), !motion_background, menu.duration_seconds,
                               menu.duration_seconds, progress_for_video(units, label),
                               menu.background_color, motion_background && menu.loop_media, menu_overlay, motion_background && !menu.loop_media, menu_video_peak_limit_kbps,
                               menu.encoding_worker, menu.encoding_worker_mode, true);
            const fs::path cache_tmp_output = cache_tmp_base.string() + video_extension(menu_encoding);
            if (fs::exists(fallback_filter_marker(cache_tmp_base))) {
                const auto uncached = preserve_software_filter_fallback(cache_tmp_output, md / "video", video_extension(menu_encoding));
                videoMeta = video_meta_for(uncached, menu_encoding, menu_timing);
            } else {
                finalize_encoded_cache_file(cache_tmp_output, menu_video_cache.write, p.force_reencode || p.refresh_encode_cache, menu_video_metadata);
                videoMeta = video_meta_for(menu_video_cache.write, menu_encoding, menu_timing);
            }
        } else {
            videoMeta = encode_on_pool("Menu "+menu.name, menu_background, md / "video",
                                     menu_encoding, menu_timing,
                                     timing_source_interlaced(menu_timing_source), !motion_background, menu.duration_seconds,
                                     menu.duration_seconds, progress_for_video(units, label),
                                     menu.background_color, motion_background && menu.loop_media, menu_overlay, motion_background && !menu.loop_media, menu_video_peak_limit_kbps,
                               menu.encoding_worker, menu.encoding_worker_mode, true);
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
        const double mux_units = work_duration(menu.duration_seconds) * MuxWorkFactor;
        reporter.emit(video_percent(completed_video_work), "Muxing HDMV menu " + menu.name + " — 0.0%");
        tsmux(tools_, metaPath, bd, [&](double fraction) {
            std::ostringstream message;
            message << "Muxing HDMV menu " << menu.name << " — " << std::fixed
                    << std::setprecision(1) << 100.0 * fraction << "%";
            reporter.emit(video_percent(completed_video_work + mux_units * fraction), message.str());
        });
        completed_video_work += mux_units;
        const unsigned menu_igs_clip = menu_main_clip + 1U;
        move_playlist_clips(bd, disc, static_cast<unsigned>(mi), {menu_main_clip, menu_igs_clip});
        backup_stream_metadata(disc, static_cast<unsigned>(mi), {menu_main_clip, menu_igs_clip});
        if (!p.keep_work_directory) fs::remove_all(md);
    }

    const auto menu_count = static_cast<unsigned>(menus.size());
    struct PendingTitleFinalize {
        std::size_t index=0;
        fs::path directory;
        VideoTimingMode timing;
        int subtitle_width=0;
        int subtitle_height=0;
        double subtitle_fps=0.0;
        std::string audio_meta;
        std::string video_meta;
        std::future<std::string> video_future;
        bool has_future=false;
        std::string label;
    };
    std::vector<PendingTitleFinalize> pending_titles;
    pending_titles.reserve(p.titles.size());
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
            const std::string audio_worker=(stream_settings&&!stream_settings->encoding_worker.empty())?stream_settings->encoding_worker:title.encoding_worker;
            const auto audio_affinity=(stream_settings&&!stream_settings->encoding_worker.empty())?stream_settings->encoding_worker_mode:title.encoding_worker_mode;
            const auto stream_encoding = effective_audio_encoding_for_stream(title, p.target, source_track.source_ordinal);
            validate_encoding_for_target(stream_encoding, p.target, title_label.str() + " audio track " + std::to_string(ai + 1U),
                                         limits_.allow_exceeding_format_limits);
            fs::create_directories(track_dir);

            const double manual_audio_units = p.automatic_video_bitrate ? 0.0 :
                work_duration(title_durations[i]) * ManualAudioWorkFactor;
            const double audio_progress_start = completed_video_work;
            auto manual_audio_progress = [&, audio_progress_start](double fraction) {
                std::ostringstream message;
                message << "Audio encoding: " << title_label.str() << " track " << ai + 1U << "/"
                        << title_tracks[i].audio.size() << " — " << std::fixed << std::setprecision(1)
                        << fraction * 100.0 << "%";
                reporter.emit(video_percent(audio_progress_start + manual_audio_units * fraction), message.str());
            };
            if (p.automatic_video_bitrate && prepared_title_audio[i][ai]) {
                const auto& prepared = *prepared_title_audio[i][ai];
                audioMeta += audio_meta_for(prepared.path, stream_encoding, language);
                aggregate_audio_peak_kbps += prepared.peak_kbps;
                continue;
            }
            if (audio_passthrough[ai]) {
                reporter.emit(video_percent(completed_video_work), "Remuxing compliant audio track " + std::to_string(ai + 1U) + " before video for " + title_label.str());
                audioMeta += remux_compliant_audio(tools_, title.source, track_dir,
                                                   analysis->audio[ai].decision, language,
                                                   source_track.source_ordinal, source_track.input_stream_index);
                const fs::path remuxed = track_dir / (std::string("audio") + analysis->audio[ai].decision.elementary_extension);
                const int peak = probe_elementary_audio_peak_kbps(tools_, remuxed, track_dir / "peak-bitrate.txt");
                aggregate_audio_peak_kbps += peak;
                reporter.emit(video_percent(completed_video_work), "Measured audio track " + std::to_string(ai + 1U) +
                              " peak: " + std::to_string(peak) + " kb/s");
                completed_video_work += manual_audio_units;
                manual_audio_progress(1.0);
                continue;
            }

            EncodedCachePaths audio_cache;
            if (title_fp && !encode_cache_root.empty()) {
                const int requested_output_channels = stream_settings ? stream_settings->output_channels : 0;
                audio_cache = encoded_cache_paths(
                    encode_cache_root, "audio",
                    audio_cache_key(*title_fp, stream_encoding, source_track.source_ordinal, requested_output_channels, tools_),
                    audio_cache_key_remote_location_compat(*title_fp, stream_encoding, source_track.source_ordinal, requested_output_channels, tools_),
                    audio_extension(stream_encoding), !force_reencode && !p.refresh_encode_cache);
            }
            const int source_channels_for_metadata = analysis && ai < analysis->audio.size() && analysis->audio[ai].info.channels > 0
                ? analysis->audio[ai].info.channels : audio_channels(tools_, title.source, track_dir, source_track.source_ordinal,
                                                                          source_track.input_stream_index);
            const int output_channels_for_metadata = resolved_audio_output_channels(
                source_channels_for_metadata, stream_encoding, p.target,
                stream_settings ? stream_settings->output_channels : 0,
                "title audio track " + std::to_string(ai + 1U));
            const int fixed_peak = fixed_encoded_audio_peak_kbps(stream_encoding, p.target, output_channels_for_metadata);
            const auto base_audio_metadata = audio_cache_metadata(
                "title-audio", title.source, p.target, stream_encoding, output_channels_for_metadata,
                source_track.source_ordinal);
            const int cached_peak = audio_cache.hit.empty() ? 0 : cached_audio_peak_kbps(
                tools_, audio_cache.hit, fixed_peak, track_dir / "cached-peak-bitrate.txt", base_audio_metadata);
            if (!audio_cache.hit.empty() && cached_peak > 0) {
                auto title_audio_metadata = base_audio_metadata;
                title_audio_metadata.peak_bitrate_kbps = cached_peak;
                touch_cache_metadata(audio_cache.hit, title_audio_metadata);
                reporter.emit(video_percent(completed_video_work), "Using cached encoded audio track " + std::to_string(ai + 1U) + " before video for " + title_label.str());
                audioMeta += audio_meta_for(audio_cache.hit, stream_encoding, language);
                aggregate_audio_peak_kbps += cached_peak;
            } else {
                reporter.emit(video_percent(completed_video_work), "Encoding audio track " + std::to_string(ai + 1U) + " before video for " + title_label.str());
                EncodedAudioResult encoded;
                if (!audio_cache.write.empty()) {
                    const auto cache_tmp_dir = cache_encode_temp_dir(audio_cache.write);
                    CacheTempDirCleanup cache_tmp_cleanup{cache_tmp_dir};
                    encoded = encode_audio_on_pool("Title audio "+std::to_string(i+1U)+" track "+std::to_string(ai+1U), title.source, cache_tmp_dir, stream_encoding, p.target, language, false, 0,
                                           source_track.source_ordinal, stream_settings ? stream_settings->output_channels : 0,
                                           title_durations[i], manual_audio_progress, audio_worker, audio_affinity, source_track.input_stream_index);
                    const auto title_audio_metadata = audio_cache_metadata(
                        "title-audio", title.source, p.target, stream_encoding, output_channels_for_metadata,
                        source_track.source_ordinal, encoded.peak_bitrate_kbps);
                    finalize_encoded_cache_file(encoded.path, audio_cache.write, force_reencode || p.refresh_encode_cache, title_audio_metadata);
                    audioMeta += audio_meta_for(audio_cache.write, stream_encoding, language);
                } else {
                    encoded = encode_audio_on_pool("Title audio "+std::to_string(i+1U)+" track "+std::to_string(ai+1U), title.source, track_dir, stream_encoding, p.target, language, false, 0,
                                           source_track.source_ordinal, stream_settings ? stream_settings->output_channels : 0,
                                           title_durations[i], manual_audio_progress, audio_worker, audio_affinity, source_track.input_stream_index);
                    audioMeta += encoded.meta;
                }
                aggregate_audio_peak_kbps += encoded.peak_bitrate_kbps;
                reporter.emit(video_percent(completed_video_work), "Measured encoded audio track " + std::to_string(ai + 1U) +
                              " peak: " + std::to_string(encoded.peak_bitrate_kbps) + " kb/s");
            }
            completed_video_work += manual_audio_units;
            manual_audio_progress(1.0);
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

        PendingTitleFinalize pending;
        pending.index=i;
        pending.directory=td;
        pending.timing=title_timing;
        pending.subtitle_width=title_geometry.graphics_width;
        pending.subtitle_height=title_geometry.graphics_height;
        pending.subtitle_fps=video_passthrough ? analysis->video.frame_rate : title_timing.frame_rate_value;
        pending.audio_meta=audioMeta;
        pending.label=title_label.str();
        if (video_passthrough) {
            reporter.emit(video_percent(completed_video_work), "Remuxing compliant video for " + title_label.str());
            pending.video_meta = remux_compliant_video(tools_, title.source, td / "passthrough-video", analysis->video, analysis->video_decision);
            reporter.emit(video_percent(completed_video_work + units), "Remuxed compliant video for " + title_label.str());
        } else {
            EncodedCachePaths video_cache;
            const fs::path title_video_cache_root = video_encode_cache_root(title_encoding);
            if (title_fp && !title_video_cache_root.empty()) {
                video_cache = encoded_cache_paths(
                    title_video_cache_root, "video",
                    video_cache_key(*title_fp, title_encoding, p, title_timing, tools_, title_video_peak_limit_kbps),
                    video_cache_key_remote_location_compat(*title_fp, title_encoding, p, title_timing, tools_, title_video_peak_limit_kbps),
                    video_extension(title_encoding), !force_reencode && !p.refresh_encode_cache);
            }
            const auto title_video_metadata = video_cache_metadata("title-video", title.source, p.target, title_encoding, title_timing, tools_, title_video_peak_limit_kbps);
            if (!video_cache.hit.empty()) {
                touch_cache_metadata(video_cache.hit, title_video_metadata);
                reporter.emit(video_percent(completed_video_work + units), "Using cached encoded video for " + title_label.str());
                pending.video_meta = video_meta_for(video_cache.hit, title_encoding, title_timing);
            } else {
                const std::string job_label=title_label.str();
                const auto source=title.source;
                const auto timing=title_timing;
                const auto encoding=title_encoding;
                const bool source_interlaced_copy=timing_source_interlaced(title_timing_source);
                const double duration=title_durations[i];
                const int peak=title_video_peak_limit_kbps;
                const bool replace_cache=force_reencode || p.refresh_encode_cache;
                const std::string requested_worker=title.encoding_worker;
                const auto affinity_mode=title.encoding_worker_mode;
                const auto video_cache_write=video_cache.write;
                reporter.emit(video_percent(completed_video_work), "Queued " + job_label + " for parallel encoding");
                pending.has_future=true;
                pending.video_future=std::async(std::launch::async,[&,job_label,source,timing,encoding,source_interlaced_copy,duration,peak,video_cache_write,title_video_metadata,replace_cache,td,requested_worker,affinity_mode]() mutable -> std::string {
                    if (!video_cache_write.empty()) {
                        const auto cache_tmp_dir=cache_encode_temp_dir(video_cache_write);
                        CacheTempDirCleanup cache_tmp_cleanup{cache_tmp_dir};
                        const auto cache_tmp_base=cache_tmp_dir/"video";
                        (void)encode_on_pool(job_label,source,cache_tmp_base,encoding,timing,source_interlaced_copy,false,0,duration,{},
                                             {0,0,0,255},false,{},false,peak,requested_worker,affinity_mode);
                        const fs::path cache_tmp_output=cache_tmp_base.string()+video_extension(encoding);
                        if (fs::exists(fallback_filter_marker(cache_tmp_base))) {
                            const auto uncached = preserve_software_filter_fallback(cache_tmp_output, td / "video", video_extension(encoding));
                            return video_meta_for(uncached, encoding, timing);
                        }
                        finalize_encoded_cache_file(cache_tmp_output,video_cache_write,replace_cache,title_video_metadata);
                        return video_meta_for(video_cache_write,encoding,timing);
                    }
                    return encode_on_pool(job_label,source,td/"video",encoding,timing,source_interlaced_copy,false,0,duration,{},
                                          {0,0,0,255},false,{},false,peak,requested_worker,affinity_mode);
                });
            }
        }
        completed_video_work += units;
        pending_titles.push_back(std::move(pending));
    }

    // Finalize titles in project order after all video jobs have been queued.
    // This lets independent video encodes occupy all local/remote worker slots
    // concurrently while keeping playlist/clip numbering deterministic.
    for(auto& pending:pending_titles){
        const std::size_t i=pending.index;
        const auto& title=p.titles[i];
        const auto& td=pending.directory;
        std::string videoMeta=pending.has_future?pending.video_future.get():pending.video_meta;
        std::string subtitleMeta;
        for (std::size_t si = 0; si < title_tracks[i].subtitles.size(); ++si) {
            reporter.emit(video_percent(completed_video_work), "Preparing subtitle track " + std::to_string(si + 1U) + " for " + pending.label);
            auto subtitle_track = title_tracks[i].subtitles[si];
            subtitle_track.language = effective_subtitle_language(title, subtitle_track);
            subtitleMeta += prepare_subtitle(tools_, title.source,
                                             td / ("subtitle-" + std::to_string(si + 1U)),
                                             subtitle_track,
                                             pending.subtitle_width, pending.subtitle_height, pending.subtitle_fps);
        }
        const auto playlist = menu_count + static_cast<unsigned>(i);
        const auto title_clip = 2U * menu_count + static_cast<unsigned>(i);
        std::ostringstream meta;
        meta << meta_header(playlist, resolved_title_chapters[i], p.target, static_cast<unsigned>(combined_transport_limit_kbps), 0U, title_clip) << videoMeta;
        meta << pending.audio_meta << subtitleMeta;
        auto mf = td / "title.meta";
        write_text(mf, meta.str());
        auto bd = td / "bd";
        const double mux_units = work_duration(title_durations[i]) * MuxWorkFactor;
        reporter.emit(video_percent(completed_video_work), "Muxing " + pending.label + " — 0.0%");
        tsmux(tools_, mf, bd, [&](double fraction) {
            std::ostringstream message;
            message << "Muxing " << pending.label << " — " << std::fixed
                    << std::setprecision(1) << 100.0 * fraction << "%";
            reporter.emit(video_percent(completed_video_work + mux_units * fraction), message.str());
        });
        completed_video_work += mux_units;
        if (target_is_avchd(p.target)) {
            // In AVCHD mode tsMuxer writes the version-0100 index, MovieObject,
            // playlist and clip information. Preserve that complete native tree
            // instead of replacing it with BDMV Author's Blu-ray 0200/0300 navigation.
            std::error_code ec;
            fs::remove_all(disc, ec); ec.clear();
            fs::rename(bd, disc, ec);
            if (ec) throw std::runtime_error("cannot move native AVCHD structure into the canonical work tree: " + ec.message());
        } else {
            move_playlist_clips(bd, disc, playlist, {title_clip});
            {std::ostringstream playlist_name;playlist_name<<std::setw(5)<<std::setfill('0')<<playlist<<".mpls";hdmv::add_playlist_user_operation_mask(disc/"BDMV/PLAYLIST"/playlist_name.str(),bluray_uop_mask(title.prohibited_user_operations));}
            backup_stream_metadata(disc, playlist, {title_clip});
        }
        if (!p.keep_work_directory) fs::remove_all(td);
    }

    if (target_is_avchd(p.target)) {
        reporter.emit(VideoProgressEnd, "Validating native AVCHD 1.0 navigation");
        auto require_v0100 = [&](const fs::path& path, const char* magic) {
            std::ifstream f(path, std::ios::binary);
            std::array<char,8> header{};
            if (!f.read(header.data(), static_cast<std::streamsize>(header.size())) ||
                std::string(header.data(),4) != magic || std::string(header.data()+4,4) != "0100")
                throw std::runtime_error("tsMuxer did not produce an AVCHD 1.0 control file: " + path.string());
        };
        require_v0100(disc / "BDMV/index.bdmv", "INDX");
        require_v0100(disc / "BDMV/MovieObject.bdmv", "MOBJ");
        require_v0100(disc / "BDMV/PLAYLIST/00000.mpls", "MPLS");
    } else {
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
    }

    reporter.emit(88, "Building UDF 2.50 image — 0.0%");
    udf25::WriterOptions o;
    o.output = temporary_output_image;
    o.volume_label = p.volume_label;
    o.application_id = "*BDMVAuthor";
    o.implementation_id = "*BDMVAuthor";
    o.automatic_bdmv_correction = true;
    o.automatic_hddvd_correction = false;
    o.verify_after_write = true;
    o.cancel_requested = [] { return authoring_cancel_requested(); };
    o.progress = [&](double fraction) {
        std::ostringstream message;
        message << "Building UDF 2.50 image — " << std::fixed << std::setprecision(1)
                << fraction * 100.0 << "%";
        reporter.emit(88.0 + 11.0 * fraction, message.str());
    };
    if (target_is_avchd(p.target)) {
        // tsMuxer owns the AVCHD application tree.  Graft its output root as a
        // whole rather than reconstructing a Blu-ray-shaped subset here; this
        // preserves every top-level entry tsMuxer decides belongs to AVCHD while
        // the bundled UDF writer remains solely responsible for UDF 2.50.
        o.grafts.push_back({disc, ""});
    } else {
        o.grafts.push_back({disc / "BDMV", "BDMV"});
        o.grafts.push_back({disc / "CERTIFICATE", "CERTIFICATE"});
    }
    udf25::ImageWriter w(std::move(o));
    try {
        w.build();
    } catch (...) {
        if (authoring_cancel_requested()) throw AuthorCancelled();
        throw;
    }
    if (!disc_capacity_is_unlimited(p.disc_capacity_bytes)) {
        const auto image_bytes = fs::file_size(temporary_output_image);
        if (image_bytes > p.disc_capacity_bytes) {
            const std::string prefix = target_is_avchd(p.target)
                ? "completed AVCHD image exceeds the selected size target despite preflight budgeting; image is "
                : "completed Blu-ray image exceeds the selected size target despite preflight budgeting; image is ";
            throw std::runtime_error(prefix + std::to_string(image_bytes) +
                                     " bytes but target is " + std::to_string(p.disc_capacity_bytes) + " bytes");
        }
    }
    authoring_cancellation_point();
    publish_rendered_image(temporary_output_image, p.output_image);
    reporter.emit(100, target_is_avchd(p.target) ? "AVCHD image complete" : "Blu-ray image complete");
    if (!p.keep_work_directory) fs::remove_all(work);
}

} // namespace bdmvauthor
