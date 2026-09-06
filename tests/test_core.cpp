// SPDX-License-Identifier: Apache-2.0
#include "bdmvauthor/author.hpp"
#include "bdmvauthor/audio_tools.hpp"
#include "bdmvauthor/compliance.hpp"
#include "bdmvauthor/hdmv.hpp"
#include "bdmvauthor/progress.hpp"
#include "bdmvauthor/media_fingerprint.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace bdmvauthor;

namespace {
using Bytes = std::vector<unsigned char>;

void req(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

void remove_test_tree(const std::filesystem::path& path) {
#ifdef _WIN32
    // Windows antivirus/indexing can briefly reopen a freshly closed test
    // artifact.  First make sure our own streams have gone out of scope, then
    // tolerate only those transient sharing violations during cleanup.
    std::error_code ec;
    for (unsigned attempt = 0; attempt < 40; ++attempt) {
        std::filesystem::remove_all(path, ec);
        if (!ec) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
#endif
    std::filesystem::remove_all(path);
}

void need(const Bytes& b, std::size_t pos, std::size_t len, const char* message) {
    if (pos > b.size() || len > b.size() - pos) {
        throw std::runtime_error(message);
    }
}

unsigned be16(const Bytes& b, std::size_t p) {
    need(b, p, 2, "truncated be16");
    return (unsigned(b[p]) << 8) | b[p + 1];
}

unsigned be24(const Bytes& b, std::size_t p) {
    need(b, p, 3, "truncated be24");
    return (unsigned(b[p]) << 16) | (unsigned(b[p + 1]) << 8) | b[p + 2];
}

unsigned be32(const Bytes& b, std::size_t p) {
    need(b, p, 4, "truncated be32");
    return (unsigned(b[p]) << 24) | (unsigned(b[p + 1]) << 16) |
           (unsigned(b[p + 2]) << 8) | b[p + 3];
}

std::uint64_t be33(const Bytes& b, std::size_t p) {
    need(b, p, 5, "truncated be33");
    return (std::uint64_t(b[p] & 1U) << 32) | std::uint64_t(be32(b, p + 1));
}

struct ParsedMovieObject {
    std::uint8_t flags = 0;
    std::vector<hdmv::NavCommand> commands;
};

std::vector<ParsedMovieObject> parse_movie_objects(const Bytes& mobj) {
    need(mobj, 48, 2, "truncated MOBJ object count");
    const auto count = be16(mobj, 48);
    std::size_t p = 50;
    std::vector<ParsedMovieObject> out;
    out.reserve(count);
    for (unsigned i = 0; i < count; ++i) {
        need(mobj, p, 4, "truncated MOBJ object header");
        ParsedMovieObject object;
        object.flags = mobj[p];
        const auto command_count = be16(mobj, p + 2);
        p += 4;
        object.commands.reserve(command_count);
        for (unsigned j = 0; j < command_count; ++j) {
            need(mobj, p, 12, "truncated MOBJ navigation command");
            object.commands.push_back({be32(mobj, p), be32(mobj, p + 4), be32(mobj, p + 8)});
            p += 12;
        }
        out.push_back(std::move(object));
    }
    return out;
}

void check_button_bog(const Bytes& ics, std::size_t& p, unsigned expected_id,
                      unsigned expected_target, const std::array<unsigned,4>& neighbors) {
    need(ics, p, 3, "truncated IG BOG");
    req(be16(ics, p) == expected_id, "BOG default-valid button regression");
    p += 2;
    req(ics[p++] == 1, "each menu BOG must contain exactly one button");

    need(ics, p, 47, "truncated IG button");
    req(be16(ics, p) == expected_id, "IG button id regression");
    p += 2;
    req(be16(ics, p) == 0xffff, "unexpected numeric-select mapping");
    p += 2;
    req((ics[p] & 0x80U) == 0, "button auto-action must be disabled");
    ++p;

    // Position.
    p += 4;

    req(be16(ics, p) == neighbors[0], "IG up-neighbor regression");
    p += 2;
    req(be16(ics, p) == neighbors[1], "IG down-neighbor regression");
    p += 2;
    req(be16(ics, p) == neighbors[2], "IG left-neighbor regression");
    p += 2;
    req(be16(ics, p) == neighbors[3], "IG right-neighbor regression");
    p += 2;

    const unsigned base_object = (expected_id - 1U) * 3U;
    req(be16(ics, p) == base_object && be16(ics, p + 2) == base_object,
        "normal-state object reference regression");
    p += 4;
    ++p; // normal repeat flag/reserved bits

    req(ics[p++] == 0xff, "selected-state sound id regression");
    req(be16(ics, p) == base_object + 1U && be16(ics, p + 2) == base_object + 1U,
        "selected-state object reference regression");
    p += 4;
    ++p; // selected repeat flag/reserved bits

    req(ics[p++] == 0xff, "activated-state sound id regression");
    req(be16(ics, p) == base_object + 2U && be16(ics, p + 2) == base_object + 2U,
        "activated-state object reference regression");
    p += 4;

    req(be16(ics, p) == 1, "menu button must contain one navigation command");
    p += 2;
    req(be32(ics, p) == 0x21810000U, "button command is not Jump_Title immediate");
    req(be32(ics, p + 4) == expected_target, "button Jump_Title target regression");
    req(be32(ics, p + 8) == 0, "button Jump_Title second operand regression");
    p += 12;
}

struct ParsedIgButton { unsigned id=0,x=0,y=0,normal_object=0; };
std::vector<std::vector<ParsedIgButton>> parse_menu_bogs(const Bytes& ics) {
    // BDMV Author emits one page with no in/out effects. Parse just enough of
    // the ICS to verify BOG membership and shared indicator geometry.
    std::size_t p=17;
    need(ics,p,21,"truncated IG page while parsing BOGs");
    p+=2; // page id/version
    p+=8; // UO mask
    req(ics[p++]==0&&ics[p++]==0,"unexpected in-effects while parsing BOGs");
    req(ics[p++]==0&&ics[p++]==0,"unexpected out-effects while parsing BOGs");
    ++p; // animation frame rate
    p+=2; // default selected
    p+=2; // default activated
    ++p; // palette id
    const auto bog_count=ics[p++];
    std::vector<std::vector<ParsedIgButton>> out;out.reserve(bog_count);
    for(unsigned bi=0;bi<bog_count;++bi){
        need(ics,p,3,"truncated BOG header");p+=2;const auto count=ics[p++];
        std::vector<ParsedIgButton> buttons;buttons.reserve(count);
        for(unsigned j=0;j<count;++j){
            need(ics,p,35,"truncated BOG button");
            ParsedIgButton info;info.id=be16(ics,p);info.x=be16(ics,p+5);info.y=be16(ics,p+7);info.normal_object=be16(ics,p+17);
            const auto command_count=be16(ics,p+33);p+=35;need(ics,p,static_cast<std::size_t>(command_count)*12U,"truncated BOG commands");p+=static_cast<std::size_t>(command_count)*12U;
            buttons.push_back(info);
        }
        out.push_back(std::move(buttons));
    }
    req(p==ics.size(),"unexpected bytes after parsed IG BOGs");
    return out;
}

void check_menu_ics(const Bytes& ics, const std::vector<std::array<unsigned,4>>& neighbors, double duration_seconds) {
    const unsigned button_count = static_cast<unsigned>(neighbors.size());
    req(be16(ics, 0) == 1920 && be16(ics, 2) == 1080, "IG dimensions");
    req(be24(ics, 9) == ics.size() - 12, "ICS data length");
    (void)duration_seconds;
    req(ics[12] == 0x80, "IG stream/ui model must be preloaded always-on (stream_model=1, ui_model=0)");
    req(be24(ics, 13) == 0 && ics[16] == 1, "preloaded IG user-timeout/page framing");

    // Parse the page in exactly the field order used by libbluray's IG decoder:
    // page id/version, UO mask, in/out effect sequences, animation rate,
    // default selected/activated ids, palette id, then Button Overlap Groups.
    std::size_t p = 17;
    need(ics, p, 21, "truncated IG page");
    req(ics[p++] == 0, "IG page id regression");
    req(ics[p++] == 0, "IG page version regression");
    p += 8; // UO mask
    req(ics[p++] == 0 && ics[p++] == 0, "unexpected menu in-effect sequence");
    req(ics[p++] == 0 && ics[p++] == 0, "unexpected menu out-effect sequence");
    req(ics[p++] == 0, "unexpected menu animation frame rate");
    req(be16(ics, p) == 0xffffU, "menu without an initializer must preserve PSR10 instead of forcing button 1 on every loop");
    p += 2;
    req(be16(ics, p) == 0x0000, "menu must not auto-activate the selected button at selection timeout");
    p += 2;
    req(ics[p++] == 0, "palette id regression");
    req(ics[p++] == button_count, "each independently visible button must have its own BOG");

    for (unsigned i = 0; i < button_count; ++i) {
        check_button_bog(ics, p, i + 1U, i + 1U, neighbors.at(i));
    }
    req(p == ics.size(), "unexpected bytes after IG button groups");
}
} // namespace

int main() {
    {
        req(target_max_combined_av_bitrate_kbps(DiscTarget::BluRay1080) == 48000, "Blu-ray combined AV ceiling regression");
        req(target_max_combined_av_bitrate_kbps(DiscTarget::UltraHdBluRay2160) == 70000, "UHD combined AV ceiling regression");
        req(target_max_combined_av_bitrate_kbps(DiscTarget::DvdVideo480p) == 10080, "DVD combined AV ceiling regression");
        AuthoringLimits runtime_limits{};
        req(runtime_limits.combined_transport_bitrate_kbps(DiscTarget::BluRay1080) == 48000, "Blu-ray runtime transport default regression");
        req(runtime_limits.combined_transport_bitrate_kbps(DiscTarget::UltraHdBluRay2160) == 70000, "UHD runtime transport default regression");
        req(runtime_limits.combined_transport_bitrate_kbps(DiscTarget::DvdVideo480p) == 10080, "DVD runtime transport default regression");
        runtime_limits.bluray_transport_bitrate_kbps = 90000;
        runtime_limits.dvd_transport_bitrate_kbps = 20000;
        req(runtime_limits.combined_transport_bitrate_kbps(DiscTarget::BluRay1080) == 48000, "normal mode must clamp a debug transport setting to the Blu-ray standard");
        req(runtime_limits.combined_transport_bitrate_kbps(DiscTarget::DvdVideo480p) == 10080, "normal mode must clamp a debug transport setting to the DVD standard");
        runtime_limits.allow_exceeding_format_limits = true;
        req(runtime_limits.combined_transport_bitrate_kbps(DiscTarget::BluRay1080) == 90000, "explicit debug allow-exceeding gate must expose configured Blu-ray transport limit");
        req(runtime_limits.combined_transport_bitrate_kbps(DiscTarget::DvdVideo480p) == 20000, "explicit debug allow-exceeding gate must expose configured DVD mux limit");
        req(lpcm_bitrate_kbps(96000, 24, 6) == 13824, "LPCM bitrate calculation regression");
        req(lpcm_bitrate_kbps(48000, 16, 2) == 1536, "stereo LPCM bitrate calculation regression");
        Title stream_title;
        stream_title.encoding.audio_codec=AudioCodec::Ac3;stream_title.encoding.ac3_bitrate_kbps=640;
        AudioStreamSettings stream_override;stream_override.source_ordinal=1;stream_override.override_encoding=true;
        stream_override.encoding.audio_codec=AudioCodec::Lpcm;stream_override.encoding.lpcm_sample_rate_hz=96000;stream_override.encoding.lpcm_bit_depth=24;
        stream_title.audio_stream_settings.push_back(stream_override);
        const auto inherited=effective_audio_encoding_for_stream(stream_title,DiscTarget::BluRay1080,0);
        const auto overridden=effective_audio_encoding_for_stream(stream_title,DiscTarget::BluRay1080,1);
        req(inherited.audio_codec==AudioCodec::Ac3 && inherited.ac3_bitrate_kbps==640,"unconfigured audio stream must retain title fallback");
        req(overridden.audio_codec==AudioCodec::Lpcm && overridden.lpcm_sample_rate_hz==96000 && overridden.lpcm_bit_depth==24,"per-stream audio encoding override regression");

        const ToolPaths tools{};
        req(tools.h264_provider==VideoEncoderProvider::Ffmpeg,"FFmpeg must remain the default H.264 provider");
        req(tools.hevc_provider==VideoEncoderProvider::Ffmpeg,"FFmpeg must remain the default H.265 provider");
        req(tools.x264=="x264" && tools.x265=="x265","standalone encoder command defaults changed");
    }

    try {
        int parsed_int = 0;
        req(parse_integer_exact("123", parsed_int) && parsed_int == 123, "from_chars integer parser decimal regression");
        req(parse_integer_exact("  +42", parsed_int) && parsed_int == 42, "from_chars integer parser leading-space/plus regression");
        req(!parse_integer_exact("42x", parsed_int), "from_chars integer parser must reject trailing junk");
        unsigned parsed_hex = 0;
        req(parse_integer_exact("ff", parsed_hex, 16) && parsed_hex == 255U, "from_chars integer parser hex regression");

        NewProjectDefaults subtitle_defaults;req(!subtitle_defaults.subtitle_style.override_style&&subtitle_defaults.subtitle_style.font_size_px==80&&subtitle_defaults.subtitle_style.bottom_offset_px==80,"new-project subtitle defaults must use the 80-unit 1080-line baseline without forcing a full style override");
        Project defaults;
        req(defaults.output_image.empty(), "new projects must not have a default output image path");
        req(defaults.menu.encoding.audio_codec == AudioCodec::Ac3, "AC-3 must be the default menu audio mode");
        req(defaults.menu.encoding.video_codec == VideoCodec::X264, "x264 must be the default menu video codec");
        req(defaults.menu.encoding.video_bitrate_kbps == 24000, "default video bitrate regression");
        req(defaults.menu.encoding.video_min_bitrate_kbps == 0 && defaults.menu.encoding.video_max_bitrate_kbps == 40000, "default Blu-ray video minrate/maxrate regression");
        const auto uhd_rate_defaults = default_uhd_encoding_profile();
        req(uhd_rate_defaults.video_min_bitrate_kbps == 0 && uhd_rate_defaults.video_max_bitrate_kbps == 86000, "default UHD video minrate/maxrate regression");
        const auto dvd_rate_defaults = default_dvd_encoding_profile();
        req(dvd_rate_defaults.video_min_bitrate_kbps == 0 && dvd_rate_defaults.video_max_bitrate_kbps == 9800, "default DVD video minrate/maxrate regression");
        EncodingProfile over_limit = defaults.menu.encoding;
        over_limit.video_bitrate_kbps = 45000;
        over_limit.video_max_bitrate_kbps = 50000;
        bool normal_rejected = false;
        try { AuthorEngine::validate_encoding_profile_for_target(over_limit, DiscTarget::BluRay1080, "rate-limit test"); }
        catch (const std::exception&) { normal_rejected = true; }
        req(normal_rejected, "normal mode must reject video average/maxrate above the Blu-ray format ceiling");
        AuthorEngine::validate_encoding_profile_for_target(over_limit, DiscTarget::BluRay1080, "debug rate-limit test", true);
        EncodingProfile audio_over_limit = defaults.menu.encoding;
        audio_over_limit.ac3_bitrate_kbps = 900;
        bool normal_audio_rejected = false;
        try { AuthorEngine::validate_encoding_profile_for_target(audio_over_limit, DiscTarget::BluRay1080, "audio rate-limit test"); }
        catch (const std::exception&) { normal_audio_rejected = true; }
        req(normal_audio_rejected, "normal mode must reject AC-3 bitrate above the format ceiling");
        AuthorEngine::validate_encoding_profile_for_target(audio_over_limit, DiscTarget::BluRay1080, "debug audio rate-limit test", true);
        req(defaults.menu.width==kProjectDesignWidth&&defaults.menu.height==kProjectDesignHeight,"project menu design space must be 3840x2160");
        req(target_geometry(DiscTarget::BluRay1080).video_width==1920&&target_geometry(DiscTarget::BluRay1080).graphics_width==1920,"1080p target geometry regression");
        req(target_geometry(DiscTarget::UltraHdBluRay2160).video_width==3840&&target_geometry(DiscTarget::UltraHdBluRay2160).graphics_width==1920,"UHD target must use 4K primary video and 1080p BD graphics plane");
        req(target_geometry(DiscTarget::DvdVideo480p).video_width==720&&target_geometry(DiscTarget::DvdVideo480p).video_height==480,"DVD target geometry regression");
        req(target_is_currently_authorable(DiscTarget::DvdVideo480p),"DVD target must be authorable");
        Title chapter_defaults;req(chapter_defaults.chapter_mode==ChapterMode::SourceOrFiveMinute&&chapter_defaults.chapter_interval_seconds==300.0,"new titles must default to source chapters with five-minute fallback");
        Title rate_defaults;req(rate_defaults.frame_rate=="inherit"&&rate_defaults.uhd_frame_rate=="inherit"&&rate_defaults.dvd_frame_rate=="inherit","new titles must inherit the project frame-rate default on every target");
        req(rate_defaults.resolution=="auto"&&rate_defaults.uhd_resolution=="auto"&&rate_defaults.dvd_resolution=="auto","new titles must default to closest-source resolution selection on every target");
        req(rate_defaults.aspect_ratio=="auto"&&rate_defaults.uhd_aspect_ratio=="auto"&&rate_defaults.dvd_aspect_ratio=="auto","new titles must default to source display aspect selection on every target");
        req(defaults.menu_resolution=="1920x1080"&&defaults.menu_uhd_resolution=="1920x1080"&&defaults.menu_dvd_resolution=="720x480","menus must default to 1080p Blu-ray/UHD and NTSC full-D1 DVD; UHD 1080p is the VLC mouse-navigation compatibility default");
        req(defaults.menu_aspect_ratio=="16:9"&&defaults.menu_uhd_aspect_ratio=="16:9"&&defaults.menu_dvd_aspect_ratio=="16:9","menu aspect defaults must be independent and widescreen");
        menu_resolution_for_target(defaults,DiscTarget::DvdVideo480p)="704x480";menu_aspect_ratio_for_target(defaults,DiscTarget::DvdVideo480p)="4:3";
        req(defaults.menu_resolution=="1920x1080"&&defaults.menu_uhd_resolution=="1920x1080"&&defaults.menu_dvd_resolution=="704x480","changing DVD menu resolution must not alter Blu-ray/UHD menu resolution");
        req(defaults.menu_aspect_ratio=="16:9"&&defaults.menu_uhd_aspect_ratio=="16:9"&&defaults.menu_dvd_aspect_ratio=="4:3","changing DVD menu aspect must not alter Blu-ray/UHD menu aspect");
        req(defaults.menu.encoding.keyframe_interval_frames==0&&rate_defaults.encoding.keyframe_interval_frames==0,"zero keyframe interval must preserve historical GOP defaults");
        req(maximum_keyframe_interval_for_rate(DiscTarget::BluRay1080,VideoCodec::X264,24.0)==25,"24p AVC legal GOP maximum must be 25 frames");
        req(maximum_keyframe_interval_for_rate(DiscTarget::BluRay1080,VideoCodec::X264,25.0)==26,"25p AVC legal GOP maximum regression");
        req(maximum_keyframe_interval_for_rate(DiscTarget::BluRay1080,VideoCodec::Mpeg2,24000.0/1001.0)==24,"23.976p MPEG-2 legal GOP maximum regression");
        req(maximum_keyframe_interval_for_rate(DiscTarget::DvdVideo480p,VideoCodec::Mpeg2,25.0,true)==15,"PAL DVD legal GOP maximum regression");
        req(maximum_keyframe_interval_for_rate(DiscTarget::DvdVideo480p,VideoCodec::Mpeg2,30000.0/1001.0,false)==18,"NTSC DVD legal GOP maximum regression");
        frame_rate_for_target(rate_defaults,DiscTarget::BluRay1080)="24p";frame_rate_for_target(rate_defaults,DiscTarget::UltraHdBluRay2160)="50p";frame_rate_for_target(rate_defaults,DiscTarget::DvdVideo480p)="film-dvd";
        req(frame_rate_for_target(rate_defaults,DiscTarget::BluRay1080)=="24p"&&frame_rate_for_target(rate_defaults,DiscTarget::UltraHdBluRay2160)=="50p"&&frame_rate_for_target(rate_defaults,DiscTarget::DvdVideo480p)=="film-dvd","per-target title frame-rate storage regression");
        req(defaults.menu.uhd_encoding.video_codec==VideoCodec::Hevc&&defaults.menu.uhd_encoding.video_bitrate_kbps==60000,"default UHD HEVC profile regression");
        req(defaults.menu.encoding.x264_preset == "medium", "default x264 preset regression");
        req(!defaults.menu.encoding.two_pass, "single-pass must be the default");
        EncodingProfile advanced_test;advanced_test.x264_advanced_options="aq-mode=2\npsy-rd=1.0:0.15";
        const auto parsed_advanced=parse_advanced_codec_options(advanced_test.x264_advanced_options);
        req(parsed_advanced.size()==2U&&parsed_advanced[0].name=="aq-mode"&&parsed_advanced[0].value=="2","advanced codec option parser regression");
        validate_advanced_codec_options(advanced_test,DiscTarget::BluRay1080);
        bool advanced_rejected=false;advanced_test.x264_advanced_options="keyint=50";try{validate_advanced_video_options(advanced_test,DiscTarget::BluRay1080);}catch(const std::exception&){advanced_rejected=true;}
        req(advanced_rejected,"advanced options must not override spec-critical GOP controls");
        advanced_rejected=false;advanced_test.x264_advanced_options="bframes=4";try{validate_advanced_video_options(advanced_test,DiscTarget::BluRay1080);}catch(const std::exception&){advanced_rejected=true;}
        req(advanced_rejected,"Blu-ray x264 advanced B-frame limit regression");
        advanced_rejected=false;advanced_test.x264_advanced_options="fake-interlaced=1";try{validate_advanced_video_options(advanced_test,DiscTarget::BluRay1080);}catch(const std::exception&){advanced_rejected=true;}
        req(advanced_rejected,"fake-interlaced signaling must remain controlled by the selected Blu-ray timing mode");
        advanced_test.x264_advanced_options.clear();advanced_test.ac3_advanced_options="drc_scale=0.8";validate_advanced_audio_options(advanced_test,DiscTarget::BluRay1080);
        advanced_test.ac3_advanced_options="ar=44100";advanced_rejected=false;try{validate_advanced_audio_options(advanced_test,DiscTarget::BluRay1080);}catch(const std::exception&){advanced_rejected=true;}
        req(advanced_rejected,"advanced audio options must not override disc sample rate");
        Title default_title;
        req(default_title.encoding.audio_codec == AudioCodec::Ac3, "AC-3 must be the default title audio mode");
        req(audio_codec_has_configurable_bitrate(AudioCodec::Ac3), "AC-3 bitrate must be configurable");
        req(audio_codec_has_configurable_bitrate(AudioCodec::Dca), "DTS bitrate must be configurable");
        req(audio_codec_has_configurable_bitrate(AudioCodec::TrueHdAc3), "TrueHD AC-3 core bitrate must be configurable");
        req(!audio_codec_has_configurable_bitrate(AudioCodec::Lpcm), "LPCM must not expose a target bitrate");
        EncodingProfile bitrate_profile;bitrate_profile.audio_codec=AudioCodec::Dca;set_audio_bitrate_kbps(bitrate_profile,768);req(bitrate_profile.dca_bitrate_kbps==768,"DTS bitrate setter regression");bitrate_profile.audio_codec=AudioCodec::TrueHdAc3;set_audio_bitrate_kbps(bitrate_profile,448);req(bitrate_profile.ac3_bitrate_kbps==448,"TrueHD AC-3 core bitrate setter regression");

        NewProjectDefaults new_defaults;new_defaults.volume_label="DEFAULTDISC";new_defaults.disc_capacity_bytes=50000000000ULL;new_defaults.disc_uhd_capacity_bytes=25000000000ULL;new_defaults.disc_dvd_capacity_bytes=2660000000ULL;new_defaults.use_encode_cache=false;new_defaults.force_reencode_new_titles=true;new_defaults.title_encoding.video_bitrate_kbps=18000;new_defaults.title_encoding.audio_codec=AudioCodec::Dca;new_defaults.title_encoding.dca_bitrate_kbps=768;new_defaults.menu_encoding.video_codec=VideoCodec::Mpeg2;new_defaults.menu_encoding.video_bitrate_kbps=12000;new_defaults.menu_duration_seconds=45.0;new_defaults.menu_resolution="1280x720";new_defaults.menu_uhd_resolution="3840x2160";new_defaults.menu_dvd_resolution="720x576";new_defaults.button_style.font_size_px=36;new_defaults.button_width=640;new_defaults.button_height=72;new_defaults.label_font_size_px=50;new_defaults.subtitle_style.override_style=true;new_defaults.subtitle_style.font_size_px=34;new_defaults.subtitle_style.bottom_offset_px=30;
        Project from_defaults;apply_new_project_defaults(from_defaults,new_defaults);req(from_defaults.volume_label=="DEFAULTDISC","new-project volume default regression");req(from_defaults.output_image.empty(),"new-project output path must remain empty until explicitly selected");req(!from_defaults.use_encode_cache,"new-project cache default regression");req(from_defaults.disc_capacity_bytes==50000000000ULL,"new-project Blu-ray size default regression");new_defaults.target=DiscTarget::UltraHdBluRay2160;apply_new_project_defaults(from_defaults,new_defaults);req(from_defaults.disc_capacity_bytes==25000000000ULL,"new-project UHD size default regression");new_defaults.target=DiscTarget::DvdVideo480p;apply_new_project_defaults(from_defaults,new_defaults);req(from_defaults.disc_capacity_bytes==2660000000ULL,"new-project DVD size default regression");new_defaults.target=DiscTarget::BluRay1080;apply_new_project_defaults(from_defaults,new_defaults);req(from_defaults.menu.encoding.video_codec==VideoCodec::Mpeg2&&from_defaults.menu.encoding.video_bitrate_kbps==12000,"new-project menu encoding defaults regression");req(from_defaults.menu.duration_seconds==45.0,"new-project menu duration default regression");req(from_defaults.menu_resolution=="1280x720"&&from_defaults.menu_uhd_resolution=="3840x2160"&&from_defaults.menu_dvd_resolution=="720x576","new-project per-target menu-resolution defaults regression");req(from_defaults.menu.default_button_style.font_size_px==36,"new-project button-style default regression");const auto title_from_defaults=make_title_from_defaults(new_defaults);req(title_from_defaults.encoding.video_bitrate_kbps==18000&&title_from_defaults.encoding.audio_codec==AudioCodec::Dca&&title_from_defaults.encoding.dca_bitrate_kbps==768,"new-title encoding defaults regression");req(title_from_defaults.force_reencode,"new-title force-reencode default regression");req(title_from_defaults.subtitle_default_style.override_style&&title_from_defaults.subtitle_default_style.font_size_px==34&&title_from_defaults.subtitle_default_style.bottom_offset_px==30,"new-title subtitle defaults regression");

        VideoStreamInfo avc;
        avc.codec_name="h264";avc.profile="High";avc.pixel_format="yuv420p";avc.field_order="progressive";avc.sample_aspect_ratio="1:1";
        avc.width=1920;avc.height=1080;avc.frame_rate=24000.0/1001.0;avc.sampled_average_bitrate_bps=35'000'000.0;avc.first_packet_is_keyframe=true;avc.max_keyframe_interval_seconds=1.0;avc.max_consecutive_b_frames=3;
        avc.h264_profile_idc=100;avc.h264_level_idc=41;avc.h264_chroma_format_idc=1;avc.h264_bit_depth_luma_minus8=0;avc.h264_bit_depth_chroma_minus8=0;avc.h264_max_num_ref_frames=4;avc.h264_hrd_present=true;avc.h264_buffering_period_sei_present=true;avc.h264_picture_timing_sei_present=true;avc.h264_initial_cpb_removal_delay_90k=60'750.0;avc.h264_hrd_max_bitrate_bps=40'000'000.0;avc.h264_hrd_max_cpb_bits=30'000'000.0;
        req(evaluate_blu_ray_video_headers(avc).compliant,"Blu-ray H.264 cheap header compliance regression");
        req(evaluate_blu_ray_video(avc).compliant,"Blu-ray H.264 compliance regression");
        auto bursty_avc=avc;bursty_avc.sampled_average_bitrate_bps=45'000'000.0;req(evaluate_blu_ray_video(bursty_avc).compliant,"H.264 DTS/PTS packet burst must not override compliant HRD bitrate");
        auto missing_sei_avc=avc;missing_sei_avc.h264_buffering_period_sei_present=false;missing_sei_avc.h264_picture_timing_sei_present=false;missing_sei_avc.h264_initial_cpb_removal_delay_90k=0.0;req(evaluate_blu_ray_video_headers(missing_sei_avc).compliant,"Blu-ray H.264 without native timing SEI should remain passthrough-capable because bundled tsMuxer synthesizes corrected HRD timing");
        auto excessive_delay_avc=avc;excessive_delay_avc.h264_initial_cpb_removal_delay_90k=855'000.0;req(!evaluate_blu_ray_video_headers(excessive_delay_avc).compliant,"Blu-ray H.264 passthrough must reject tsMuxer-style multi-second initial CPB delay");
        auto main_avc=avc;main_avc.profile="Main";main_avc.field_order="unknown";main_avc.h264_profile_idc=77;main_avc.h264_chroma_format_idc=-1;main_avc.h264_bit_depth_luma_minus8=-1;main_avc.h264_bit_depth_chroma_minus8=-1;main_avc.h264_frame_mbs_only_flag=1;
        req(evaluate_blu_ray_video_headers(main_avc).compliant,"Blu-ray H.264 Main implicit 8-bit 4:2:0/progressive SPS regression");
        req(evaluate_blu_ray_video(main_avc).compliant,"Blu-ray H.264 Main stream must not be falsely re-encoded");
        auto fake25_avc=avc;fake25_avc.frame_rate=25.0;fake25_avc.h264_frame_mbs_only_flag=0;fake25_avc.h264_mb_adaptive_frame_field_flag=0;
        req(evaluate_blu_ray_video_headers(fake25_avc).compliant,"Blu-ray x264 fake-interlaced 1080p25 header regression");
        auto fake2997_avc=fake25_avc;fake2997_avc.frame_rate=30000.0/1001.0;
        req(evaluate_blu_ray_video_headers(fake2997_avc).compliant,"Blu-ray x264 fake-interlaced 1080p29.97 header regression");
        auto fake25_mbaff=fake25_avc;fake25_mbaff.h264_mb_adaptive_frame_field_flag=1;fake25_mbaff.field_order="progressive";
        req(!evaluate_blu_ray_video_headers(fake25_mbaff).compliant,"progressive 1080p25 without x264 fake-interlaced SPS signaling must remain illegal");
        auto bad_avc=avc;bad_avc.h264_max_num_ref_frames=5;req(!evaluate_blu_ray_video_headers(bad_avc).compliant,"H.264 header gate must reject DPB overflow before sampling");req(!evaluate_blu_ray_video(bad_avc).compliant,"1080p H.264 DPB overflow must require re-encode");
        bad_avc=avc;bad_avc.h264_hrd_max_cpb_bits=31'000'000.0;req(!evaluate_blu_ray_video(bad_avc).compliant,"H.264 CPB overflow must require re-encode");
        bad_avc=avc;bad_avc.max_keyframe_interval_seconds=1.25;req(!evaluate_blu_ray_video(bad_avc).compliant,"H.264 long random-access interval must require re-encode");
        bad_avc=avc;bad_avc.sample_aspect_ratio="4:3";req(!evaluate_blu_ray_video(bad_avc).compliant,"H.264 invalid HD sample aspect ratio must require re-encode");

        VideoStreamInfo hevc;
        hevc.codec_name="hevc";hevc.profile="Main 10";hevc.pixel_format="yuv420p10le";hevc.field_order="progressive";hevc.sample_aspect_ratio="1:1";
        hevc.width=3840;hevc.height=2160;hevc.frame_rate=24000.0/1001.0;hevc.sampled_average_bitrate_bps=80'000'000.0;hevc.first_packet_is_keyframe=true;hevc.max_keyframe_interval_seconds=1.0;
        hevc.hevc_profile_idc=2;hevc.hevc_tier_flag=1;hevc.hevc_level_idc=153;hevc.hevc_chroma_format_idc=1;hevc.hevc_bit_depth_luma_minus8=2;hevc.hevc_bit_depth_chroma_minus8=2;hevc.hevc_hrd_present=true;hevc.hevc_hrd_max_bitrate_bps=100'000'000.0;hevc.hevc_hrd_max_cpb_bits=100'000'000.0;
        req(evaluate_blu_ray_video_headers(hevc,DiscTarget::UltraHdBluRay2160).compliant,"Ultra HD Blu-ray HEVC cheap header compliance regression");
        auto hevc60=hevc;hevc60.frame_rate=60.0;req(evaluate_blu_ray_video_headers(hevc60,DiscTarget::UltraHdBluRay2160).compliant,"Ultra HD Blu-ray 2160p60 must be accepted");
        auto hevc2997=hevc;hevc2997.frame_rate=30000.0/1001.0;req(!evaluate_blu_ray_video_headers(hevc2997,DiscTarget::UltraHdBluRay2160).compliant,"Ultra HD Blu-ray 2160p29.97 is not a ROM4 primary-video mode");
        req(evaluate_blu_ray_video(hevc,DiscTarget::UltraHdBluRay2160).compliant,"Ultra HD Blu-ray HEVC compliance regression");
        auto hevc1080=hevc;hevc1080.width=1920;hevc1080.height=1080;req(evaluate_blu_ray_video_headers(hevc1080,DiscTarget::UltraHdBluRay2160).compliant,"Ultra HD Blu-ray v3 must accept compliant 1920x1080 HEVC primary video");
        req(!evaluate_blu_ray_video(hevc,DiscTarget::BluRay1080).compliant,"HEVC UHD source must not pass standard Blu-ray compliance unchanged");
        req(evaluate_blu_ray_video_headers(avc,DiscTarget::UltraHdBluRay2160).compliant,"Ultra HD Blu-ray v3 must accept compliant AVC 1920x1080p23.976 primary video");
        req(evaluate_blu_ray_video(avc,DiscTarget::UltraHdBluRay2160).compliant,"Ultra HD Blu-ray v3 AVC sampled compliance regression");
        auto avc24=avc;avc24.frame_rate=24.0;req(evaluate_blu_ray_video_headers(avc24,DiscTarget::UltraHdBluRay2160).compliant,"Ultra HD Blu-ray v3 AVC 1080p24 must be accepted");
        auto avc25=avc;avc25.frame_rate=25.0;req(!evaluate_blu_ray_video_headers(avc25,DiscTarget::UltraHdBluRay2160).compliant,"Ultra HD Blu-ray v3 AVC 1080p25 is not legal");
        auto avc4k=avc;avc4k.width=3840;avc4k.height=2160;req(!evaluate_blu_ray_video_headers(avc4k,DiscTarget::UltraHdBluRay2160).compliant,"Ultra HD Blu-ray v3 AVC must be limited to 1920x1080");
        auto uhd_avc2020=avc;uhd_avc2020.color_primaries="bt2020";uhd_avc2020.color_transfer="bt2020-10";uhd_avc2020.color_space="bt2020nc";uhd_avc2020.color_range="tv";req(!evaluate_blu_ray_video_headers(uhd_avc2020,DiscTarget::UltraHdBluRay2160).compliant,"Ultra HD Blu-ray v3 AVC primary video must remain SDR BT.709");
        auto bad_hevc=hevc;bad_hevc.hevc_tier_flag=0;req(!evaluate_blu_ray_video_headers(bad_hevc,DiscTarget::UltraHdBluRay2160).compliant,"UHD HEVC Main Tier must be rejected");
        bad_hevc=hevc;bad_hevc.hevc_hrd_max_cpb_bits=100'100'001.0;req(!evaluate_blu_ray_video_headers(bad_hevc,DiscTarget::UltraHdBluRay2160).compliant,"UHD HEVC CPB above 100 Mbit must be rejected");
        auto pq_hevc=hevc;pq_hevc.color_primaries="bt2020";pq_hevc.color_transfer="smpte2084";pq_hevc.color_space="bt2020nc";pq_hevc.color_range="tv";req(evaluate_blu_ray_video_headers(pq_hevc,DiscTarget::UltraHdBluRay2160).compliant,"UHD BT.2020/ST2084 HDR color-description regression");
        auto sdr2020_hevc=hevc;sdr2020_hevc.color_primaries="bt2020";sdr2020_hevc.color_transfer="bt2020-10";sdr2020_hevc.color_space="bt2020nc";sdr2020_hevc.color_range="tv";req(evaluate_blu_ray_video_headers(sdr2020_hevc,DiscTarget::UltraHdBluRay2160).compliant,"UHD BT.2020 SDR color-description regression");
        auto partial709_hevc=hevc;partial709_hevc.color_primaries="bt709";partial709_hevc.color_transfer="unknown";partial709_hevc.color_space="unknown";partial709_hevc.color_range="tv";req(evaluate_blu_ray_video_headers(partial709_hevc,DiscTarget::UltraHdBluRay2160).compliant,"partially tagged UHD BT.709 source must use BT.709 defaults instead of being rejected");
        auto partial709_avc=avc;partial709_avc.color_primaries="bt709";partial709_avc.color_transfer="unknown";partial709_avc.color_space="unknown";partial709_avc.color_range="tv";req(evaluate_blu_ray_video_headers(partial709_avc,DiscTarget::BluRay1080).compliant,"partially tagged HD BT.709 source must use BT.709 defaults instead of being rejected");
        auto bad_color_hevc=hevc;bad_color_hevc.color_primaries="bt2020";bad_color_hevc.color_transfer="bt709";bad_color_hevc.color_space="bt2020nc";req(!evaluate_blu_ray_video_headers(bad_color_hevc,DiscTarget::UltraHdBluRay2160).compliant,"invalid UHD mixed color tags must be rejected");
        auto avc2020=avc;avc2020.color_primaries="bt2020";avc2020.color_transfer="smpte2084";avc2020.color_space="bt2020nc";req(!evaluate_blu_ray_video_headers(avc2020,DiscTarget::BluRay1080).compliant,"BT.2020/HDR source must be converted rather than passed through to 1080p Blu-ray");

        VideoStreamInfo mpeg2;
        mpeg2.codec_name="mpeg2video";mpeg2.profile="Main";mpeg2.pixel_format="yuv420p";mpeg2.field_order="tt";mpeg2.sample_aspect_ratio="1:1";
        mpeg2.width=1920;mpeg2.height=1080;mpeg2.frame_rate=30000.0/1001.0;mpeg2.sampled_average_bitrate_bps=30'000'000.0;mpeg2.first_packet_is_keyframe=true;mpeg2.max_keyframe_interval_seconds=0.5;
        mpeg2.mpeg2_profile_and_level_indication=0x44;mpeg2.mpeg2_header_bitrate_bps=40'000'000.0;mpeg2.mpeg2_vbv_bits=9'781'248.0;
        req(evaluate_blu_ray_video(mpeg2).compliant,"Blu-ray MPEG-2 compliance regression");
        auto bursty_mpeg2=mpeg2;bursty_mpeg2.sampled_average_bitrate_bps=45'000'000.0;req(evaluate_blu_ray_video(bursty_mpeg2).compliant,"MPEG-2 DTS/PTS packet burst must not override compliant sequence-header bitrate");
        auto bad_mpeg2=mpeg2;bad_mpeg2.mpeg2_vbv_bits=10'000'000.0;req(!evaluate_blu_ray_video(bad_mpeg2).compliant,"MPEG-2 VBV overflow must require re-encode");

        VideoStreamInfo vc1;
        vc1.codec_name="vc1";vc1.profile="Advanced";vc1.level=3;vc1.pixel_format="yuv420p";vc1.field_order="progressive";vc1.sample_aspect_ratio="1:1";vc1.color_primaries="bt709";vc1.color_transfer="bt709";vc1.color_space="bt709";vc1.color_range="tv";
        vc1.width=1280;vc1.height=720;vc1.frame_rate=60000.0/1001.0;vc1.sampled_average_bitrate_bps=25'000'000.0;vc1.first_packet_is_keyframe=true;vc1.max_keyframe_interval_seconds=1.0;
        req(evaluate_blu_ray_video(vc1).compliant,"Blu-ray VC-1 compliance regression");
        auto bad_vc1_rate=vc1;bad_vc1_rate.sampled_average_bitrate_bps=41'000'000.0;req(!evaluate_blu_ray_video(bad_vc1_rate).compliant,"VC-1 sampled-average bitrate sanity check regression");

        AudioStreamInfo ac3_info;ac3_info.present=true;ac3_info.codec_name="ac3";ac3_info.sample_rate=48000;ac3_info.channels=6;ac3_info.bitrate_bps=640000;
        req(evaluate_blu_ray_audio(ac3_info).compliant,"Blu-ray AC-3 compliance regression");
        auto bad_ac3=ac3_info;bad_ac3.sample_rate=44100;req(!evaluate_blu_ray_audio(bad_ac3).compliant,"44.1 kHz AC-3 must require re-encode");
        AudioStreamInfo eac3;eac3.present=true;eac3.codec_name="eac3";eac3.sample_rate=48000;eac3.channels=8;eac3.bitrate_bps=4'000'000;
        req(evaluate_blu_ray_audio(eac3).compliant,"Blu-ray Dolby Digital Plus compliance regression");
        const std::vector<std::uint8_t> dts_1509_header{0x7f,0xfe,0x80,0x01,0xfc,0x3c,0x7d,0xb0,0x37,0x00,0x01,0x38};
        const auto dts_1509_actual=detail::dts_core_actual_bitrate_from_bytes(dts_1509_header);
        req(dts_1509_actual&&std::abs(*dts_1509_actual-1'509'000.0)<1.0,"DTS core actual bitrate must come from FSIZE/NBLKS/SFREQ rather than nominal RATE");
        const std::vector<std::uint8_t> dts_150975_header{0x7f,0xfe,0x80,0x01,0xfc,0x3c,0x7d,0xc0,0x37,0x00,0x01,0x38};
        const auto dts_150975_actual=detail::dts_core_actual_bitrate_from_bytes(dts_150975_header);
        req(dts_150975_actual&&std::abs(*dts_150975_actual-1'509'750.0)<1.0,"DTS 2013-byte core frame must derive exact 1509.75 kb/s payload rate");
        const std::vector<std::uint8_t> dts_1536_header{0x7f,0xfe,0x80,0x01,0xfc,0x3c,0x7f,0xf0,0x37,0x00,0x01,0x38};
        const auto dts_1536_actual=detail::dts_core_actual_bitrate_from_bytes(dts_1536_header);
        req(dts_1536_actual&&std::abs(*dts_1536_actual-1'536'000.0)<1.0,"DTS full-rate core parser regression");
        AudioStreamInfo dts;dts.present=true;dts.codec_name="dts";dts.profile="DTS";dts.sample_rate=48000;dts.channels=6;dts.bitrate_bps=1'509'750;dts.reported_bitrate_bps=1'536'000;dts.dts_core_bitrate_bps=1'509'750;
        req(evaluate_blu_ray_audio(dts).compliant,"Blu-ray DTS core compliance regression");
        auto bad_dts=dts;bad_dts.dts_core_bitrate_bps=1'600'000;bad_dts.bitrate_bps=1'600'000;req(!evaluate_blu_ray_audio(bad_dts).compliant,"DTS core frame-header rate above Blu-ray maximum must require re-encode");
        AudioStreamInfo dtshd;dtshd.present=true;dtshd.codec_name="dts";dtshd.profile="DTS-HD MA";dtshd.sample_rate=48000;dtshd.channels=8;dtshd.bitrate_bps=20'000'000;dtshd.reported_bitrate_bps=40'000'000;dtshd.dts_core_bitrate_bps=1'509'750;
        req(evaluate_blu_ray_audio(dtshd).compliant,"Blu-ray DTS-HD must use measured elementary bitrate rather than an unrelated stream-level estimate");
        AudioStreamInfo pcm;pcm.present=true;pcm.codec_name="pcm_s24le";pcm.sample_rate=96000;pcm.channels=8;pcm.bits_per_sample=24;
        req(evaluate_blu_ray_audio(pcm).compliant,"Blu-ray LPCM compliance regression");

        const auto progress_tmp = std::filesystem::temp_directory_path() / "bdmvauthor-test-ffmpeg-progress.txt";
        {
            std::ofstream f(progress_tmp);
            f << "frame=12\nout_time_us=500000\nprogress=continue\n"
              << "frame=48\nout_time_us=2000000\nprogress=continue\n";
        }
        const auto progress_seconds = detail::read_ffmpeg_progress_seconds(progress_tmp);
        req(progress_seconds && *progress_seconds == 2.0, "FFmpeg progress parser must use newest out_time_us");
        req(detail::progress_fraction(2.0, 8.0) == 0.25, "FFmpeg progress fraction regression");
        req(detail::progress_fraction(20.0, 8.0) == 1.0, "FFmpeg progress fraction must clamp to 100 percent");
        std::filesystem::remove(progress_tmp);
        req(defaults.menu.default_button_style.selected.text_color == Rgba{128,200,255,255},
            "selected text buttons must default to light blue");

        const auto j = hdmv::jump_title(1);
        req(j.opcode == 0x21810000U, "Jump_Title opcode regression");
        const auto pp = hdmv::play_pl(7);
        req(pp.opcode == 0x22800000U && pp.operand1 == 7, "Play_PL opcode regression");
        const auto ppm = hdmv::play_pl_pm(7,2);
        req(ppm.opcode == 0x42c20000U && ppm.operand1 == 7 && ppm.operand2 == 2, "Play_PL_PM chapter playmark opcode regression");

        MenuButton audio_select;audio_select.target_kind=MenuButtonTargetKind::AudioTrack;audio_select.target_title=1;audio_select.target_stream=2;
        const auto audio_commands=hdmv::make_button_navigation_commands(audio_select);
        req(audio_commands.size()==7, "audio selection must preserve subtitle display state with conditional SetStream commands");
        req(audio_commands[0].opcode==0x50000001U && audio_commands[0].operand1==4095 && audio_commands[0].operand2==0x80000002U, "audio selection must read subtitle PSR2");
        req(audio_commands[1].opcode==0x50400009U && audio_commands[1].operand2==0x80000000U, "audio selection must isolate subtitle display bit");
        req(audio_commands[2].opcode==0x48400200U && audio_commands[2].operand2==0, "audio selection subtitle-state test regression");
        req(audio_commands[3].opcode==0x20810000U && audio_commands[3].operand1==6, "audio selection conditional branch regression");
        req(audio_commands[4].opcode==0x51c00001U && audio_commands[4].operand1==0x80024000U, "audio selection with subtitles-on SetStream regression");
        req(audio_commands[5].opcode==0x20810000U && audio_commands[5].operand1==7, "audio selection end branch regression");
        req(audio_commands[6].opcode==0x51c00001U && audio_commands[6].operand1==0x80020000U, "audio selection with subtitles-off SetStream regression");
        MenuButton subtitle_select;subtitle_select.target_kind=MenuButtonTargetKind::SubtitleTrack;subtitle_select.target_title=1;subtitle_select.target_stream=3;
        const auto subtitle_commands=hdmv::make_button_navigation_commands(subtitle_select);
        req(subtitle_commands.size()==1 && subtitle_commands[0].opcode==0x51c00001U && subtitle_commands[0].operand1==0x0000c003U, "subtitle selection SetStream regression");
        MenuButton subtitle_off;subtitle_off.target_kind=MenuButtonTargetKind::SubtitleOff;subtitle_off.target_title=1;
        const auto subtitle_off_commands=hdmv::make_button_navigation_commands(subtitle_off);
        req(subtitle_off_commands.size()==1 && subtitle_off_commands[0].opcode==0x51c00001U && subtitle_off_commands[0].operand1==0, "subtitle-off SetStream regression");

        hdmv::MenuObjectMap action_menus{{"top",0},{"extras",1}};
        NavigationAction play_trailer;play_trailer.kind=NavigationActionKind::PlayTitle;play_trailer.target_title=2;
        NavigationAction select_audio;select_audio.kind=NavigationActionKind::AudioTrack;select_audio.target_title=2;select_audio.target_stream=1;
        NavigationAction play_main;play_main.kind=NavigationActionKind::PlayTitle;play_main.target_title=1;
        NavigationAction return_extras;return_extras.kind=NavigationActionKind::Menu;return_extras.target_menu_id="extras";
        const std::vector<NavigationAction> sequence{play_trailer,select_audio,play_main,return_extras};
        const auto sequence_commands=hdmv::make_navigation_sequence_commands(sequence,action_menus,3,0);
        req(std::any_of(sequence_commands.begin(),sequence_commands.end(),[](const auto& c){return c.opcode==hdmv::play_pl(4).opcode&&c.operand1==4;}),"first PlayTitle must map to the title playlist after menu playlists");
        req(std::any_of(sequence_commands.begin(),sequence_commands.end(),[](const auto& c){return c.opcode==0x50400001U&&c.operand1==hdmv::title_audio_state_gpr(2)&&c.operand2==1;}),"audio action must update title 2's independent audio-state GPR");
        req(std::any_of(sequence_commands.begin(),sequence_commands.end(),[](const auto& c){return c.opcode==hdmv::play_pl(3).opcode&&c.operand1==3;}),"second PlayTitle playlist regression");
        req(sequence_commands.back().opcode==hdmv::jump_object(1).opcode&&sequence_commands.back().operand1==1,"final action must jump to requested return menu");
        const std::vector<NavigationAction> no_explicit_return{play_main};
        const auto implicit_return=hdmv::make_navigation_sequence_commands(no_explicit_return,action_menus,3,1);
        req(implicit_return.size()==4&&implicit_return[0].operand1==4091&&implicit_return[0].operand2==1&&implicit_return[1].operand1==4094&&implicit_return[2].operand1==4093&&implicit_return[3].opcode==hdmv::jump_title(1).opcode,"submenu title sequence must enter the numbered title while preserving its return menu in GPR4091");
        const auto top_menu_title=hdmv::make_navigation_sequence_commands(no_explicit_return,action_menus,3,0);
        req(top_menu_title.size()==4&&top_menu_title[0].operand1==4091&&top_menu_title[0].operand2==0&&top_menu_title[1].operand1==4094&&top_menu_title[1].operand2==0&&top_menu_title[2].operand1==4093&&top_menu_title[2].operand2==0&&top_menu_title[3].opcode==hdmv::jump_title(1).opcode&&top_menu_title[3].operand1==1,"terminal top-menu PlayTitle must enter the numbered non-interactive title with cleared return/chapter/repeat state");
        const auto immediate_submenu_title=hdmv::make_immediate_navigation_sequence_commands(no_explicit_return,action_menus,1);
        req(immediate_submenu_title&&immediate_submenu_title->size()==4&&(*immediate_submenu_title)[0].operand1==4091&&(*immediate_submenu_title)[0].operand2==1&&(*immediate_submenu_title)[3].opcode==hdmv::jump_title(1).opcode,"ordinary submenu PlayTitle must compile directly into the IG button instead of a synthetic MovieObject");
        const std::vector<NavigationAction> stream_then_title{select_audio,play_main};
        const auto immediate_stream_then_title=hdmv::make_immediate_navigation_sequence_commands(stream_then_title,action_menus,1);
        req(immediate_stream_then_title&&immediate_stream_then_title->size()==5&&(*immediate_stream_then_title)[0].opcode==0x50400001U&&(*immediate_stream_then_title)[0].operand1==hdmv::title_audio_state_gpr(2)&&(*immediate_stream_then_title)[4].opcode==hdmv::jump_title(1).opcode,"stream selection followed by one terminal title must update per-title state and remain IG-button-immediate");
        const std::vector<NavigationAction> two_titles{play_trailer,play_main};
        req(!hdmv::make_immediate_navigation_sequence_commands(two_titles,action_menus,1),"multi-title sequence must retain a synthetic MovieObject because Play_PL must return and continue");
        const std::vector<NavigationAction> stream_only{select_audio};
        const auto immediate_stream_only=hdmv::make_immediate_navigation_sequence_commands(stream_only,action_menus,1);
        req(immediate_stream_only&&immediate_stream_only->size()==1&&(*immediate_stream_only)[0].operand1==hdmv::title_audio_state_gpr(2),"stream-only button must update only its target title state without restarting the menu playlist");

        req(hdmv::title_audio_state_gpr(1)==4090&&hdmv::title_subtitle_state_gpr(1)==4089&&hdmv::title_audio_state_gpr(2)==4088,"per-title stream-state GPR allocation regression");
        req(hdmv::menu_selection_state_gpr(0)==0&&hdmv::menu_selection_state_gpr(7)==7&&hdmv::menu_selection_state_gpr(998)==998,"per-menu selection-state GPR allocation regression");
        const std::array<hdmv::TitleStreamDefaults,2> stream_defaults_test{{{2,0},{1,3}}};
        const auto init_streams=hdmv::make_title_stream_initialization_commands(stream_defaults_test);
        req(init_streams.size()==4&&init_streams[0].operand1==4090&&init_streams[0].operand2==2&&init_streams[1].operand1==4089&&init_streams[1].operand2==0&&init_streams[2].operand1==4088&&init_streams[2].operand2==1&&init_streams[3].operand1==4087&&init_streams[3].operand2==3,"per-title default stream initialization regression");
        Menu indicator_menu;indicator_menu.width=1920;indicator_menu.height=1080;indicator_menu.duration_seconds=30;
        auto add_indicator_button=[&](NavigationAction action,int x){MenuButton b;b.label="state";b.bounds={x,100,180,60};set_button_action_sequence(b,{action});indicator_menu.buttons.push_back(std::move(b));};
        NavigationAction t1a1=select_audio;t1a1.target_title=1;t1a1.target_stream=1;add_indicator_button(t1a1,100);
        NavigationAction t1a2=t1a1;t1a2.target_stream=2;add_indicator_button(t1a2,300);
        NavigationAction t2a1=t1a1;t2a1.target_title=2;add_indicator_button(t2a1,500);
        NavigationAction t1off;t1off.kind=NavigationActionKind::SubtitleOff;t1off.target_title=1;add_indicator_button(t1off,700);
        NavigationAction t1sub;t1sub.kind=NavigationActionKind::SubtitleTrack;t1sub.target_title=1;t1sub.target_stream=1;add_indicator_button(t1sub,900);
        const auto indicator_plan=hdmv::make_menu_stream_indicator_plan(indicator_menu);
        req(indicator_plan.indicator_bog_count==5&&indicator_plan.variants.size()==5&&indicator_plan.initializer_button_id==11,"active-stream indicators must use one hardware-safe BOG per visual variant without cross-product state combinations");
        const auto indicator_id=[&](hdmv::StreamIndicatorKind kind,std::uint16_t title,std::uint16_t stream){for(const auto& v:indicator_plan.variants)if(v.kind==kind&&v.target_title==title&&v.target_stream==stream)return v.button_id;throw std::runtime_error("missing expected active-stream indicator variant");};
        const auto t1a1_id=indicator_id(hdmv::StreamIndicatorKind::Audio,1,1),t1a2_id=indicator_id(hdmv::StreamIndicatorKind::Audio,1,2);
        const auto t1off_id=indicator_id(hdmv::StreamIndicatorKind::Subtitle,1,0),t1sub_id=indicator_id(hdmv::StreamIndicatorKind::Subtitle,1,1);
        const auto indicator_segs=hdmv::make_menu_display_set(indicator_menu);
        const auto parsed_bogs=parse_menu_bogs(indicator_segs.front().payload);
        auto find_bog=[&](unsigned id)->std::pair<std::size_t,ParsedIgButton>{for(std::size_t bi=0;bi<parsed_bogs.size();++bi)for(const auto& b:parsed_bogs[bi])if(b.id==id)return {bi,b};throw std::runtime_error("missing indicator button in authored BOG");};
        const auto [t1a1_bog,t1a1_button]=find_bog(t1a1_id);const auto [t1a2_bog,t1a2_button]=find_bog(t1a2_id);
        const auto [t1off_bog,t1off_button]=find_bog(t1off_id);const auto [t1sub_bog,t1sub_button]=find_bog(t1sub_id);
        req(t1a1_bog!=t1a2_bog&&t1a1_button.x!=t1a2_button.x,"title-1 audio active indicators must use separate tightly bounded BOGs so transparent pixels cannot cover sibling buttons");
        req(t1off_bog!=t1sub_bog&&t1off_button.x!=t1sub_button.x,"title-1 subtitle active indicators must use separate tightly bounded BOGs so transparent pixels cannot cover sibling buttons");
        req(t1a1_bog!=t1off_bog,"audio and subtitle active indicators must remain independent BOGs");
        auto variant_index=[&](hdmv::StreamIndicatorKind kind,std::uint16_t title,std::uint16_t stream){for(std::size_t i=0;i<indicator_plan.variants.size();++i){const auto& v=indicator_plan.variants[i];if(v.kind==kind&&v.target_title==title&&v.target_stream==stream)return i;}throw std::runtime_error("missing expected indicator variant index");};
        const auto ods_for_variant=[&](std::size_t vi)->const Bytes&{return indicator_segs[2U+indicator_menu.buttons.size()*3U+vi].payload;};
        const auto& t1a1_ods=ods_for_variant(variant_index(hdmv::StreamIndicatorKind::Audio,1,1));const auto& t1a2_ods=ods_for_variant(variant_index(hdmv::StreamIndicatorKind::Audio,1,2));
        const auto& t1off_ods=ods_for_variant(variant_index(hdmv::StreamIndicatorKind::Subtitle,1,0));const auto& t1sub_ods=ods_for_variant(variant_index(hdmv::StreamIndicatorKind::Subtitle,1,1));
        req(be16(t1a1_ods,7)<500&&be16(t1a2_ods,7)<500,"audio active-indicator objects must remain tightly bounded rather than spanning the whole choice group");
        req(be16(t1off_ods,7)<500&&be16(t1sub_ods,7)<500,"subtitle active-indicator objects must remain tightly bounded rather than spanning the whole choice group");
        const auto active_change=hdmv::make_immediate_navigation_sequence_commands(std::span<const NavigationAction>(&t1a2,1),action_menus,0,&indicator_menu);
        req(active_change&&active_change->size()==3&&(*active_change)[0].operand1==hdmv::title_audio_state_gpr(1)&&(*active_change)[0].operand2==2&&(*active_change)[1].opcode==0x31800005U&&(*active_change)[1].operand1==t1a1_id&&(*active_change)[2].opcode==0x31800004U&&(*active_change)[2].operand1==t1a2_id,"audio selection must explicitly disable the conflicting active indicator before enabling the new one");
        const auto subtitle_on=hdmv::make_immediate_navigation_sequence_commands(std::span<const NavigationAction>(&t1sub,1),action_menus,0,&indicator_menu);
        req(subtitle_on&&subtitle_on->size()==3&&(*subtitle_on)[1].opcode==0x31800005U&&(*subtitle_on)[1].operand1==t1off_id&&(*subtitle_on)[2].opcode==0x31800004U&&(*subtitle_on)[2].operand1==t1sub_id,"enabling subtitles must clear the Subtitles-off active indicator");
        const auto subtitle_off_change=hdmv::make_immediate_navigation_sequence_commands(std::span<const NavigationAction>(&t1off,1),action_menus,0,&indicator_menu);
        req(subtitle_off_change&&subtitle_off_change->size()==3&&(*subtitle_off_change)[1].opcode==0x31800005U&&(*subtitle_off_change)[1].operand1==t1sub_id&&(*subtitle_off_change)[2].opcode==0x31800004U&&(*subtitle_off_change)[2].operand1==t1off_id,"turning subtitles off must clear the previously active subtitle-track indicator");
        auto chapter_action=no_explicit_return;chapter_action[0].target_chapter=3;
        const auto top_menu_chapter=hdmv::make_navigation_sequence_commands(chapter_action,action_menus,3,0);
        req(top_menu_chapter.size()==4&&top_menu_chapter[1].opcode==0x50400001U&&top_menu_chapter[1].operand1==4094&&top_menu_chapter[1].operand2==3&&top_menu_chapter[2].operand1==4093&&top_menu_chapter[2].operand2==0&&top_menu_chapter[3].opcode==hdmv::jump_title(1).opcode,"terminal chapter link must carry chapter through GPR4094 and preserve Jump_Title context");
        auto repeat_title=no_explicit_return;repeat_title[0].repeat_count=3;
        const auto top_menu_repeat=hdmv::make_navigation_sequence_commands(repeat_title,action_menus,3,0);
        req(top_menu_repeat.size()==4&&top_menu_repeat[2].operand1==4093&&top_menu_repeat[2].operand2==2&&top_menu_repeat[3].opcode==hdmv::jump_title(1).opcode,"finite terminal title repeat must carry two additional plays through GPR4093 and preserve Jump_Title");
        repeat_title[0].repeat_count=0;const auto top_menu_forever=hdmv::make_navigation_sequence_commands(repeat_title,action_menus,3,0);
        req(top_menu_forever.size()==4&&top_menu_forever[2].operand1==4093&&top_menu_forever[2].operand2==0xffffffffU&&top_menu_forever[3].opcode==hdmv::jump_title(1).opcode,"infinite terminal title repeat must use the GPR4093 forever sentinel");
        repeat_title[0].repeat_count=3;const auto submenu_repeat=hdmv::make_navigation_sequence_commands(repeat_title,action_menus,3,1);
        req(submenu_repeat.size()==4&&submenu_repeat[0].operand1==4091&&submenu_repeat[0].operand2==1&&submenu_repeat[2].operand1==4093&&submenu_repeat[2].operand2==2&&submenu_repeat[3].opcode==hdmv::jump_title(1).opcode,"finite submenu PlayTitle repeat must preserve normal numbered-title playback and return-menu state");
        repeat_title[0].repeat_count=0;const auto submenu_forever=hdmv::make_navigation_sequence_commands(repeat_title,action_menus,3,1);
        req(submenu_forever.size()==4&&submenu_forever[0].operand1==4091&&submenu_forever[0].operand2==1&&submenu_forever[2].operand2==0xffffffffU&&submenu_forever[3].opcode==hdmv::jump_title(1).opcode,"infinite submenu PlayTitle repeat must loop through the numbered title MovieObject");
        NavigationAction group_begin;group_begin.kind=NavigationActionKind::RepeatBegin;group_begin.repeat_count=3;NavigationAction group_end;group_end.kind=NavigationActionKind::RepeatEnd;
        const std::vector<NavigationAction> finite_group{group_begin,play_trailer,play_main,group_end};
        const auto expanded_group=expand_navigation_repeat_groups(finite_group);
        req(!expanded_group.has_infinite_loop()&&expanded_group.actions.size()==6&&expanded_group.actions[0].target_title==2&&expanded_group.actions[1].target_title==1&&expanded_group.actions[4].target_title==2&&expanded_group.actions[5].target_title==1,"finite repeat group must flatten the complete action series in order");
        const auto finite_group_commands=hdmv::make_navigation_sequence_commands(finite_group,action_menus,3,1);
        req(std::count_if(finite_group_commands.begin(),finite_group_commands.end(),[](const auto& c){return c.opcode==hdmv::play_pl(4).opcode&&c.operand1==4;})==3&&std::count_if(finite_group_commands.begin(),finite_group_commands.end(),[](const auto& c){return c.opcode==hdmv::play_pl(3).opcode&&c.operand1==3;})==2&&finite_group_commands[finite_group_commands.size()-4].operand1==4091&&finite_group_commands[finite_group_commands.size()-4].operand2==1&&finite_group_commands.back().opcode==hdmv::jump_title(1).opcode,"finite HDMV repeat group must apply per-title stream state to intermediate playlists, enter its final numbered title, and return to the invoking submenu");
        group_begin.repeat_count=0;const std::vector<NavigationAction> forever_group{group_begin,play_trailer,play_main,group_end};
        const auto expanded_forever_group=expand_navigation_repeat_groups(forever_group);
        req(expanded_forever_group.has_infinite_loop()&&expanded_forever_group.infinite_loop_start==0&&expanded_forever_group.actions.size()==2,"forever repeat group must preserve one body and expose its loop start");
        const auto forever_group_commands=hdmv::make_navigation_sequence_commands(forever_group,action_menus,3,1);
        req(std::any_of(forever_group_commands.begin(),forever_group_commands.end(),[](const auto& c){return c.opcode==hdmv::play_pl(4).opcode&&c.operand1==4;})&&std::any_of(forever_group_commands.begin(),forever_group_commands.end(),[](const auto& c){return c.opcode==hdmv::play_pl(3).opcode&&c.operand1==3;})&&forever_group_commands.back().opcode==0x20810000U&&forever_group_commands.back().operand1==0,"forever HDMV repeat group must loop the whole ordered series after applying each title's stream state");
        bool nested_group_rejected=false;try{std::vector<NavigationAction> nested{group_begin,group_begin,play_main,group_end,group_end};(void)expand_navigation_repeat_groups(nested);}catch(const std::exception&){nested_group_rejected=true;}req(nested_group_rejected,"nested repeat groups must be rejected");
        const std::array<std::uint16_t,1> chapter_counts{{3}};const auto chapter_mobj=hdmv::make_movie_object(1,1,false,{},chapter_counts);
        const std::array<std::uint8_t,4> ppm_opcode{{0x42,0xc2,0x00,0x00}};req(std::search(chapter_mobj.begin(),chapter_mobj.end(),ppm_opcode.begin(),ppm_opcode.end())!=chapter_mobj.end(),"numbered title MovieObject must contain Play_PL_PM chapter dispatch");
        const std::array<std::uint8_t,4> sub_opcode{{0x50,0x40,0x00,0x04}};req(std::search(chapter_mobj.begin(),chapter_mobj.end(),sub_opcode.begin(),sub_opcode.end())!=chapter_mobj.end(),"numbered title MovieObject must decrement GPR4093 between finite repeats");

        const auto idx = hdmv::make_index(2);
        req(std::string(reinterpret_cast<const char*>(idx.data()), 8) == "INDX0200", "INDX header");
        const auto idx_v3=hdmv::make_index(2,1,true);req(std::string(reinterpret_cast<const char*>(idx_v3.data()),8)=="INDX0300","UHD INDX0300 header regression");
        const auto io = be32(idx, 8);
        req(io < idx.size(), "index offset");
        std::size_t q = io + 4;
        req(((idx[q] >> 6) & 3U) == 1U, "first play is not HDMV");
        q += 24;
        req(be16(idx, q) == 2, "title count");
        q += 2;
        for (int i = 0; i < 2; ++i) {
            req(((idx[q] >> 6) & 3U) == 1U, "title is not HDMV");
            q += 12;
        }

        const auto mo = hdmv::make_movie_object(2);
        req(std::string(reinterpret_cast<const char*>(mo.data()), 8) == "MOBJ0200", "MOBJ header");
        const auto mo_v3=hdmv::make_movie_object(2,1,true);req(std::string(reinterpret_cast<const char*>(mo_v3.data()),8)=="MOBJ0300","UHD MOBJ0300 header regression");
        req(be32(mo, 40) > 0, "MOBJ body length");
        req(be16(mo, 48) == 3, "MOBJ object count");

        const auto idx_nested = hdmv::make_index(2, 3);
        std::size_t nq = be32(idx_nested, 8) + 4 + 24;
        req(be16(idx_nested, nq) == 2, "nested-menu title count regression");
        nq += 2;
        req(be16(idx_nested, nq + 6) == 3, "title 1 MovieObject must follow three menu objects");
        req(be16(idx_nested, nq + 18) == 4, "title 2 MovieObject must follow three menu objects");
        const auto mo_nested = hdmv::make_movie_object(2, 3);
        const auto nested_objects = parse_movie_objects(mo_nested);
        req(nested_objects.size() == 5, "three menus plus two titles must create five MovieObjects");

        const auto& top_commands = nested_objects[0].commands;
        req(top_commands.size() == 17U, "top menu must contain title-return dispatcher plus submenu branches");
        req(top_commands[0].opcode == 0x48400200U && top_commands[0].operand1 == 2092U && top_commands[0].operand2 == 1U,
            "top menu must test the natural-title-return dispatch flag before normal entry");
        req(top_commands[1].opcode == 0x20810000U && top_commands[1].operand1 == 6U,
            "top menu natural-title-return branch target regression");
        req(top_commands[3].opcode == hdmv::play_pl(0).opcode && top_commands[3].operand1 == 0U,
            "top menu MovieObject playlist regression");
        req(top_commands[4].opcode == 0x50000001U && top_commands[4].operand1 == hdmv::menu_selection_state_gpr(0) && top_commands[4].operand2 == 0x8000000aU,
            "top menu MovieObject must copy PSR10 into its per-menu selection GPR after playback");
        req(top_commands[5].opcode == 0x20810000U && top_commands[5].operand1 == 3U,
            "top menu MovieObject must loop in-place with GoTo Play_PL rather than re-running entry dispatch");
        req(top_commands[6].opcode == 0x50400001U && top_commands[6].operand1 == 2092U && top_commands[6].operand2 == 0U,
            "top menu return dispatcher must clear its one-shot pending flag");
        req(top_commands[7].opcode == 0x48400200U && top_commands[7].operand1 == 4091U && top_commands[7].operand2 == 1U &&
            top_commands[9].opcode == 0x48400200U && top_commands[9].operand1 == 4091U && top_commands[9].operand2 == 2U,
            "top menu return dispatcher must inspect the remembered submenu");
        req(top_commands[14].opcode == hdmv::jump_object(1).opcode && top_commands[14].operand1 == 1U &&
            top_commands[16].opcode == hdmv::jump_object(2).opcode && top_commands[16].operand1 == 2U,
            "top menu return dispatcher must route to the remembered submenu after entering title 0");

        for (unsigned menu_object = 1; menu_object < 3; ++menu_object) {
            const auto& menu_commands = nested_objects[menu_object].commands;
            req(menu_commands.size() == 3U, "submenu MovieObject command-count regression");
            req(menu_commands[0].opcode == hdmv::play_pl(static_cast<std::uint16_t>(menu_object)).opcode && menu_commands[0].operand1 == menu_object,
                "submenu MovieObject playlist regression");
            req(menu_commands[1].opcode == 0x50000001U && menu_commands[1].operand1 == hdmv::menu_selection_state_gpr(static_cast<std::uint16_t>(menu_object)) && menu_commands[1].operand2 == 0x8000000aU,
                "submenu must save PSR10 into its own selection-state GPR");
            req(menu_commands[2].opcode == 0x20810000U && menu_commands[2].operand1 == 0U,
                "submenu MovieObject must loop in-place with GoTo Play_PL rather than Jump_Object");
        }

        // Natural numbered-title completion must re-enter interactive title 0
        // before the Top Menu MovieObject routes to the remembered submenu.
        // A direct Jump_Object here would leave player UI in numbered-title
        // timing context and expose elapsed time over the menu.
        const auto& first_title_commands = nested_objects[3].commands;
        req(first_title_commands.size() == 14U, "numbered title return command-count regression");
        req(first_title_commands[12].opcode == 0x50400001U && first_title_commands[12].operand1 == 2092U && first_title_commands[12].operand2 == 1U,
            "numbered title must arm the one-shot interactive-menu return dispatcher");
        req(first_title_commands[13].opcode == hdmv::jump_title(0).opcode && first_title_commands[13].operand1 == 0U,
            "numbered title must return through Jump_Title 0 so elapsed time is hidden in menus");
        hdmv::MovieObject extra_nav{false,false,false,sequence_commands};
        const std::vector<hdmv::MovieObject> extras{extra_nav};
        const auto mo_with_extra=hdmv::make_movie_object(2,3,false,extras);
        req(be16(mo_with_extra,48)==6,"synthetic navigation MovieObject must be appended after menus and titles");
        const auto idx_first_sequence=hdmv::make_index(2,3,false,5);
        const auto first_entry=static_cast<std::size_t>(be32(idx_first_sequence,8))+4U;
        req(be16(idx_first_sequence,first_entry+6U)==5,"First Playback index entry must be able to target a synthetic navigation object");

        const auto menuless_defaults=default_menuless_startup_actions(3);
        req(menuless_defaults.size()==3&&menuless_defaults[0].target_title==1&&menuless_defaults[1].target_title==2&&menuless_defaults[2].target_title==3,"menu-less default startup must play every title in order");
        req(is_default_menuless_startup_sequence(menuless_defaults,3),"menu-less default startup recognizer regression");
        auto menuless_custom=menuless_defaults;std::swap(menuless_custom[0],menuless_custom[1]);
        req(!is_default_menuless_startup_sequence(menuless_custom,3),"custom menu-less title order must not be mistaken for the default");
        const auto idx_menuless=hdmv::make_index(2,0,false,3);
        const auto menuless_first=static_cast<std::size_t>(be32(idx_menuless,8))+4U;
        req(be16(idx_menuless,menuless_first+6U)==3,"menu-less First Playback must target the synthetic startup object after terminal+title objects");
        const auto menuless_titles=menuless_first+24U+2U;
        req(be16(idx_menuless,menuless_titles+6U)==1&&be16(idx_menuless,menuless_titles+18U)==2,"menu-less numbered titles must follow the terminal object");
        const auto mo_menuless=hdmv::make_movie_object(2,0);
        req(be16(mo_menuless,48)==3,"menu-less BDMV must contain one terminal object plus two title objects");
        req(be16(mo_menuless,52)==0,"menu-less MovieObject 0 must contain zero commands so playback can stop");

    const std::array<std::uint16_t,2> title_menu_objects{{0xffffU,3U}};
    const std::vector<hdmv::MovieObject> title_menu_extra{{false,false,false,{hdmv::jump_object(0)}}};
    const auto mo_title_menu=hdmv::make_movie_object(2,1,false,title_menu_extra,{},title_menu_objects);
    const auto title_menu_parsed=parse_movie_objects(mo_title_menu);
    const auto& title_menu_top=title_menu_parsed[0].commands;
    req(title_menu_top.size()==13U,"top menu must combine natural-return and per-title Menu-call dispatch while preserving its loop selection state");
    req(title_menu_top[2].opcode==0x48400200U&&title_menu_top[2].operand1==4092U&&title_menu_top[2].operand2==2U,"title Menu-call dispatcher must compare reserved GPR4092 with the active title number");
    req(title_menu_top[3].opcode==0x20810000U&&title_menu_top[3].operand1==8U,"title Menu-call dispatcher must branch past the selection-save loop arm to the custom-action arm");
    req(title_menu_top[9].opcode==hdmv::jump_object(3).opcode&&title_menu_top[9].operand1==3U,"title Menu-call custom arm must jump to the title's synthetic navigation object");
    req(title_menu_top[10].operand1==2092U&&title_menu_top[10].operand2==0U&&title_menu_top[12].opcode==0x20810000U&&title_menu_top[12].operand1==5U,"top-menu-only natural title return must clear its pending state and restart the menu playlist in interactive title 0");

    const auto uop_tmp=std::filesystem::temp_directory_path()/"bdmvauthor-uop-test.mpls";
    {std::vector<unsigned char> fake(64,0);fake[0]='M';fake[1]='P';fake[2]='L';fake[3]='S';fake[50]=0x11;std::ofstream f(uop_tmp,std::ios::binary);f.write(reinterpret_cast<const char*>(fake.data()),static_cast<std::streamsize>(fake.size()));}
    hdmv::add_playlist_user_operation_mask(uop_tmp,(std::uint64_t{1}<<0)|(std::uint64_t{1}<<9));
    {std::ifstream f(uop_tmp,std::ios::binary);std::vector<unsigned char> patched((std::istreambuf_iterator<char>(f)),{});req((patched[48]&0x80U)!=0,"Blu-ray UOP index 0 must map to the high bit of the first UO-mask byte");req((patched[49]&0x40U)!=0,"Blu-ray UOP index 9 must map to the second bit of the second UO-mask byte");req(patched[50]==0x11,"Blu-ray UOP patching must preserve tsMuxer's existing UO-mask bits");}
    std::filesystem::remove(uop_tmp);

        Menu cleanup_menu;
        MenuButton keep_first; keep_first.label="FIRST"; keep_first.target_title=1;
        MenuButton remove_second; remove_second.label="SECOND"; remove_second.target_title=2;
        MenuButton shift_third; shift_third.label="THIRD"; shift_third.target_title=3;
        MenuButton menu_link; menu_link.label="SUB"; menu_link.target_kind=MenuButtonTargetKind::Menu; menu_link.target_menu_id="sub";
        MenuButton remove_audio;remove_audio.label="AUDIO SECOND";remove_audio.target_kind=MenuButtonTargetKind::AudioTrack;remove_audio.target_title=2;remove_audio.target_stream=2;
        MenuButton shift_subtitle;shift_subtitle.label="SUB THIRD";shift_subtitle.target_kind=MenuButtonTargetKind::SubtitleTrack;shift_subtitle.target_title=3;shift_subtitle.target_stream=1;
        cleanup_menu.buttons={keep_first,remove_second,shift_third,menu_link,remove_audio,shift_subtitle};
        Menu cleanup_child; cleanup_child.id="sub";
        MenuButton child_remove; child_remove.label="SECOND AGAIN"; child_remove.target_title=2;
        MenuButton child_shift; child_shift.label="FOURTH"; child_shift.target_title=4;
        MenuButton child_sub_off;child_sub_off.label="SUB OFF FOURTH";child_sub_off.target_kind=MenuButtonTargetKind::SubtitleOff;child_sub_off.target_title=4;
        cleanup_child.buttons={child_remove,child_shift,child_sub_off};
        cleanup_menu.submenus.push_back(cleanup_child);
        remove_title_references(cleanup_menu,2);
        req(cleanup_menu.buttons.size()==4, "removing a title must delete every root-menu button targeting it");
        req(cleanup_menu.buttons[0].target_title==1, "earlier title references must not change");
        req(cleanup_menu.buttons[1].target_title==2, "later root-menu title references must be renumbered");
        req(cleanup_menu.buttons[2].target_kind==MenuButtonTargetKind::Menu && cleanup_menu.buttons[2].target_menu_id=="sub",
            "title removal must not alter menu-link buttons");
        req(cleanup_menu.buttons[3].target_kind==MenuButtonTargetKind::SubtitleTrack && cleanup_menu.buttons[3].target_title==2,
            "later subtitle-selection references must be renumbered");
        req(cleanup_menu.submenus[0].buttons.size()==2, "removing a title must delete matching buttons recursively");
        req(cleanup_menu.submenus[0].buttons[0].target_title==3, "later submenu title references must be renumbered recursively");
        req(cleanup_menu.submenus[0].buttons[1].target_kind==MenuButtonTargetKind::SubtitleOff && cleanup_menu.submenus[0].buttons[1].target_title==3,
            "later subtitles-off references must be renumbered recursively");

        Menu m;m.width=kBdGraphicsWidth;m.height=kBdGraphicsHeight;
        for (int i = 0; i < 2; ++i) {
            MenuButton b;
            b.label = i ? "PLAY TWO" : "PLAY ONE";
            b.bounds = {500, 300 + i * 100, 600, 70};
            b.target_title = static_cast<std::uint16_t>(i + 1);
            m.buttons.push_back(b);
        }
        const auto seg = hdmv::make_menu_display_set(m);
        const auto seg_5994 = hdmv::make_menu_display_set(m, {}, {}, 7);
        req(seg_5994.front().payload.size()>4 && (seg_5994.front().payload[4]>>4)==7, "IG composition frame-rate code must follow the selected menu timing");
        req(seg.size() == 9, "expected ICS + PDS + 6 ODS + END");
        req(seg.front().type == 0x18 && seg[1].type == 0x14 && seg.back().type == 0x80,
            "IG segment order");
        check_menu_ics(seg.front().payload, {{{2,2,1,1}},{{1,1,2,2}}}, m.duration_seconds);
        const auto raw = hdmv::serialize_igs_file(seg);
        req(raw[0] == 'I' && raw[1] == 'G' && raw[10] == 0x18, "raw IGS framing");
        req(seg.front().pts90k == hdmv::kMenuIgsPts90k, "raw menu IGS PTS regression");

        Menu labeled_menu = m;
        MenuOverlay text_label; text_label.kind=MenuOverlayKind::Text; text_label.text="CHAPTERS"; text_label.bounds={100,80,500,80};
        MenuOverlay image_label; image_label.kind=MenuOverlayKind::Image; image_label.image="decorative-logo.png"; image_label.bounds={1400,80,320,120};
        labeled_menu.overlays.push_back(text_label); labeled_menu.overlays.push_back(image_label);
        const auto labeled_seg = hdmv::make_menu_display_set(labeled_menu);
        req(labeled_seg.size()==seg.size(), "burned-in labels must not change IG topology");
        for(std::size_t i=0;i<seg.size();++i){
            req(labeled_seg[i].type==seg[i].type && labeled_seg[i].pts90k==seg[i].pts90k &&
                labeled_seg[i].dts90k==seg[i].dts90k && labeled_seg[i].payload==seg[i].payload,
                "burned-in labels must not change any IG segment bytes");
        }

        Menu submenu_link;
        MenuButton to_sub;to_sub.label="EXTRAS";to_sub.bounds={500,300,600,70};
        to_sub.target_kind=MenuButtonTargetKind::Menu;to_sub.target_menu_id="extras";submenu_link.buttons.push_back(to_sub);
        hdmv::MenuObjectMap objects{{"top",0},{"extras",1}};
        const auto link_seg=hdmv::make_menu_display_set(submenu_link,objects);
        // For a preloaded one-button page the first navigation command begins at byte 76:
        // the ten-byte composition/selection timeout block exists only in multiplexed IG.
        constexpr std::size_t preloaded_one_button_command = 76U;
        req(be32(link_seg.front().payload,preloaded_one_button_command)==hdmv::jump_object(1).opcode, "submenu button must use Jump_Object");
        req(be32(link_seg.front().payload,preloaded_one_button_command+4U)==1, "submenu Jump_Object target regression");
        const std::vector<std::vector<hdmv::NavCommand>> supplied_commands{{hdmv::jump_object(5)}};
        const auto action_link_seg=hdmv::make_menu_display_set(submenu_link,objects,supplied_commands);
        req(be32(action_link_seg.front().payload,preloaded_one_button_command)==hdmv::jump_object(5).opcode&&be32(action_link_seg.front().payload,preloaded_one_button_command+4U)==5,
            "IG serializer must preserve an explicitly supplied synthetic-MovieObject jump");

        Menu styled_menu = m;
        styled_menu.default_button_style.font_family.clear();
        styled_menu.default_button_style.font_size_px = 42;
        styled_menu.default_button_style.normal.text_color = {12,34,56,255};
        styled_menu.default_button_style.normal.background_color = {90,80,70,200};
        styled_menu.default_button_style.selected.background_color = {10,90,160,230};
        styled_menu.buttons[0].label = "CUSTOM TITLE";
        styled_menu.buttons[1].use_custom_style = true;
        styled_menu.buttons[1].style = styled_menu.default_button_style;
        styled_menu.buttons[1].style.activated.background_color = {200,40,20,255};
        const auto styled = hdmv::make_menu_display_set(styled_menu);
        req(styled.size() == seg.size(), "button styling must not change IG segment topology");
        req(styled[1].payload != seg[1].payload, "custom button colors must change the PDS palette");
        req(styled[2].payload != seg[2].payload, "custom button label/font size must change the normal ODS bitmap");
        req(styled.front().payload == seg.front().payload, "button styling must not change ICS navigation/timing bytes");

        const auto image_tmp = std::filesystem::temp_directory_path() / "bdmvauthor-test-image-button";
        std::filesystem::remove_all(image_tmp);
        std::filesystem::create_directories(image_tmp);
        const int iw = m.buttons[0].bounds.width, ih = m.buttons[0].bounds.height;
        Bytes rgba(static_cast<std::size_t>(iw) * static_cast<std::size_t>(ih) * 4U, 0);
        for (std::size_t i = 0; i < rgba.size() / 4U; ++i) {
            rgba[i*4U] = 220; rgba[i*4U+1U] = 40; rgba[i*4U+2U] = 40; rgba[i*4U+3U] = 255;
        }
        const auto normal_path = image_tmp / "normal.rgba";
        { std::ofstream f(normal_path, std::ios::binary); f.write(reinterpret_cast<const char*>(rgba.data()), static_cast<std::streamsize>(rgba.size())); }
        Menu image_menu = m;
        image_menu.buttons[0].kind = MenuButtonKind::Image;
        image_menu.buttons[0].normal_image = normal_path;
        image_menu.buttons[0].selected_image.clear();
        image_menu.buttons[0].image_highlight_color = {128,200,255,112};
        const auto image_seg = hdmv::make_menu_display_set(image_menu);
        req(image_seg.size() == seg.size(), "image buttons must preserve IG segment topology");
        req(image_seg.front().payload == seg.front().payload, "image buttons must not change ICS navigation/timing bytes");
        req(image_seg[2].payload != image_seg[3].payload,
            "missing selected image must synthesize a visibly different highlighted selected state");
        req(image_seg[3].payload.size() == image_seg[4].payload.size() && image_seg[3].payload.size() > 11 &&
                std::equal(image_seg[3].payload.begin()+11,image_seg[3].payload.end(),image_seg[4].payload.begin()+11),
            "activated image state should reuse the selected image/highlight when no separate activated image exists");
        std::filesystem::remove_all(image_tmp);

        Menu geometry_menu = m;
        geometry_menu.buttons[0].bounds = {123,456,321,99};
        const auto geometry_seg = hdmv::make_menu_display_set(geometry_menu);
        // Preloaded IG omits the ten-byte multiplexed timeout block, so the first
        // button position is ten bytes earlier than in the historical stream_model=0 layout.
        req(be16(geometry_seg.front().payload,46) == 123 && be16(geometry_seg.front().payload,48) == 456,
            "button drag position must be authored into the ICS");
        req(be16(geometry_seg[2].payload,7) == 321 && be16(geometry_seg[2].payload,9) == 99,
            "button resize dimensions must be authored into the ODS");
        // Geometry changes may legitimately alter the directional graph; the
        // authored position/size checks above ensure the edited bounds reach IG.

        Menu horizontal_menu = m;
        horizontal_menu.buttons[0].bounds = {120,420,420,90};
        horizontal_menu.buttons[1].bounds = {900,420,420,90};
        const auto horizontal_seg = hdmv::make_menu_display_set(horizontal_menu);
        check_menu_ics(horizontal_seg.front().payload, {{{1,1,2,2}},{{2,2,1,1}}}, horizontal_menu.duration_seconds);

        const auto tmp = std::filesystem::temp_directory_path() / "bdmvauthor-test-javafree";
        std::filesystem::remove_all(tmp);
        hdmv::write_control_files(tmp / "BDMV", 2);
        std::filesystem::create_directories(tmp / "CERTIFICATE/BACKUP");
        std::string why;
        req(AuthorEngine::validate_java_free_bdmv(tmp, &why), why.c_str());
        std::filesystem::create_directories(tmp / "BDMV/JAR");
        req(!AuthorEngine::validate_java_free_bdmv(tmp, &why), "BD-J directory was not rejected");
        std::filesystem::remove_all(tmp);

        const auto tree_tmp = std::filesystem::temp_directory_path() / "bdmvauthor-test-menu-tree";
        std::filesystem::remove_all(tree_tmp);
        std::filesystem::create_directories(tree_tmp);
        const auto fake_video = tree_tmp / "title.mkv";
        const auto fake_bg = tree_tmp / "background.png";
        { std::ofstream(fake_video).put('x'); std::ofstream(fake_bg).put('x'); }
        Project tree_project;tree_project.output_image=tree_tmp/"disc.udf";
        Title tree_title;tree_title.name="Feature";tree_title.source=fake_video;tree_project.titles.push_back(tree_title);
        tree_project.menu.background_image=fake_bg;
        MenuButton root_title;root_title.label="Feature";root_title.bounds={100,100,400,80};root_title.target_title=1;tree_project.menu.buttons.push_back(root_title);
        Menu child;child.id="extras";child.name="Extras";child.background_image=tree_tmp/"missing-but-inherited.png";
        MenuButton duplicate=root_title;duplicate.bounds={200,200,400,80};child.buttons.push_back(duplicate);
        Menu grandchild;grandchild.id="extras-more";grandchild.name="More Extras";
        grandchild.background_image=tree_tmp/"also-missing-but-recursively-inherited.png";
        grandchild.background_color={255,0,255,255}; // ignored while inheritance remains enabled
        child.submenus.push_back(grandchild);
        tree_project.menu.submenus.push_back(child);
        // The child and grandchild background paths are intentionally invalid: recursive
        // inheritance must resolve them to the valid top-menu path before validation. Their
        // automatic Back buttons also provide parent-menu targets without caller-created links.
        AuthorEngine::validate_project(tree_project);
        MenuButton bad_link;bad_link.label="Broken";bad_link.bounds={600,100,300,80};bad_link.target_kind=MenuButtonTargetKind::Menu;bad_link.target_menu_id="missing";tree_project.menu.buttons.push_back(bad_link);
        bool rejected=false;try{AuthorEngine::validate_project(tree_project);}catch(const std::exception&){rejected=true;}
        req(rejected,"unknown submenu target must be rejected");
        std::filesystem::remove_all(tree_tmp);

        const auto audio_tmp = std::filesystem::temp_directory_path() / "bdmvauthor-test-truehd-core";
        remove_test_tree(audio_tmp);
        std::filesystem::create_directories(audio_tmp);
        Bytes thd;
        for (unsigned i = 0; i < 77; ++i) {
            Bytes frame(24, 0);
            frame[1] = 12; // TrueHD frame length = 24 bytes
            if (i == 0) {
                frame[4] = 0xf8; frame[5] = 0x72; frame[6] = 0x6f; frame[7] = 0xba;
                frame[8] = 0x00; // 48 kHz => 40 samples/frame
            }
            frame[9] = static_cast<unsigned char>(i);
            thd.insert(thd.end(), frame.begin(), frame.end());
        }
        auto fake_ac3_core = [](std::size_t frame_bytes, unsigned frmsizecod) {
            Bytes bytes(3U * frame_bytes, 0);
            for (unsigned i = 0; i < 3; ++i) {
                const std::size_t p = static_cast<std::size_t>(i) * frame_bytes;
                bytes[p] = 0x0b;
                bytes[p + 1U] = 0x77;
                bytes[p + 2U] = static_cast<unsigned char>(i + 1U);
                bytes[p + 4U] = static_cast<unsigned char>(frmsizecod); // 48 kHz fscod=0
            }
            return bytes;
        };
        Bytes ac3 = fake_ac3_core(2560U, 36U); // 640 kb/s
        auto put = [](const std::filesystem::path& path, const Bytes& bytes) {
            std::ofstream f(path, std::ios::binary);
            f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        };
        const auto thd_path = audio_tmp / "a.thd";
        const auto ac3_path = audio_tmp / "a.ac3";
        const auto merged_path = audio_tmp / "a.thd+ac3";
        put(thd_path, thd);
        put(ac3_path, ac3);
        const int merged_peak_640 = detail::merge_truehd_ac3_core(thd_path, ac3_path, merged_path);
        Bytes merged;
        {
            std::ifstream merged_file(merged_path, std::ios::binary);
            merged.assign(std::istreambuf_iterator<char>(merged_file), {});
        } // Destroy the stream/Win32 handle before any cleanup can run.
        req(merged.size() == 3U * 2560U + 77U * 24U, "TrueHD/core merge size regression");
        req(merged_peak_640 == 655, "TrueHD/core measured peak bitrate regression");
        req(merged[0] == 0x0b && merged[1] == 0x77 && merged[2] == 1, "first AC-3 core frame regression");
        const std::size_t core2 = 2560U + 39U * 24U;
        req(merged[core2] == 0x0b && merged[core2 + 1] == 0x77 && merged[core2 + 2] == 2,
            "second AC-3 core interleave regression");
        const std::size_t core3 = core2 + 2560U + 38U * 24U;
        req(merged[core3] == 0x0b && merged[core3 + 1] == 0x77 && merged[core3 + 2] == 3,
            "third AC-3 core interleave regression");

        // The compatibility-core bitrate is user configurable.  0.1.80 and
        // earlier sliced every core as 2560 bytes, corrupting any legal rate
        // other than 640 kb/s.  448 kb/s AC-3 uses 1792-byte frames at 48 kHz.
        const auto ac3_448_path = audio_tmp / "a-448.ac3";
        const auto merged_448_path = audio_tmp / "a-448.thd+ac3";
        const Bytes ac3_448 = fake_ac3_core(1792U, 30U); // bitrate index 15 => 448 kb/s
        put(ac3_448_path, ac3_448);
        const int merged_peak_448 = detail::merge_truehd_ac3_core(thd_path, ac3_448_path, merged_448_path);
        Bytes merged_448;
        {
            std::ifstream merged_448_file(merged_448_path, std::ios::binary);
            merged_448.assign(std::istreambuf_iterator<char>(merged_448_file), {});
        } // Destroy the stream/Win32 handle before temporary-directory cleanup.
        req(merged_448.size() == 3U * 1792U + 77U * 24U, "448 kb/s TrueHD/core merge size regression");
        req(merged_peak_448 == 463, "448 kb/s TrueHD/core measured peak bitrate regression");
        const std::size_t core2_448 = 1792U + 39U * 24U;
        req(merged_448[core2_448] == 0x0b && merged_448[core2_448 + 1] == 0x77 && merged_448[core2_448 + 2] == 2,
            "448 kb/s TrueHD/core interleave regression");
        remove_test_tree(audio_tmp);

        req(sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            "SHA-256 implementation regression");
        const auto fp_tmp = std::filesystem::temp_directory_path() / "bdmvauthor-test-fingerprint";
        std::filesystem::remove_all(fp_tmp);std::filesystem::create_directories(fp_tmp);
        const auto small = fp_tmp / "small-original.bin";
        { std::ofstream f(small,std::ios::binary);std::string block(2U*1024U*1024U,'s');f.write(block.data(),static_cast<std::streamsize>(block.size())); }
        const auto sfp = fingerprint_media_file(small);req(sfp.whole_file,"files smaller than 3 MiB must use whole-file SHA-256");
        const auto renamed = fp_tmp / "renamed.bin";std::filesystem::rename(small,renamed);
        req(fingerprint_media_file(renamed).canonical()==sfp.canonical(),"media fingerprint must not depend on filename/path");
        const auto large = fp_tmp / "large.bin";
        { std::ofstream f(large,std::ios::binary);std::string block(4U*1024U*1024U,'L');f.write(block.data(),static_cast<std::streamsize>(block.size())); }
        const auto lfp=fingerprint_media_file(large);req(!lfp.whole_file,"files >= 3 MiB must use sampled SHA-256");
        { std::fstream f(large,std::ios::binary|std::ios::in|std::ios::out);f.seekp(static_cast<std::streamoff>(lfp.middle_offset+123));f.put('X'); }
        req(fingerprint_media_file(large).canonical()!=lfp.canonical(),"middle sample change must invalidate media fingerprint");
        std::filesystem::remove_all(fp_tmp);

        std::cout << "bdmvauthor core tests ok\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
}
