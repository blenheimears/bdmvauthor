// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cmath>
#include <filesystem>
#include <initializer_list>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sstream>
#include <span>
#include <type_traits>
#include <vector>
#include <cctype>

namespace bdmvauthor {

template <typename Integer>
inline bool parse_integer_exact(std::string_view text, Integer& value, int base = 10) {
    static_assert(std::is_integral_v<Integer>);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    if (!text.empty() && text.front() == '+') text.remove_prefix(1);
    if (text.empty()) return false;
    Integer parsed{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed, base);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) return false;
    value = parsed;
    return true;
}

inline constexpr int kProjectDesignWidth = 3840;
inline constexpr int kProjectDesignHeight = 2160;
inline constexpr int kBdGraphicsWidth = 1920;
inline constexpr int kBdGraphicsHeight = 1080;

enum class DiscTarget { BluRay1080, UltraHdBluRay2160, DvdVideo480p };

inline constexpr std::uint64_t default_disc_capacity_bytes(DiscTarget target) {
    switch (target) {
        case DiscTarget::DvdVideo480p: return 4700000000ULL;
        case DiscTarget::BluRay1080: return 25000000000ULL;
        case DiscTarget::UltraHdBluRay2160: return 50000000000ULL;
    }
    return 25000000000ULL;
}

inline constexpr bool disc_capacity_is_unlimited(std::uint64_t bytes) { return bytes == 0ULL; }

enum class AudioCodec { Ac3, Lpcm, Dca, TrueHdAc3 };
enum class VideoCodec { X264, Mpeg2, Hevc };

struct TargetGeometry {
    int video_width;
    int video_height;
    int graphics_width;
    int graphics_height;
};

inline constexpr TargetGeometry target_geometry(DiscTarget target) {
    switch (target) {
        case DiscTarget::UltraHdBluRay2160: return {3840,2160,1920,1080};
        case DiscTarget::DvdVideo480p: return {720,480,720,480};
        case DiscTarget::BluRay1080: return {1920,1080,1920,1080};
    }
    return {1920,1080,1920,1080};
}
inline constexpr bool target_is_currently_authorable(DiscTarget) {
    return true;
}
inline constexpr bool target_is_uhd(DiscTarget target) {
    return target == DiscTarget::UltraHdBluRay2160;
}
inline constexpr bool target_is_dvd(DiscTarget target) {
    return target == DiscTarget::DvdVideo480p;
}

inline constexpr bool audio_codec_has_configurable_bitrate(AudioCodec codec) {
    return codec != AudioCodec::Lpcm;
}

inline int maximum_keyframe_interval_for_rate(DiscTarget target, VideoCodec codec, double frame_rate, bool dvd_pal = false) {
    if (target == DiscTarget::DvdVideo480p) return dvd_pal ? 15 : 18;
    if (!(frame_rate > 0.0) || !std::isfinite(frame_rate)) throw std::runtime_error("keyframe maximum requires a positive finite frame rate");
    if (codec == VideoCodec::Mpeg2) return std::max(1, static_cast<int>(std::ceil(frame_rate)));
    return std::max(1, static_cast<int>(std::floor(frame_rate * 1.05 + 1e-6)));
}

struct EncodingProfile {
    VideoCodec video_codec = VideoCodec::X264;
    int video_bitrate_kbps = 24000;
    // Structured encoder rate-control bounds. 0 kb/s is the lowest supported
    // minrate for every currently supported disc-video codec. The default
    // maxrate for this base profile is the standard Blu-ray video ceiling;
    // target-specific default profiles replace it where appropriate.
    int video_min_bitrate_kbps = 0;
    int video_max_bitrate_kbps = 40000;
    std::string x264_preset = "medium";
    std::string x265_preset = "medium";
    bool two_pass = false;
    // 0 keeps BDMV Author's codec/timing-specific historical default. A
    // positive value requests an exact GOP/keyframe interval in encoded frames,
    // subject to the selected disc standard's maximum GOP length.
    int keyframe_interval_frames = 0;
    AudioCodec audio_codec = AudioCodec::Ac3;
    int ac3_bitrate_kbps = 640;
    int dca_bitrate_kbps = 1509;
    // LPCM is uncompressed; its bitrate is derived from sample rate, bit depth,
    // and channel count rather than entered directly.
    int lpcm_sample_rate_hz = 0; // 0 = match source / round up to next legal lossless rate
    int lpcm_bit_depth = 0; // 0 = match source / round up to next encoder-supported lossless depth
    // Advanced codec-private options, one option per line as name=value.
    // Spec-critical parameters (codec/profile/level/rate/VBV/GOP/timing,
    // pixel format, channel layout/sample rate, etc.) are deliberately kept
    // in the structured fields above and cannot be overridden here.
    std::string x264_advanced_options;
    std::string x265_advanced_options;
    std::string mpeg2_advanced_options;
    std::string ac3_advanced_options;
    std::string dca_advanced_options;
    std::string lpcm_advanced_options;
    std::string truehd_advanced_options;
};

struct AdvancedCodecOption {
    std::string name;
    std::string value;
    bool has_value = false;
};

inline std::string trim_codec_option_text(std::string text) {
    auto not_space=[](unsigned char c){return !std::isspace(c);};
    auto b=std::find_if(text.begin(),text.end(),not_space);
    auto e=std::find_if(text.rbegin(),text.rend(),not_space).base();
    return b<e?std::string(b,e):std::string();
}

inline std::vector<AdvancedCodecOption> parse_advanced_codec_options(const std::string& text) {
    std::vector<AdvancedCodecOption> out;
    std::istringstream in(text);
    std::string line;
    std::size_t line_no=0;
    while(std::getline(in,line)){
        ++line_no; line=trim_codec_option_text(line);
        if(line.empty()||line[0]=='#')continue;
        const auto eq=line.find('=');
        std::string name=trim_codec_option_text(line.substr(0,eq));
        while(!name.empty()&&name.front()=='-')name.erase(name.begin());
        if(name.empty())throw std::runtime_error("advanced codec option line "+std::to_string(line_no)+" has no option name");
        for(char raw:name){const auto c=static_cast<unsigned char>(raw);if(!(std::isalnum(c)||c=='-'||c=='_'||c=='.'||c==':'))
            throw std::runtime_error("advanced codec option '"+name+"' contains an invalid character");}
        AdvancedCodecOption o;o.name=std::move(name);
        if(eq!=std::string::npos){o.has_value=true;o.value=trim_codec_option_text(line.substr(eq+1));}
        out.push_back(std::move(o));
    }
    return out;
}

inline std::string normalized_codec_option_name(std::string name) {
    while(!name.empty()&&name.front()=='-')name.erase(name.begin());
    std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
    std::replace(name.begin(),name.end(),'_','-');
    return name;
}

inline const std::string& advanced_video_options_for_codec(const EncodingProfile& e) {
    switch(e.video_codec){
        case VideoCodec::X264:return e.x264_advanced_options;
        case VideoCodec::Hevc:return e.x265_advanced_options;
        case VideoCodec::Mpeg2:return e.mpeg2_advanced_options;
    }
    return e.x264_advanced_options;
}
inline std::string& advanced_video_options_for_codec(EncodingProfile& e) {
    return const_cast<std::string&>(advanced_video_options_for_codec(static_cast<const EncodingProfile&>(e)));
}
inline const std::string& advanced_audio_options_for_codec(const EncodingProfile& e) {
    switch(e.audio_codec){
        case AudioCodec::Ac3:return e.ac3_advanced_options;
        case AudioCodec::Dca:return e.dca_advanced_options;
        case AudioCodec::Lpcm:return e.lpcm_advanced_options;
        case AudioCodec::TrueHdAc3:return e.truehd_advanced_options;
    }
    return e.ac3_advanced_options;
}
inline std::string& advanced_audio_options_for_codec(EncodingProfile& e) {
    return const_cast<std::string&>(advanced_audio_options_for_codec(static_cast<const EncodingProfile&>(e)));
}
inline void append_advanced_codec_option(std::string& text,const std::string& option) {
    const auto parsed=parse_advanced_codec_options(option);
    if(parsed.size()!=1U)throw std::runtime_error("advanced codec option must contain exactly one name[=value] entry");
    if(!text.empty()&&text.back()!='\n')text.push_back('\n');
    text+=parsed.front().name;
    if(parsed.front().has_value){text.push_back('=');text+=parsed.front().value;}
}

inline bool codec_option_name_in(std::string_view name,std::initializer_list<std::string_view> values){
    return std::find(values.begin(),values.end(),name)!=values.end();
}

inline void validate_advanced_video_options(const EncodingProfile& e, DiscTarget) {
    const auto options=parse_advanced_codec_options(advanced_video_options_for_codec(e));
    for(const auto& o:options){
        const auto n=normalized_codec_option_name(o.name);
        if(codec_option_name_in(n,{"c:v","codec","vcodec","codec:v","b:v","bitrate","vbv-maxrate","vbv-bufsize","maxrate","minrate","bufsize","g","keyint","min-keyint","scenecut","open-gop","profile","profile:v","level","level:v","pix-fmt","pixel-format","output-depth","input-depth","bit-depth","chroma-format","fps","force-cfr","tff","bff","interlaced","fake-interlaced","sar","aspect","s","vf","filter:v","colorprim","color-primaries","transfer","color-trc","colormatrix","colorspace","range","color-range","bluray-compat","uhd-bd","nal-hrd","hrd","aud","repeat-headers","output","o","pass","stats","demuxer","input","i","map","an","sn","dn","y","hide-banner","loglevel","progress","f"}))
            throw std::runtime_error("advanced video option '"+o.name+"' controls a disc-spec parameter; use BDMV Author's normal format/codec controls instead");
        if(e.video_codec==VideoCodec::X264&&(n=="bframes"||n=="ref")){
            if(!o.has_value)throw std::runtime_error("advanced x264 option '"+o.name+"' requires an integer value");
            int value=0;if(!parse_integer_exact(o.value,value))throw std::runtime_error("advanced x264 option '"+o.name+"' requires an integer value");
            if(n=="bframes"&&(value<0||value>3))throw std::runtime_error("Blu-ray x264 bframes must be 0..3");
            if(n=="ref"&&(value<1||value>4))throw std::runtime_error("Blu-ray x264 ref must be 1..4");
        }
        if(e.video_codec==VideoCodec::Hevc&&o.value.find(':')!=std::string::npos)
            throw std::runtime_error("advanced x265 option values may not contain ':' because libx265 uses ':' as its private-option separator");
    }
}

inline void validate_advanced_audio_options(const EncodingProfile& e, DiscTarget) {
    const auto options=parse_advanced_codec_options(advanced_audio_options_for_codec(e));
    for(const auto& o:options){
        const auto n=normalized_codec_option_name(o.name);
        if(codec_option_name_in(n,{"c:a","codec","acodec","codec:a","b:a","bitrate","ar","sample-rate","ac","channels","channel-layout","sample-fmt","sample-format","f","format","strict","i","map","y","hide-banner","loglevel","progress","output","o"}))
            throw std::runtime_error("advanced audio option '"+o.name+"' controls a disc-spec parameter; use BDMV Author's normal codec/bitrate controls instead");
    }
}

inline void validate_advanced_codec_options(const EncodingProfile& e, DiscTarget target) {
    validate_advanced_video_options(e,target);
    validate_advanced_audio_options(e,target);
}

inline EncodingProfile default_uhd_encoding_profile() {
    EncodingProfile e;
    e.video_codec = VideoCodec::Hevc;
    e.video_bitrate_kbps = 60000;
    e.video_min_bitrate_kbps = 0;
    e.video_max_bitrate_kbps = 86000;
    e.x265_preset = "medium";
    return e;
}

inline EncodingProfile default_dvd_encoding_profile() {
    EncodingProfile e;
    e.video_codec = VideoCodec::Mpeg2;
    e.video_bitrate_kbps = 8000;
    e.video_min_bitrate_kbps = 0;
    e.video_max_bitrate_kbps = 9800;
    e.audio_codec = AudioCodec::Ac3;
    e.ac3_bitrate_kbps = 448;
    e.lpcm_sample_rate_hz = 48000;
    e.lpcm_bit_depth = 16;
    return e;
}

inline constexpr bool video_codec_allowed_for_target(DiscTarget target, VideoCodec codec) {
    switch (target) {
        case DiscTarget::BluRay1080: return codec == VideoCodec::X264 || codec == VideoCodec::Mpeg2;
        case DiscTarget::UltraHdBluRay2160: return codec == VideoCodec::Hevc || codec == VideoCodec::X264;
        case DiscTarget::DvdVideo480p: return codec == VideoCodec::Mpeg2;
    }
    return false;
}
inline constexpr bool audio_codec_allowed_for_target(DiscTarget target, AudioCodec codec) {
    if (target == DiscTarget::DvdVideo480p) return codec == AudioCodec::Ac3 || codec == AudioCodec::Lpcm;
    return true;
}
inline constexpr int video_codec_min_bitrate_kbps(DiscTarget, VideoCodec) {
    // None of the supported optical-disc standards requires a positive encoder
    // minrate. Zero therefore represents the lowest legal/default minrate.
    return 0;
}
inline constexpr int video_codec_max_bitrate_kbps(DiscTarget target, VideoCodec codec) {
    if (target == DiscTarget::UltraHdBluRay2160 && codec == VideoCodec::X264) return 40000;
    switch (target) {
        case DiscTarget::UltraHdBluRay2160: return 86000;
        case DiscTarget::DvdVideo480p: return 9800;
        case DiscTarget::BluRay1080: return 40000;
    }
    return 40000;
}
inline constexpr int target_max_video_bitrate_kbps(DiscTarget target) {
    switch (target) {
        case DiscTarget::UltraHdBluRay2160: return 86000;
        case DiscTarget::DvdVideo480p: return 9800;
        case DiscTarget::BluRay1080: return 40000;
    }
    return 40000;
}
inline constexpr int target_max_ac3_bitrate_kbps(DiscTarget target) {
    return target == DiscTarget::DvdVideo480p ? 448 : 640;
}
inline constexpr int target_max_combined_av_bitrate_kbps(DiscTarget target) {
    switch (target) {
        case DiscTarget::BluRay1080: return 48000;
        case DiscTarget::UltraHdBluRay2160: return 70000;
        case DiscTarget::DvdVideo480p: return 10080;
    }
    return 48000;
}
inline constexpr bool lpcm_sample_rate_allowed_for_target(DiscTarget target, int sample_rate_hz) {
    if (target == DiscTarget::DvdVideo480p) return sample_rate_hz == 48000 || sample_rate_hz == 96000;
    return sample_rate_hz == 48000 || sample_rate_hz == 96000 || sample_rate_hz == 192000;
}
inline constexpr bool lpcm_bit_depth_supported_by_encoder(int bit_depth) {
    // FFmpeg exposes native 16- and 24-bit PCM encoders. 20-bit LPCM is legal
    // in the disc formats but has no native FFmpeg PCM encoder, so do not offer
    // a misleading 20-bit setting that would actually author 24-bit samples.
    return bit_depth == 16 || bit_depth == 24;
}
inline constexpr int lpcm_max_channels_for_target(DiscTarget target, int sample_rate_hz) {
    if (target == DiscTarget::DvdVideo480p) return 2; // current mplex backend supports stereo LPCM safely
    return sample_rate_hz == 192000 ? 6 : 8;
}

inline constexpr bool lossless_audio_codec(AudioCodec codec) {
    return codec == AudioCodec::Lpcm || codec == AudioCodec::TrueHdAc3;
}
inline constexpr bool lossless_sample_rate_allowed_for_target(DiscTarget target, int sample_rate_hz) {
    if (sample_rate_hz == 0) return true;
    return lpcm_sample_rate_allowed_for_target(target, sample_rate_hz);
}
inline constexpr bool lossless_bit_depth_supported_by_encoder(int bit_depth) {
    return bit_depth == 0 || lpcm_bit_depth_supported_by_encoder(bit_depth);
}
inline int resolved_lossless_sample_rate_hz(DiscTarget target, int configured_hz, int source_hz) {
    if (configured_hz > 0) return configured_hz;
    if (target == DiscTarget::DvdVideo480p) return 48000;
    const int source = source_hz > 0 ? source_hz : 48000;
    for (int rate : {48000, 96000, 192000}) if (source <= rate) return rate;
    throw std::runtime_error("source audio sample rate exceeds the maximum supported Blu-ray lossless rate of 192 kHz");
}
inline int resolved_lossless_bit_depth(DiscTarget target, int configured_bits, int source_bits) {
    if (configured_bits > 0) return configured_bits;
    if (target == DiscTarget::DvdVideo480p) return 16;
    const int source = source_bits > 0 ? source_bits : 16;
    if (source <= 16) return 16;
    if (source <= 24) return 24;
    throw std::runtime_error("source audio bit depth exceeds the maximum supported lossless output depth of 24 bits");
}

inline constexpr int lpcm_bitrate_kbps(int sample_rate_hz, int bit_depth, int channels) {
    return (sample_rate_hz * bit_depth * channels) / 1000;
}

inline int audio_bitrate_kbps(const EncodingProfile& profile) {
    return profile.audio_codec == AudioCodec::Dca ? profile.dca_bitrate_kbps : profile.ac3_bitrate_kbps;
}

inline void set_audio_bitrate_kbps(EncodingProfile& profile, int bitrate_kbps) {
    if (profile.audio_codec == AudioCodec::Dca) profile.dca_bitrate_kbps = bitrate_kbps;
    else if (profile.audio_codec != AudioCodec::Lpcm) profile.ac3_bitrate_kbps = bitrate_kbps;
}

inline void copy_audio_encoding_settings(EncodingProfile& destination, const EncodingProfile& source) {
    destination.audio_codec = source.audio_codec;
    destination.ac3_bitrate_kbps = source.ac3_bitrate_kbps;
    destination.dca_bitrate_kbps = source.dca_bitrate_kbps;
    destination.lpcm_sample_rate_hz = source.lpcm_sample_rate_hz;
    destination.lpcm_bit_depth = source.lpcm_bit_depth;
    destination.ac3_advanced_options = source.ac3_advanced_options;
    destination.dca_advanced_options = source.dca_advanced_options;
    destination.lpcm_advanced_options = source.lpcm_advanced_options;
    destination.truehd_advanced_options = source.truehd_advanced_options;
}

struct Rect { int x=0, y=0, width=0, height=0; };

struct Rgba {
    std::uint8_t r=0, g=0, b=0, a=255;
    friend constexpr bool operator==(const Rgba&, const Rgba&) = default;
};

struct ButtonStateStyle {
    Rgba text_color{255,255,255,255};
    Rgba background_color{20,20,20,190};
    Rgba border_color{255,255,255,255};
    int border_width = 6;
};

enum class MenuButtonKind { Text, Image };
enum class MenuButtonTargetKind { Title, Menu, AudioTrack, SubtitleTrack, SubtitleOff };
enum class NavigationActionKind { PlayTitle, Menu, AudioTrack, SubtitleTrack, SubtitleOff, RepeatBegin, RepeatEnd };

// User operations which can be prohibited while a title is playing. The model
// stores semantic operations rather than raw Blu-ray/DVD bit positions because
// the two formats use different UOP tables. A zero mask is deliberately the
// default: BDMV Author adds no user-operation prohibitions unless requested.
enum class UserOperation : std::uint8_t {
    MenuCall, TitleSearch, ChapterSearch, TimeSearch, SkipNext, SkipPrevious, Stop, Pause, StillOff,
    ForwardPlay, BackwardPlay, Resume, MoveUp, MoveDown, MoveLeft, MoveRight, SelectButton,
    ActivateButton, SelectAndActivate, PrimaryAudioChange, AngleChange, PopupOn, PopupOff,
    SubtitleEnableDisable, SubtitleChange, SecondaryVideoEnableDisable, SecondaryVideoChange,
    SecondaryAudioEnableDisable, SecondaryAudioChange, PipSubtitleChange, GoUp, TitleMenuCall,
    SubtitleMenuCall, AudioMenuCall, AngleMenuCall, ChapterMenuCall, KaraokeAudioMixChange,
    VideoPresentationModeChange, TitleOrTimePlay, ChapterSearchOrPlay
};

inline constexpr std::uint64_t user_operation_bit(UserOperation op) {
    return std::uint64_t{1} << static_cast<unsigned>(op);
}
inline constexpr bool user_operation_prohibited(std::uint64_t mask, UserOperation op) {
    return (mask & user_operation_bit(op)) != 0;
}

enum class ChapterMode { SourceOrFiveMinute, Manual, Interval, None };
enum class MenuOverlayKind { Text, Image };

struct NavigationAction {
    NavigationActionKind kind = NavigationActionKind::PlayTitle;
    std::uint16_t target_title = 1;
    // 0 means normal title start. 1 is also the title start; values >1 target
    // an authored chapter number.
    std::uint16_t target_chapter = 0;
    std::uint16_t target_stream = 1;
    std::string target_menu_id;
    // Number of times to execute this action. For RepeatBegin this is the
    // number of times to execute the enclosed action group. 1 is the
    // historical behavior; 0 means repeat forever where validation permits it.
    std::uint16_t repeat_count = 1;
};

struct ExpandedNavigationSequence {
    std::vector<NavigationAction> actions;
    // When set, the flattened sequence loops forever back to this action index
    // after reaching the end. Finite repeat groups are expanded in-place.
    std::size_t infinite_loop_start = static_cast<std::size_t>(-1);
    bool has_infinite_loop() const { return infinite_loop_start != static_cast<std::size_t>(-1); }
};

inline constexpr bool navigation_action_is_repeat_marker(NavigationActionKind kind) {
    return kind == NavigationActionKind::RepeatBegin || kind == NavigationActionKind::RepeatEnd;
}

inline ExpandedNavigationSequence expand_navigation_repeat_groups(const std::vector<NavigationAction>& input,
                                                                   std::size_t max_actions = 9000U) {
    ExpandedNavigationSequence result;
    result.actions.reserve(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        const auto& action = input[i];
        if (action.kind == NavigationActionKind::RepeatEnd)
            throw std::runtime_error("repeat group has an unmatched end marker");
        if (action.kind != NavigationActionKind::RepeatBegin) {
            result.actions.push_back(action);
            if (result.actions.size() > max_actions) throw std::runtime_error("navigation sequence expands beyond its action limit");
            continue;
        }
        if (action.repeat_count > 1000U) throw std::runtime_error("repeat group count is above 1000");
        std::size_t end = i + 1U;
        for (; end < input.size() && input[end].kind != NavigationActionKind::RepeatEnd; ++end) {
            if (input[end].kind == NavigationActionKind::RepeatBegin)
                throw std::runtime_error("nested repeat groups are not supported");
        }
        if (end >= input.size()) throw std::runtime_error("repeat group is missing its end marker");
        if (end == i + 1U) throw std::runtime_error("repeat group cannot be empty");
        for (std::size_t j = i + 1U; j < end; ++j) {
            if (input[j].kind == NavigationActionKind::Menu)
                throw std::runtime_error("a repeat group cannot contain a menu jump");
            if (input[j].kind == NavigationActionKind::RepeatEnd || input[j].kind == NavigationActionKind::RepeatBegin)
                throw std::runtime_error("nested repeat groups are not supported");
            if (input[j].repeat_count == 0U)
                throw std::runtime_error("an action inside a repeat group cannot itself repeat forever");
        }
        if (action.repeat_count == 0U) {
            if (end + 1U != input.size()) throw std::runtime_error("an infinitely repeated group must be final");
            bool has_play_title = false;
            for (std::size_t j = i + 1U; j < end; ++j) has_play_title = has_play_title || input[j].kind == NavigationActionKind::PlayTitle;
            if (!has_play_title) throw std::runtime_error("an infinitely repeated group must contain at least one Play Title action");
            result.infinite_loop_start = result.actions.size();
            result.actions.insert(result.actions.end(), input.begin() + static_cast<std::ptrdiff_t>(i + 1U),
                                   input.begin() + static_cast<std::ptrdiff_t>(end));
            if (result.actions.size() > max_actions) throw std::runtime_error("navigation sequence expands beyond its action limit");
        } else {
            for (std::uint16_t r = 0; r < action.repeat_count; ++r) {
                result.actions.insert(result.actions.end(), input.begin() + static_cast<std::ptrdiff_t>(i + 1U),
                                       input.begin() + static_cast<std::ptrdiff_t>(end));
                if (result.actions.size() > max_actions) throw std::runtime_error("navigation sequence expands beyond its action limit");
            }
        }
        i = end;
    }
    return result;
}

struct ButtonStyle {
    std::string font_family; // empty = choose the first installed default sans-serif font
    int font_size_px = 60;
    bool bold = true;
    bool italic = false;
    int corner_radius = 16;
    ButtonStateStyle normal{};
    // Persistent state for audio/subtitle buttons whose option is currently
    // active in the player. Yellow is deliberately distinct from the blue
    // keyboard/mouse selection state and can be customized by the user.
    ButtonStateStyle active{{255,215,0,255},{0,0,0,0},{255,215,0,255},6};
    ButtonStateStyle selected{{128,200,255,255},{20,20,20,205},{128,200,255,255},6};
    ButtonStateStyle activated{{180,225,255,255},{20,20,20,220},{180,225,255,255},10};
};

struct AudioStreamSettings {
    // 0-based ordinal within the source file's audio streams.
    int source_ordinal = 0;
    // Empty means preserve the source language tag (with the legacy first-track
    // title fallback used when the source has no language tag).
    std::string language;
    // False preserves the historical behavior: remux a compliant source track,
    // otherwise encode it with the title's fallback audio settings.
    bool override_encoding = false;
    // 0 preserves the source channel count. Positive values request a downmix
    // to that many channels; upmixing is intentionally rejected.
    int output_channels = 0;
    EncodingProfile encoding;
    EncodingProfile uhd_encoding = default_uhd_encoding_profile();
    EncodingProfile dvd_encoding = default_dvd_encoding_profile();
};

struct SubtitleStyle {
    // False leaves advanced renderer-specific styling at its defaults. Text
    // subtitles still receive the concrete font, resolution-independent size,
    // and default placement below; bitmap PGS/SUP streams keep their rasterized
    // glyphs and only use applicable positioning controls.
    bool override_style = false;
    // Empty means automatic selection from the installed sans-serif fallback
    // list. These two integer values use a resolution-independent subtitle
    // coordinate system whose vertical extent is 1080 units; one unit is one
    // pixel on a 1080-line subtitle plane. UHD Blu-ray also uses a 1080-line
    // subtitle plane, so its conversion is intentionally 1:1.
    std::string font_family;
    int font_size_px = 80;
    Rgba font_color{255, 255, 255, 255};
    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool strikeout = false;
    double line_spacing = 1.0;
    double border_width = 2.0;
    int bottom_offset_px = 80;
    double fade_in_ms = 0.0;
    double fade_out_ms = 0.0;

    // DVD/spumux-only text rendering controls. Geometry/fps remain automatic
    // because they are constrained by the selected DVD video mode.
    Rgba dvd_outline_color{0, 0, 0, 255};
    Rgba dvd_shadow_color{0, 0, 0, 255};
    int dvd_shadow_offset_x = 0;
    int dvd_shadow_offset_y = 0;
    std::string dvd_horizontal_alignment = "center"; // default/left/center/right
    std::string dvd_vertical_alignment = "bottom";   // top/center/bottom
    int dvd_left_margin_px = 40;
    int dvd_right_margin_px = 40;
    int dvd_top_margin_px = 20;
    int dvd_bottom_margin_px = 36;
    bool dvd_force_display = false;
};

inline SubtitleStyle default_new_project_subtitle_style() {
    return SubtitleStyle{};
}

struct SubtitleStreamSettings {
    int source_ordinal = 0;
    // Empty means preserve the source subtitle language tag.
    std::string language;
    SubtitleStyle style;
};

struct ExternalSubtitle {
    std::filesystem::path source;
    // Empty/invalid is normalized to und at author time.
    std::string language = "und";
    SubtitleStyle style;
};

inline EncodingProfile& encoding_for_target(AudioStreamSettings& stream, DiscTarget target) {
    switch (target) {
        case DiscTarget::UltraHdBluRay2160: return stream.uhd_encoding;
        case DiscTarget::DvdVideo480p: return stream.dvd_encoding;
        case DiscTarget::BluRay1080: return stream.encoding;
    }
    return stream.encoding;
}
inline const EncodingProfile& encoding_for_target(const AudioStreamSettings& stream, DiscTarget target) {
    return encoding_for_target(const_cast<AudioStreamSettings&>(stream), target);
}

struct Title {
    std::string name;
    std::filesystem::path source;
    std::string audio_language = "eng";
    std::vector<AudioStreamSettings> audio_stream_settings;
    std::vector<SubtitleStreamSettings> subtitle_stream_settings;
    std::vector<ExternalSubtitle> external_subtitles;
    // Initial stream selection when this title starts.  Audio 0 and subtitle
    // -1 preserve the player's language/default selection; subtitle 0 means
    // explicitly start with subtitle display disabled. Positive values are
    // one-based authored stream numbers.
    int default_audio_stream = 0;
    int default_subtitle_stream = -1;
    // Per-title inherited subtitle rendering defaults. Individual subtitle
    // streams and attachments override these when their own style is enabled.
    SubtitleStyle subtitle_default_style;
    ChapterMode chapter_mode = ChapterMode::SourceOrFiveMinute;
    // Manual additional chapter starts. Chapter 1 is always the title start (0s).
    std::vector<double> chapters_seconds;
    double chapter_interval_seconds = 300.0;
    // Per-target timing overrides. "inherit" uses the project menu/default
    // frame-rate selection; "auto" independently matches this title's source.
    std::string frame_rate = "inherit";
    std::string uhd_frame_rate = "inherit";
    std::string dvd_frame_rate = "inherit";
    // Per-target raster/aspect choices. "auto" selects the legal mode nearest
    // the source raster and display aspect; exact ties prefer scaling up.
    std::string resolution = "auto";
    std::string uhd_resolution = "auto";
    std::string dvd_resolution = "auto";
    std::string aspect_ratio = "auto";
    std::string uhd_aspect_ratio = "auto";
    std::string dvd_aspect_ratio = "auto";
    // Separate fallback profiles let one project be rendered as either a
    // standard 1080p Blu-ray or an Ultra HD Blu-ray without losing settings.
    EncodingProfile encoding;
    EncodingProfile uhd_encoding = default_uhd_encoding_profile();
    EncodingProfile dvd_encoding = default_dvd_encoding_profile();
    bool force_reencode = false;
    // Empty means the format-specific default for the player's Menu button:
    // return to the top/main menu when one exists, or stop on a menu-less disc.
    // A non-empty sequence overrides that default for this title.
    std::vector<NavigationAction> menu_button_actions;
    // Semantic User Operation Prohibition mask. Zero means prohibit nothing.
    std::uint64_t prohibited_user_operations = 0;
};

inline const AudioStreamSettings* find_audio_stream_settings(const Title& title, int source_ordinal) {
    const auto it = std::find_if(title.audio_stream_settings.begin(), title.audio_stream_settings.end(),
        [source_ordinal](const AudioStreamSettings& s) { return s.source_ordinal == source_ordinal; });
    return it == title.audio_stream_settings.end() ? nullptr : &*it;
}
inline AudioStreamSettings* find_audio_stream_settings(Title& title, int source_ordinal) {
    return const_cast<AudioStreamSettings*>(find_audio_stream_settings(static_cast<const Title&>(title), source_ordinal));
}
inline const SubtitleStreamSettings* find_subtitle_stream_settings(const Title& title, int source_ordinal) {
    const auto it = std::find_if(title.subtitle_stream_settings.begin(), title.subtitle_stream_settings.end(),
        [source_ordinal](const SubtitleStreamSettings& s) { return s.source_ordinal == source_ordinal; });
    return it == title.subtitle_stream_settings.end() ? nullptr : &*it;
}
inline EncodingProfile effective_audio_encoding_for_stream(const Title& title, DiscTarget target, int source_ordinal) {
    EncodingProfile effective;
    switch (target) {
        case DiscTarget::UltraHdBluRay2160: effective = title.uhd_encoding; break;
        case DiscTarget::DvdVideo480p: effective = title.dvd_encoding; break;
        case DiscTarget::BluRay1080: effective = title.encoding; break;
    }
    if (const auto* stream = find_audio_stream_settings(title, source_ordinal); stream && stream->override_encoding)
        copy_audio_encoding_settings(effective, encoding_for_target(*stream, target));
    return effective;
}

inline std::string& frame_rate_for_target(Title& title, DiscTarget target) {
    switch (target) {
        case DiscTarget::UltraHdBluRay2160: return title.uhd_frame_rate;
        case DiscTarget::DvdVideo480p: return title.dvd_frame_rate;
        case DiscTarget::BluRay1080: return title.frame_rate;
    }
    return title.frame_rate;
}
inline const std::string& frame_rate_for_target(const Title& title, DiscTarget target) {
    switch (target) {
        case DiscTarget::UltraHdBluRay2160: return title.uhd_frame_rate;
        case DiscTarget::DvdVideo480p: return title.dvd_frame_rate;
        case DiscTarget::BluRay1080: return title.frame_rate;
    }
    return title.frame_rate;
}
inline std::string& resolution_for_target(Title& title, DiscTarget target) {
    switch (target) {
        case DiscTarget::UltraHdBluRay2160: return title.uhd_resolution;
        case DiscTarget::DvdVideo480p: return title.dvd_resolution;
        case DiscTarget::BluRay1080: return title.resolution;
    }
    return title.resolution;
}
inline const std::string& resolution_for_target(const Title& title, DiscTarget target) {
    switch (target) {
        case DiscTarget::UltraHdBluRay2160: return title.uhd_resolution;
        case DiscTarget::DvdVideo480p: return title.dvd_resolution;
        case DiscTarget::BluRay1080: return title.resolution;
    }
    return title.resolution;
}
inline std::string& aspect_ratio_for_target(Title& title, DiscTarget target) {
    switch (target) {
        case DiscTarget::UltraHdBluRay2160: return title.uhd_aspect_ratio;
        case DiscTarget::DvdVideo480p: return title.dvd_aspect_ratio;
        case DiscTarget::BluRay1080: return title.aspect_ratio;
    }
    return title.aspect_ratio;
}
inline const std::string& aspect_ratio_for_target(const Title& title, DiscTarget target) {
    switch (target) {
        case DiscTarget::UltraHdBluRay2160: return title.uhd_aspect_ratio;
        case DiscTarget::DvdVideo480p: return title.dvd_aspect_ratio;
        case DiscTarget::BluRay1080: return title.aspect_ratio;
    }
    return title.aspect_ratio;
}

struct MenuButton {
    std::string label;
    Rect bounds;
    MenuButtonTargetKind target_kind = MenuButtonTargetKind::Title;
    std::uint16_t target_title = 1;
    std::uint16_t target_chapter = 0;
    std::uint16_t target_stream = 1;
    std::string target_menu_id;
    // Project format v4 stores an ordered action sequence.  The legacy target
    // fields above are retained as a compatibility mirror for v1-v3 projects
    // and older callers that construct a one-action button.
    std::vector<NavigationAction> actions;
    MenuButtonKind kind = MenuButtonKind::Text;
    bool use_custom_style = false;
    ButtonStyle style;
    std::filesystem::path normal_image;
    std::filesystem::path selected_image;
    Rgba image_highlight_color{128,200,255,112};
    bool auto_submenu_link = false;
};

struct MenuOverlay {
    MenuOverlayKind kind = MenuOverlayKind::Text;
    Rect bounds{240,240,1280,180};
    std::string text = "Label";
    std::string font_family; // empty = automatic installed sans-serif default
    int font_size_px = 84;
    bool bold = true;
    bool italic = false;
    Rgba text_color{255,255,255,255};
    std::filesystem::path image;
};

struct Menu {
    std::string id = "top";
    std::string name = "Top Menu";
    bool inherit_background_image = true;
    bool inherit_background_color = true;
    bool inherit_button_style = true;
    bool inherit_encoding = true;
    bool inherit_audio = true;
    bool inherit_duration = true;

    std::filesystem::path background_image;
    std::filesystem::path background_video;
    Rgba background_color{0,0,0,255};
    std::filesystem::path audio_source;
    std::vector<MenuButton> buttons;
    std::vector<MenuOverlay> overlays;
    ButtonStyle default_button_style;
    // Menu layout is always stored in the 3840x2160 project design space.
    // Authoring derives the target video raster and the Blu-ray 1920x1080
    // graphics plane from these coordinates.
    int width = kProjectDesignWidth;
    int height = kProjectDesignHeight;
    double duration_seconds = 30.0;
    // When source media is present, repeat the complete menu media cycle.
    // Disabled menus play their media once and then hold the final menu frame.
    bool loop_media = true;
    EncodingProfile encoding;
    EncodingProfile uhd_encoding = default_uhd_encoding_profile();
    EncodingProfile dvd_encoding = default_dvd_encoding_profile();

    bool auto_back_button = true;
    MenuButton back_button = [] {
        MenuButton b;
        b.label = "Back";
        b.bounds = {3280,1920,440,140};
        b.target_kind = MenuButtonTargetKind::Menu;
        return b;
    }();

    std::vector<Menu> submenus;
};

inline const EncodingProfile& encoding_for_target(const Title& title, DiscTarget target) {
    switch (target) {
        case DiscTarget::UltraHdBluRay2160: return title.uhd_encoding;
        case DiscTarget::DvdVideo480p: return title.dvd_encoding;
        case DiscTarget::BluRay1080: return title.encoding;
    }
    return title.encoding;
}
inline EncodingProfile& encoding_for_target(Title& title, DiscTarget target) {
    switch (target) {
        case DiscTarget::UltraHdBluRay2160: return title.uhd_encoding;
        case DiscTarget::DvdVideo480p: return title.dvd_encoding;
        case DiscTarget::BluRay1080: return title.encoding;
    }
    return title.encoding;
}
inline const EncodingProfile& encoding_for_target(const Menu& menu, DiscTarget target) {
    switch (target) {
        case DiscTarget::UltraHdBluRay2160: return menu.uhd_encoding;
        case DiscTarget::DvdVideo480p: return menu.dvd_encoding;
        case DiscTarget::BluRay1080: return menu.encoding;
    }
    return menu.encoding;
}
inline EncodingProfile& encoding_for_target(Menu& menu, DiscTarget target) {
    switch (target) {
        case DiscTarget::UltraHdBluRay2160: return menu.uhd_encoding;
        case DiscTarget::DvdVideo480p: return menu.dvd_encoding;
        case DiscTarget::BluRay1080: return menu.encoding;
    }
    return menu.encoding;
}

inline constexpr bool button_target_is_title_scoped(MenuButtonTargetKind kind) {
    return kind != MenuButtonTargetKind::Menu;
}
inline constexpr bool navigation_action_is_title_scoped(NavigationActionKind kind) {
    return kind == NavigationActionKind::PlayTitle || kind == NavigationActionKind::AudioTrack ||
           kind == NavigationActionKind::SubtitleTrack || kind == NavigationActionKind::SubtitleOff;
}

inline NavigationAction legacy_button_action(const MenuButton& button) {
    NavigationAction action;
    switch (button.target_kind) {
        case MenuButtonTargetKind::Title: action.kind = NavigationActionKind::PlayTitle; break;
        case MenuButtonTargetKind::Menu: action.kind = NavigationActionKind::Menu; break;
        case MenuButtonTargetKind::AudioTrack: action.kind = NavigationActionKind::AudioTrack; break;
        case MenuButtonTargetKind::SubtitleTrack: action.kind = NavigationActionKind::SubtitleTrack; break;
        case MenuButtonTargetKind::SubtitleOff: action.kind = NavigationActionKind::SubtitleOff; break;
    }
    action.target_title = button.target_title;
    action.target_chapter = button.target_chapter;
    action.target_stream = button.target_stream;
    action.target_menu_id = button.target_menu_id;
    return action;
}
inline std::vector<NavigationAction> button_action_sequence(const MenuButton& button) {
    return button.actions.empty() ? std::vector<NavigationAction>{legacy_button_action(button)} : button.actions;
}
inline void mirror_first_action_to_legacy_target(MenuButton& button) {
    const auto it = std::find_if(button.actions.begin(), button.actions.end(), [](const NavigationAction& action) {
        return !navigation_action_is_repeat_marker(action.kind);
    });
    if (it == button.actions.end()) return;
    const auto& action = *it;
    switch (action.kind) {
        case NavigationActionKind::PlayTitle: button.target_kind = MenuButtonTargetKind::Title; break;
        case NavigationActionKind::Menu: button.target_kind = MenuButtonTargetKind::Menu; break;
        case NavigationActionKind::AudioTrack: button.target_kind = MenuButtonTargetKind::AudioTrack; break;
        case NavigationActionKind::SubtitleTrack: button.target_kind = MenuButtonTargetKind::SubtitleTrack; break;
        case NavigationActionKind::SubtitleOff: button.target_kind = MenuButtonTargetKind::SubtitleOff; break;
        case NavigationActionKind::RepeatBegin:
        case NavigationActionKind::RepeatEnd: return;
    }
    button.target_title = action.target_title;
    button.target_chapter = action.target_chapter;
    button.target_stream = action.target_stream;
    button.target_menu_id = action.target_menu_id;
}
inline void set_button_action_sequence(MenuButton& button, std::vector<NavigationAction> actions) {
    button.actions = std::move(actions);
    mirror_first_action_to_legacy_target(button);
}
inline void set_single_button_action(MenuButton& button, NavigationAction action) {
    set_button_action_sequence(button, std::vector<NavigationAction>{std::move(action)});
}

inline void remove_title_references(std::vector<NavigationAction>& actions, std::uint16_t removed_title) {
    actions.erase(std::remove_if(actions.begin(), actions.end(), [removed_title](const NavigationAction& action) {
        return navigation_action_is_title_scoped(action.kind) && action.target_title == removed_title;
    }), actions.end());
    for (auto& action : actions)
        if (navigation_action_is_title_scoped(action.kind) && action.target_title > removed_title) --action.target_title;
    // If deleting a title removed the only actions inside a repeat group, drop
    // the now-empty group boundaries too so editing remains valid.
    for (std::size_t i = 0; i + 1U < actions.size();) {
        if (actions[i].kind == NavigationActionKind::RepeatBegin && actions[i + 1U].kind == NavigationActionKind::RepeatEnd) {
            actions.erase(actions.begin() + static_cast<std::ptrdiff_t>(i),
                          actions.begin() + static_cast<std::ptrdiff_t>(i + 2U));
            if (i > 0U) --i;
        } else ++i;
    }
}
inline void remove_title_references(Menu& menu, std::uint16_t removed_title) {
    menu.buttons.erase(std::remove_if(menu.buttons.begin(), menu.buttons.end(), [removed_title](MenuButton& button) {
        if (button.actions.empty())
            return button_target_is_title_scoped(button.target_kind) && button.target_title == removed_title;
        remove_title_references(button.actions, removed_title);
        if (button.actions.empty()) return true;
        mirror_first_action_to_legacy_target(button);
        return false;
    }), menu.buttons.end());
    for (auto& button : menu.buttons) {
        if (button.actions.empty()) {
            if (button_target_is_title_scoped(button.target_kind) && button.target_title > removed_title) --button.target_title;
        } else {
            mirror_first_action_to_legacy_target(button);
        }
    }
    if (!menu.back_button.actions.empty()) {
        remove_title_references(menu.back_button.actions, removed_title);
        mirror_first_action_to_legacy_target(menu.back_button);
    }
    for (auto& child : menu.submenus) remove_title_references(child, removed_title);
}

struct Project {
    std::string volume_label = "BLURAY";
    std::filesystem::path output_image;
    std::filesystem::path work_directory;
    std::vector<Title> titles;
    Menu menu;
    DiscTarget target = DiscTarget::BluRay1080;
    // Decimal bytes, matching marketed optical-disc capacities. Zero means
    // unlimited (for example, image playback from storage rather than physical media).
    std::uint64_t disc_capacity_bytes = default_disc_capacity_bytes(DiscTarget::BluRay1080);
    // When enabled, authoring calculates per-segment video bitrates from the
    // selected capacity and ignores the stored manual video-bitrate fields.
    bool automatic_video_bitrate = true;
    // "auto" preserves a legal source cadence when possible and otherwise
    // selects the closest target-legal cadence with an emphasis on avoiding
    // dropped frames and irregular timing.  Explicit CLI overrides are also
    // persisted here for reproducible renders.
    std::string frame_rate = "auto";
    // Menu video modes are persisted independently for each output target so
    // switching between Blu-ray, UHD, and DVD never overwrites another target's
    // menu choice. Blu-ray defaults to 1080p. UHD also defaults to 1080p as a
    // temporary VLC compatibility measure: VLC currently mouse-hit-tests the
    // 1920x1080 Interactive Graphics plane with unscaled 4K video coordinates.
    // DVD intentionally defaults to NTSC full-D1 rather than PAL.
    std::string menu_resolution = "1920x1080";
    std::string menu_uhd_resolution = "1920x1080";
    std::string menu_dvd_resolution = "720x480";
    std::string menu_aspect_ratio = "16:9";
    std::string menu_uhd_aspect_ratio = "16:9";
    std::string menu_dvd_aspect_ratio = "16:9";
    bool keep_work_directory = false;
    bool use_encode_cache = true;
    bool use_compliance_cache = true;
    // Build-only cache refresh controls. They are intentionally not persisted
    // in project files: a refresh request applies to the next authoring run.
    bool refresh_encode_cache = false;
    bool refresh_compliance_cache = false;
    bool force_reencode = false;
    // With a menu, an empty sequence means normal First Playback opens the top
    // menu. With no menu, an empty sequence means the authoring default: play
    // every title in order exactly once, then stop. A non-empty sequence always
    // overrides that default.
    std::vector<NavigationAction> first_play_actions;
};

inline bool project_has_menu(const Project& project) { return !project.menu.id.empty(); }

inline std::vector<NavigationAction> default_menuless_startup_actions(std::size_t title_count) {
    std::vector<NavigationAction> actions;
    actions.reserve(title_count);
    for (std::size_t i = 0; i < title_count; ++i) {
        NavigationAction action;
        action.kind = NavigationActionKind::PlayTitle;
        action.target_title = static_cast<std::uint16_t>(i + 1U);
        actions.push_back(std::move(action));
    }
    return actions;
}

inline bool is_default_menuless_startup_sequence(std::span<const NavigationAction> actions, std::size_t title_count) {
    if (actions.size() != title_count) return false;
    for (std::size_t i = 0; i < actions.size(); ++i) {
        const auto& action = actions[i];
        if (action.kind != NavigationActionKind::PlayTitle || action.target_title != i + 1U ||
            action.target_chapter != 0U || action.repeat_count != 1U) return false;
    }
    return true;
}

inline const std::string& menu_resolution_for_target(const Project& project, DiscTarget target) {
    switch (target) {
        case DiscTarget::UltraHdBluRay2160: return project.menu_uhd_resolution;
        case DiscTarget::DvdVideo480p: return project.menu_dvd_resolution;
        case DiscTarget::BluRay1080: return project.menu_resolution;
    }
    return project.menu_resolution;
}
inline std::string& menu_resolution_for_target(Project& project, DiscTarget target) {
    return const_cast<std::string&>(menu_resolution_for_target(static_cast<const Project&>(project), target));
}
inline const std::string& menu_aspect_ratio_for_target(const Project& project, DiscTarget target) {
    switch (target) {
        case DiscTarget::UltraHdBluRay2160: return project.menu_uhd_aspect_ratio;
        case DiscTarget::DvdVideo480p: return project.menu_dvd_aspect_ratio;
        case DiscTarget::BluRay1080: return project.menu_aspect_ratio;
    }
    return project.menu_aspect_ratio;
}
inline std::string& menu_aspect_ratio_for_target(Project& project, DiscTarget target) {
    return const_cast<std::string&>(menu_aspect_ratio_for_target(static_cast<const Project&>(project), target));
}

struct NewProjectDefaults {
    std::string volume_label = "BLURAY";
    DiscTarget target = DiscTarget::BluRay1080;
    // Per-target size defaults are kept independently so changing the default
    // output format does not discard the preferred physical-media size for
    // the other formats. Zero means Unlimited.
    std::uint64_t disc_capacity_bytes = default_disc_capacity_bytes(DiscTarget::BluRay1080);
    std::uint64_t disc_uhd_capacity_bytes = default_disc_capacity_bytes(DiscTarget::UltraHdBluRay2160);
    std::uint64_t disc_dvd_capacity_bytes = default_disc_capacity_bytes(DiscTarget::DvdVideo480p);
    bool use_encode_cache = true;
    bool use_compliance_cache = true;
    bool force_reencode_new_titles = false;
    EncodingProfile title_encoding;
    EncodingProfile title_uhd_encoding = default_uhd_encoding_profile();
    EncodingProfile title_dvd_encoding = default_dvd_encoding_profile();
    EncodingProfile menu_encoding;
    EncodingProfile menu_uhd_encoding = default_uhd_encoding_profile();
    EncodingProfile menu_dvd_encoding = default_dvd_encoding_profile();
    ButtonStyle button_style;
    int button_width = 1440;
    int button_height = 152;
    std::string label_font_family; // empty = automatic installed sans-serif default
    int label_font_size_px = 84;
    bool label_bold = true;
    bool label_italic = false;
    SubtitleStyle subtitle_style = default_new_project_subtitle_style();
    double menu_duration_seconds = 30.0;
    // New-project menu raster defaults are independent for each target. UHD
    // intentionally defaults to 1080p while VLC has a 4K-menu mouse-coordinate
    // bug; users may still opt into 3840x2160 for other players.
    std::string menu_resolution = "1920x1080";
    std::string menu_uhd_resolution = "1920x1080";
    std::string menu_dvd_resolution = "720x480";
};

inline constexpr std::uint64_t new_project_disc_capacity_for_target(const NewProjectDefaults& defaults, DiscTarget target) {
    switch (target) {
        case DiscTarget::BluRay1080: return defaults.disc_capacity_bytes;
        case DiscTarget::UltraHdBluRay2160: return defaults.disc_uhd_capacity_bytes;
        case DiscTarget::DvdVideo480p: return defaults.disc_dvd_capacity_bytes;
    }
    return defaults.disc_capacity_bytes;
}

inline void apply_new_project_defaults(Project& project, const NewProjectDefaults& defaults) {
    project.volume_label = defaults.volume_label;
    project.target = defaults.target;
    project.disc_capacity_bytes = new_project_disc_capacity_for_target(defaults, defaults.target);
    project.automatic_video_bitrate = true;
    project.use_encode_cache = defaults.use_encode_cache;
    project.use_compliance_cache = defaults.use_compliance_cache;
    project.menu.encoding = defaults.menu_encoding;
    project.menu.uhd_encoding = defaults.menu_uhd_encoding;
    project.menu.dvd_encoding = defaults.menu_dvd_encoding;
    project.menu.default_button_style = defaults.button_style;
    project.menu.duration_seconds = defaults.menu_duration_seconds;
    project.menu_resolution = defaults.menu_resolution;
    project.menu_uhd_resolution = defaults.menu_uhd_resolution;
    project.menu_dvd_resolution = defaults.menu_dvd_resolution;
}

inline Title make_title_from_defaults(const NewProjectDefaults& defaults) {
    Title title;
    title.encoding = defaults.title_encoding;
    title.uhd_encoding = defaults.title_uhd_encoding;
    title.dvd_encoding = defaults.title_dvd_encoding;
    title.force_reencode = defaults.force_reencode_new_titles;
    title.subtitle_default_style = defaults.subtitle_style;
    return title;
}

} // namespace bdmvauthor
