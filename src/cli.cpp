// SPDX-License-Identifier: Apache-2.0
#include "bdmvauthor/author.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

using namespace bdmvauthor;

namespace {
void usage() {
    std::cout << "BDMV Author " BDMVAUTHOR_VERSION " - Blu-ray / Ultra HD Blu-ray / DVD-Video authoring\n\n" R"(Usage:
  bdmvauthor-cli -o PATH [options] video1 [per-title options] video2 ...

General options:
  --target MODE                 bluray-1080 (default), uhd-bluray-2160, or dvd-video-480p
  --size-target SIZE            Disc-image capacity in decimal GB (presets include 1.46/2.66 GB 8 cm DVD,
                               7.8/15.6 GB 8 cm BD, 4.7/8.5/25/50/66/100 GB), or unlimited
  --automatic-bitrate           Maximize video bitrate while fitting the selected size (default)
  --manual-bitrate              Use configured video bitrates; render is rejected if they exceed the size target
  --frame-rate RATE             Menu/default title rate: auto (default), or a target-legal explicit rate
                               Blu-ray: 23.976p, 24p, 50i, 59.94i; AVC also 1080p25/29.97 via x264 fake interlaced; 720p also 50p/59.94p
                               UHD: 23.976p, 24p, 25p; 50p/59.94p/60p are experimental explicit-only modes
                               DVD presets: film-dvd, ntsc-dvd, pal-dvd
  --menu-resolution RES        Menu raster: a legal target raster (Blu-ray/UHD default 1920x1080; DVD defaults NTSC)
  --menu-aspect ASPECT         Menu display aspect: 16:9 (default) or 4:3 where legal
  -V, --label NAME             UDF Disc label
  --menu-background IMG       Current menu still image
  --menu-background-video FILE Current menu video; duration follows media automatically
  --menu-background-color C   Current menu background/padding color (#RRGGBB[AA])
  --menu-audio FILE           Explicit menu audio; overrides background-video audio
  --menu-duration SECONDS     Manual duration for still-image menus with no audio
  --menu-loop                 Loop menu audio/video (default)
  --no-menu-loop              Play menu audio/video once, then hold the menu
  --menu-name NAME            Rename the current menu
  --no-menu                   Author without a visible menu; empty startup means
                              play all titles in order, then stop
  --submenu ID                Create/select child submenu ID (inherits parent); also
                              adds a link button on the parent automatically
  --end-submenu               Return to the parent menu
  --menu-link ID LABEL        Add a button to the current menu that jumps to menu ID
  --title-button N LABEL      Add another button for title N to the current menu
  --title-button-chapter N C LABEL
                              Add a button that starts title N at chapter C
  --audio-button N S LABEL    Add a button selecting audio stream S for title N
  --subtitle-button N S LABEL Add a button selecting subtitle stream S for title N and enable it
  --subtitles-off-button N LABEL
                              Add a button that turns subtitles off for title N
  --button-action-title N     Append Play title N to the most recently added button
  --button-action-title-chapter N C
                              Append Play title N from chapter C
  --button-action-menu ID     Append Open menu ID (must be the final action)
  --button-action-audio N S   Append audio-stream selection for title N
  --button-action-subtitle N S
                              Append subtitle selection for title N
  --button-action-subtitles-off N
                              Append subtitles-off for title N
  --button-action-repeat N     Set repeat count on the most recently appended button action (0=forever)
  --button-repeat-group-begin N
                              Begin a repeat group before the most recent action (N=0 forever)
  --button-repeat-group-end    End the current button repeat group
  --first-play-title N        Append title N to the disc startup sequence
  --first-play-title-chapter N C
                              Append title N from chapter C to the startup sequence
  --first-play-menu ID        Finish disc startup by opening menu ID
  --first-play-repeat N       Set repeat count on the most recent First Playback action (0=forever)
  --first-play-repeat-group-begin N
                              Begin a group before the most recent First Playback action (N=0 forever)
  --first-play-repeat-group-end
                              End the current First Playback repeat group
  --text-label TEXT           Add a non-interactive text label to the current menu
  --image-label FILE          Add a non-interactive image label to the current menu
  --label-x PX                Position of the most recently added label
  --label-y PX
  --label-width PX            Size of the most recently added label
  --label-height PX
  --label-font FAMILY         Text-label font family
  --label-font-size PX        Text-label font size
  --label-color COLOR         Text-label color
  --label-bold | --label-regular
  --label-italic | --label-roman
  --no-auto-back              Disable the current submenu's automatic Back button
  --auto-back                 Enable it again (default for submenus)
  --ffmpeg PATH               ffmpeg executable
  --ffprobe PATH              ffprobe executable
  --x264 PATH                 x264 executable
  --x265 PATH                 x265 executable
  --h264-provider MODE        ffmpeg (default) or x264
  --hevc-provider MODE        ffmpeg (default) or x265
  --tsmuxer PATH              Override bundled tsMuxer executable
  --dvdauthor PATH            dvdauthor executable for DVD-Video
  --spumux PATH               spumux executable for DVD menu/subtitle SPUs
  --mplex PATH                mplex executable for DVD NAV-sector multiplexing
  --mkisofs PATH              mkisofs executable for DVD-Video ISO creation
  --cache                     Enable both persistent caches (compatibility alias; default)
  --no-cache                  Disable both persistent caches
  --encode-cache              Enable encoded-media cache reuse/publication
  --no-encode-cache           Disable encoded-media cache reuse/publication
  --compliance-cache          Enable compliance-analysis cache reuse/publication
  --no-compliance-cache       Disable compliance-analysis cache reuse/publication
  --refresh-encode-cache      Ignore matching encoded cache entries and refresh them when encoding is needed
  --refresh-compliance-cache  Ignore cached compliance results and re-run the bounded analysis
  --force-reencode-all        Force every title's video and audio to be encoded
  --keep-work                 Preserve temporary authoring tree
  --work DIR                  Use/preserve this work directory

Per-title encoding options:
  These options apply to each video argument that follows them, so they can be
  changed between clips.
  --video-codec MODE          1080p: x264/mpeg2; UHD: hevc or x264 (1080p23.976/24); DVD: mpeg2 only
  --video-bitrate KBPS        Manual video bitrate for following titles; disables automatic bitrate
  --video-minrate KBPS        Minimum video rate (default 0; target/codec limit enforced)
  --video-maxrate KBPS        Maximum video rate (default = target/codec standard maximum)
  --x264-preset PRESET        x264 preset for 1080p fallback
  --x265-preset PRESET        libx265 preset for UHD fallback
  --two-pass                  Enable two-pass video encoding
  --single-pass               Use single-pass encoding (default)
  --audio-codec MODE          Blu-ray/UHD: ac3, lpcm, dca, truehd-ac3; DVD: ac3/lpcm
  --audio-bitrate KBPS        AC-3/DTS bitrate; DVD AC-3 is limited to 448 kb/s
  --audio-sample-rate HZ      LPCM/TrueHD rate: auto, 48000, 96000, 192000 (DVD defaults 48000)
  --audio-bit-depth BITS      LPCM/TrueHD depth: auto, 16, 24 (DVD defaults 16)
  --force-reencode            Force video and audio encoding for following titles
  --allow-passthrough         Remux compliant streams for following titles (default)
  --title-resolution RES      Per-title raster: auto (default) or a legal target raster
  --title-aspect ASPECT       Per-title display aspect: auto (default), 16:9, or 4:3 where legal
  --title-frame-rate RATE     Per-title timing for following titles: inherit (default), auto,
                              or a legal rate/preset for the selected target
  --keyframe-interval N       GOP/keyframe interval in frames for following titles; 0 keeps
                              the historical default. Explicit values are spec-validated.
  --video-option NAME=VALUE  Add a codec-private option for the currently selected video codec
  --audio-option NAME=VALUE  Add a codec-private option for the currently selected audio codec
  --clear-video-options      Clear advanced options for the selected title video codec
  --clear-audio-options      Clear advanced options for the selected title audio codec
  --audio-stream-codec S MODE Override audio stream S codec for following titles
  --audio-stream-bitrate S K  Override AC-3/DTS bitrate for audio stream S
  --audio-stream-sample-rate S HZ
                              LPCM/TrueHD rate: auto, 48000, 96000, 192000
  --audio-stream-bit-depth S BITS
                              LPCM/TrueHD depth: auto, 16, 24
  --audio-stream-lpcm-rate S HZ
                              Legacy alias selecting LPCM plus an explicit rate
  --audio-stream-lpcm-depth S BITS
                              Legacy alias selecting LPCM plus an explicit depth
  --audio-stream-option S N=V Add a codec-private option for audio stream S
  --audio-stream-clear-options S
                              Clear advanced options for audio stream S's selected codec
  --audio-stream-default S    Remove the encoding override for audio stream S
  --audio-stream-language S ISO3|source
                              Override authored audio language metadata, or preserve source
  --audio-stream-channels S LAYOUT
                              Downmix stream S to mono, stereo, quad, 5.1, or 7.1; source preserves channels
  --subtitle-stream-language S ISO3|source
                              Override authored subtitle language metadata, or preserve source
  --default-audio-stream MODE Initial audio for following titles: player or 1-based stream number
  --default-subtitle-stream MODE
                              Initial subtitles for following titles: player, off, or 1-based stream number
  --subtitle-stream-style S KEY=VALUE
                              Set per-stream subtitle rendering property (repeatable)
  --subtitle-stream-clear-style S
                              Restore backend-default subtitle rendering for stream S
  --subtitle-file FILE        Attach an external subtitle file to following titles
  --subtitle-file-language ISO3
                              Set the language of the most recently attached subtitle file
  --subtitle-file-style KEY=VALUE
                              Set rendering property of the most recently attached subtitle
                              size and bottom-offset use 1080-line subtitle units (su);
                              80 su = 80 px at 1080p/UHD and scales at 720p/DVD
  --chapters-source           Use source chapters; if absent, every 5 minutes (default)
  --chapters-manual LIST      Manual additional chapter starts, comma-separated; each may
                              be seconds, MM:SS[.mmm], or HH:MM:SS[.mmm]
  --chapters-every DURATION   Add a chapter every duration (e.g. 300s, 5m, or 05:00)
  --no-chapters               No additional chapter divisions

Menu encoding options:
  --menu-video-codec MODE     1080p: x264/mpeg2; UHD: hevc or x264 (1080p23.976/24); DVD: mpeg2 only
  --menu-video-bitrate KBPS   Manual menu video bitrate; disables automatic bitrate
  --menu-video-minrate KBPS   Minimum menu video rate (default 0)
  --menu-video-maxrate KBPS   Maximum menu video rate (default = target/codec maximum)
  --menu-x264-preset PRESET   Menu x264 preset; default medium
  --menu-x265-preset PRESET   Menu libx265 preset; default medium
  --menu-two-pass             Enable two-pass menu video encoding
  --menu-single-pass          Use single-pass menu encoding (default)
  --menu-audio-codec MODE     Blu-ray/UHD: ac3, lpcm, dca, truehd-ac3; DVD: ac3/lpcm
  --menu-audio-bitrate KBPS   Menu audio bitrate; DVD AC-3 is limited to 448 kb/s
  --menu-audio-sample-rate HZ LPCM/TrueHD rate: auto, 48000, 96000, 192000
  --menu-audio-bit-depth BITS LPCM/TrueHD depth: auto, 16, 24
  --menu-keyframe-interval N  Menu GOP/keyframe interval in frames; 0 keeps the historical default
  --menu-video-option N=V    Add a codec-private option for the selected menu video codec
  --menu-audio-option N=V    Add a codec-private option for the selected menu audio codec
  --menu-clear-video-options Clear advanced options for the selected menu video codec
  --menu-clear-audio-options Clear advanced options for the selected menu audio codec

Menu button default style:
  --menu-button-font FAMILY
  --menu-button-font-size PX
  --menu-button-bold | --menu-button-regular
  --menu-button-italic | --menu-button-roman
  --menu-button-corner-radius PX
  --menu-button-normal-text COLOR
  --menu-button-normal-bg COLOR
  --menu-button-normal-border COLOR
  --menu-button-normal-border-width PX
  --menu-button-active-text COLOR
  --menu-button-active-border COLOR
  --menu-button-active-border-width PX
  --menu-button-selected-text COLOR
  --menu-button-selected-bg COLOR
  --menu-button-selected-border COLOR
  --menu-button-selected-border-width PX
  --menu-button-activated-text COLOR
  --menu-button-activated-bg COLOR
  --menu-button-activated-border COLOR
  --menu-button-activated-border-width PX

Per-button appearance and placement:
  --button-style N            Select 1-based menu button N for subsequent
                              --button-* options; it starts as a copy of the
                              current menu default style.
  --button-label TEXT         Set selected button label/title
  --button-x PX               Left position on the 3840x2160 design canvas
  --button-y PX               Top position on the 3840x2160 design canvas
  --button-width PX           Entire button width
  --button-height PX          Entire button height
  --button-image FILE         Make this an image-only button; unselected image
  --button-selected-image FILE
                              Optional selected-state image
  --button-highlight COLOR    Tint used when selected image is omitted;
                              default light blue (#80c8ff70)
  --button-text               Change the selected button back to a text button
  --button-font FAMILY
  --button-font-size PX
  --button-bold | --button-regular
  --button-italic | --button-roman
  --button-corner-radius PX
  --button-normal-text COLOR
  --button-normal-bg COLOR
  --button-normal-border COLOR
  --button-normal-border-width PX
  --button-active-text COLOR
  --button-active-border COLOR
  --button-active-border-width PX
  --button-selected-text COLOR
  --button-selected-bg COLOR
  --button-selected-border COLOR
  --button-selected-border-width PX
  --button-activated-text COLOR
  --button-activated-bg COLOR
  --button-activated-border COLOR
  --button-activated-border-width PX

Colors are #RRGGBB or #RRGGBBAA (alpha last).

Notes:
  Resolution/aspect Auto chooses the closest legal title mode to the source; an
  exact source raster is preferred, and otherwise ties prefer scaling up rather
  than scaling down. Standard Blu-ray supports 1920x1080, 1440x1080, 1280x720,
  720x576, and 720x480. DVD supports its standard 720/704/352-pixel PAL/NTSC
  rasters. 4:3 is available only on legal SD modes; HD/UHD remains 16:9.
  Source containers do not affect the passthrough decision. Video and audio
  elementary streams are checked independently and remuxed when Blu-ray
  compliant; selected encoding options are fallbacks for streams that fail.

  truehd-ac3 is experimental. It encodes TrueHD plus a configurable AC-3
  compatibility stream and interleaves them for Blu-ray/tsMuxer input.

  -h, --help                  Show this help
)";
}

VideoCodec parse_video_codec(const std::string& s) {
    if (s == "x264") return VideoCodec::X264;
    if (s == "mpeg2" || s == "mpeg-2") return VideoCodec::Mpeg2;
    if (s == "hevc" || s == "h265" || s == "h.265") return VideoCodec::Hevc;
    throw std::runtime_error("video codec must be x264, mpeg2, or hevc");
}

DiscTarget parse_target(const std::string& s) {
    if (s == "bluray-1080" || s == "bluray" || s == "1080p") return DiscTarget::BluRay1080;
    if (s == "uhd-bluray-2160" || s == "uhd" || s == "2160p" || s == "4k") return DiscTarget::UltraHdBluRay2160;
    if (s == "dvd-video-480p" || s == "dvd" || s == "480p") return DiscTarget::DvdVideo480p;
    throw std::runtime_error("target must be bluray-1080, uhd-bluray-2160, or dvd-video-480p");
}

std::uint64_t parse_size_target(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c){return static_cast<char>(std::tolower(c));});
    if(text=="unlimited"||text=="none"||text=="0")return 0ULL;
    if(text.size()>=2U&&text.substr(text.size()-2)=="gb")text.resize(text.size()-2);
    try{
        std::size_t used=0;const double gb=std::stod(text,&used);
        if(used!=text.size()||!std::isfinite(gb)||gb<=0.0)throw std::runtime_error("bad");
        const long double bytes=static_cast<long double>(gb)*1000000000.0L;
        if(bytes>static_cast<long double>(std::numeric_limits<std::uint64_t>::max()))throw std::runtime_error("bad");
        return static_cast<std::uint64_t>(std::llround(bytes));
    }catch(...){throw std::runtime_error("--size-target expects a positive decimal GB value (for example 4.7GB, 25GB, 50GB) or unlimited");}
}

AudioCodec parse_audio_codec(const std::string& s) {
    if (s == "ac3" || s == "ac-3") return AudioCodec::Ac3;
    if (s == "lpcm" || s == "pcm") return AudioCodec::Lpcm;
    if (s == "dca" || s == "dts") return AudioCodec::Dca;
    if (s == "truehd-ac3" || s == "truehd+ac3" || s == "truehd") return AudioCodec::TrueHdAc3;
    throw std::runtime_error("audio codec must be ac3, lpcm, dca, or truehd-ac3");
}

int parse_int(const std::string& text, const std::string& option) {
    int value = 0;
    if (!parse_integer_exact(text, value)) throw std::runtime_error(option + " requires an integer");
    return value;
}


double parse_duration_seconds(std::string text, const std::string& option) {
    if (text.empty()) throw std::runtime_error(option + " requires a duration");
    double multiplier = 1.0;
    if (text.back() == 's' || text.back() == 'S') text.pop_back();
    else if (text.back() == 'm' || text.back() == 'M') { text.pop_back(); multiplier = 60.0; }
    if (text.find(':') != std::string::npos) {
        std::vector<double> parts;
        std::stringstream ss(text);
        std::string part;
        while (std::getline(ss, part, ':')) {
            try { parts.push_back(std::stod(part)); }
            catch (...) { throw std::runtime_error(option + " has an invalid duration"); }
        }
        if (parts.size() == 2U) {
            if (!std::isfinite(parts[0]) || !std::isfinite(parts[1]) || parts[0] < 0.0 || parts[1] < 0.0 || parts[1] >= 60.0)
                throw std::runtime_error(option + " has an invalid MM:SS duration");
            return parts[0] * 60.0 + parts[1];
        }
        if (parts.size() == 3U) {
            if (!std::isfinite(parts[0]) || !std::isfinite(parts[1]) || !std::isfinite(parts[2]) ||
                parts[0] < 0.0 || parts[1] < 0.0 || parts[1] >= 60.0 || parts[2] < 0.0 || parts[2] >= 60.0)
                throw std::runtime_error(option + " has an invalid HH:MM:SS duration");
            return parts[0] * 3600.0 + parts[1] * 60.0 + parts[2];
        }
        throw std::runtime_error(option + " duration must be seconds, MM:SS, or HH:MM:SS");
    }
    try {
        std::size_t used = 0;
        const double value = std::stod(text, &used) * multiplier;
        if (used != text.size() || !std::isfinite(value) || !(value >= 0.0)) throw std::runtime_error("bad");
        return value;
    } catch (...) { throw std::runtime_error(option + " has an invalid duration"); }
}

std::vector<double> parse_chapter_list(const std::string& text, const std::string& option) {
    std::vector<double> chapters;
    std::stringstream ss(text);
    std::string part;
    while (std::getline(ss, part, ',')) {
        if (part.empty()) continue;
        chapters.push_back(parse_duration_seconds(part, option));
    }
    if (chapters.empty()) throw std::runtime_error(option + " requires at least one chapter point");
    return chapters;
}

Rgba parse_color(std::string s, const std::string& option) {
    if (!s.empty() && s.front() == '#') s.erase(s.begin());
    if (s.size() != 6 && s.size() != 8) throw std::runtime_error(option + " color must be #RRGGBB or #RRGGBBAA");
    auto byte = [&](std::size_t p) -> std::uint8_t {
        unsigned int value = 0;
        if (!parse_integer_exact(std::string_view(s).substr(p, 2), value, 16) || value > 255u)
            throw std::runtime_error(option + " contains an invalid color");
        return static_cast<std::uint8_t>(value);
    };
    return {byte(0),byte(2),byte(4),s.size()==8?byte(6):static_cast<std::uint8_t>(255)};
}

bool parse_on_off(std::string value,const std::string& option){std::transform(value.begin(),value.end(),value.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});if(value=="1"||value=="true"||value=="yes"||value=="on")return true;if(value=="0"||value=="false"||value=="no"||value=="off")return false;throw std::runtime_error(option+" expects true/false, yes/no, on/off, or 1/0");}
void apply_subtitle_style_option(SubtitleStyle& st,const std::string& assignment,const std::string& option){
    const auto eq=assignment.find('=');if(eq==std::string::npos||eq==0||eq+1>=assignment.size())throw std::runtime_error(option+" expects KEY=VALUE");
    auto key=assignment.substr(0,eq);auto value=assignment.substr(eq+1);std::transform(key.begin(),key.end(),key.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});st.override_style=true;
    auto integer=[&](int lo,int hi){const int n=parse_int(value,option);if(n<lo||n>hi)throw std::runtime_error(option+" value out of range");return n;};
    auto number=[&](double lo,double hi){double n=0;try{n=std::stod(value);}catch(...){throw std::runtime_error(option+" expects a number");}if(!std::isfinite(n)||n<lo||n>hi)throw std::runtime_error(option+" value out of range");return n;};
    if(key=="font")st.font_family=value;else if(key=="size")st.font_size_px=integer(4,256);else if(key=="color")st.font_color=parse_color(value,option);
    else if(key=="bold")st.bold=parse_on_off(value,option);else if(key=="italic")st.italic=parse_on_off(value,option);else if(key=="underline")st.underline=parse_on_off(value,option);else if(key=="strikeout")st.strikeout=parse_on_off(value,option);
    else if(key=="line-spacing")st.line_spacing=number(0.1,10.0);else if(key=="border")st.border_width=number(0.0,32.0);else if(key=="bottom-offset")st.bottom_offset_px=integer(0,2160);else if(key=="fade-in")st.fade_in_ms=number(0.0,10000.0);else if(key=="fade-out")st.fade_out_ms=number(0.0,10000.0);
    else if(key=="dvd-outline-color")st.dvd_outline_color=parse_color(value,option);else if(key=="dvd-shadow-color")st.dvd_shadow_color=parse_color(value,option);else if(key=="dvd-shadow-x")st.dvd_shadow_offset_x=integer(-100,100);else if(key=="dvd-shadow-y")st.dvd_shadow_offset_y=integer(-100,100);
    else if(key=="dvd-halign"){if(value!="default"&&value!="left"&&value!="center"&&value!="right")throw std::runtime_error(option+" dvd-halign must be default, left, center, or right");st.dvd_horizontal_alignment=value;}
    else if(key=="dvd-valign"){if(value!="top"&&value!="center"&&value!="bottom")throw std::runtime_error(option+" dvd-valign must be top, center, or bottom");st.dvd_vertical_alignment=value;}
    else if(key=="dvd-left-margin")st.dvd_left_margin_px=integer(0,720);else if(key=="dvd-right-margin")st.dvd_right_margin_px=integer(0,720);else if(key=="dvd-top-margin")st.dvd_top_margin_px=integer(0,720);else if(key=="dvd-bottom-margin")st.dvd_bottom_margin_px=integer(0,720);else if(key=="dvd-force")st.dvd_force_display=parse_on_off(value,option);
    else throw std::runtime_error(option+" unknown subtitle style key: "+key);
}

Menu* find_menu_by_id(Menu& root, const std::string& id) {
    if (root.id == id) return &root;
    for (auto& child : root.submenus) if (auto* found = find_menu_by_id(child, id)) return found;
    return nullptr;
}

Menu* find_parent_menu(Menu& root, const std::string& id) {
    for (auto& child : root.submenus) {
        if (child.id == id) return &root;
        if (auto* found = find_parent_menu(child, id)) return found;
    }
    return nullptr;
}

struct ButtonOverride {
    bool initialized = false;
    bool has_label = false;
    bool style_modified = false;
    std::string label;
    ButtonStyle style;
    MenuButtonKind kind = MenuButtonKind::Text;
    std::filesystem::path normal_image;
    std::filesystem::path selected_image;
    Rgba image_highlight_color{128,200,255,112};
    std::optional<int> x, y, width, height;
};

ButtonStateStyle& state(ButtonStyle& s, const std::string& which) {
    if (which == "normal") return s.normal;
    if (which == "active") return s.active;
    if (which == "selected") return s.selected;
    return s.activated;
}

bool apply_style_option(const std::string& a, const std::string& prefix, ButtonStyle& style,
                        const std::function<std::string()>& value) {
    if (a == prefix + "font") style.font_family = value();
    else if (a == prefix + "font-size") style.font_size_px = parse_int(value(), a);
    else if (a == prefix + "bold") style.bold = true;
    else if (a == prefix + "regular") style.bold = false;
    else if (a == prefix + "italic") style.italic = true;
    else if (a == prefix + "roman") style.italic = false;
    else if (a == prefix + "corner-radius") style.corner_radius = parse_int(value(), a);
    else {
        for (const char* n : {"normal", "active", "selected", "activated"}) {
            const std::string base = prefix + n + "-";
            if (a == base + "text") { state(style,n).text_color = parse_color(value(),a); return true; }
            // Active-option artwork is deliberately a transparent overlay so the
            // ordinary selected/activated background remains visible underneath.
            if (a == base + "bg" && std::string_view(n) != "active") { state(style,n).background_color = parse_color(value(),a); return true; }
            if (a == base + "border") { state(style,n).border_color = parse_color(value(),a); return true; }
            if (a == base + "border-width") { state(style,n).border_width = parse_int(value(),a); return true; }
        }
        return false;
    }
    return true;
}
} // namespace

int main(int argc, char** argv) {
    try {
        Project p;
        bool size_target_explicit = false;
        ToolPaths tools;
        EncodingProfile next_title;
        EncodingProfile next_title_uhd = default_uhd_encoding_profile();
        EncodingProfile next_title_dvd = default_dvd_encoding_profile();
        bool next_title_force_reencode = false;
        std::string next_title_resolution = "auto";
        std::string next_title_aspect = "auto";
        std::string next_title_frame_rate = "inherit";
        ChapterMode next_title_chapter_mode = ChapterMode::SourceOrFiveMinute;
        std::vector<double> next_title_chapters;
        double next_title_chapter_interval = 300.0;
        std::map<DiscTarget,int> next_title_audio_bitrate_overrides;
        std::map<int,AudioStreamSettings> next_audio_stream_settings;
        std::map<int,SubtitleStreamSettings> next_subtitle_stream_settings;
        std::vector<ExternalSubtitle> next_external_subtitles;
        int next_default_audio_stream = 0;
        int next_default_subtitle_stream = -1;
        std::map<std::pair<DiscTarget,std::string>,int> menu_audio_bitrate_overrides;
        std::map<std::size_t,ButtonOverride> button_overrides;
        ButtonOverride* current_button = nullptr;
        std::vector<std::string> menu_stack{"top"};
        std::string overlay_menu_id;
        std::optional<std::size_t> overlay_index;
        std::string action_button_menu_id;
        std::optional<std::size_t> action_button_index;
        auto current_menu = [&]() -> Menu& {
            auto* menu = find_menu_by_id(p.menu, menu_stack.back());
            if (!menu) throw std::runtime_error("internal menu selection error");
            return *menu;
        };
        auto select_latest_button = [&]() { action_button_menu_id=current_menu().id;action_button_index=current_menu().buttons.empty()?std::optional<std::size_t>{}:std::optional<std::size_t>{current_menu().buttons.size()-1U}; };
        auto selected_action_button = [&]() -> MenuButton& { if(!action_button_index)throw std::runtime_error("--button-action-* requires a button option first");auto* menu=find_menu_by_id(p.menu,action_button_menu_id);if(!menu||*action_button_index>=menu->buttons.size())throw std::runtime_error("selected button no longer exists");return menu->buttons[*action_button_index]; };
        auto append_button_action = [&](NavigationAction action){auto& b=selected_action_button();if(b.actions.empty())b.actions={legacy_button_action(b)};b.actions.push_back(std::move(action));mirror_first_action_to_legacy_target(b);};
        auto current_title_encoding = [&]() -> EncodingProfile& {
            switch (p.target) {
                case DiscTarget::UltraHdBluRay2160: return next_title_uhd;
                case DiscTarget::DvdVideo480p: return next_title_dvd;
                case DiscTarget::BluRay1080: return next_title;
            }
            return next_title;
        };
        auto pending_audio_stream = [&](int one_based, const std::string& option) -> AudioStreamSettings& {
            if (one_based < 1) throw std::runtime_error(option + " stream number must be >= 1");
            auto [it, inserted] = next_audio_stream_settings.try_emplace(one_based);
            if (inserted) {
                it->second.source_ordinal = one_based - 1;
                it->second.encoding = next_title;
                it->second.uhd_encoding = next_title_uhd;
                it->second.dvd_encoding = next_title_dvd;
            }
            return it->second;
        };
        auto pending_subtitle_stream = [&](int one_based, const std::string& option) -> SubtitleStreamSettings& {
            if (one_based < 1) throw std::runtime_error(option + " stream number must be >= 1");
            auto [it, inserted] = next_subtitle_stream_settings.try_emplace(one_based);
            if (inserted) it->second.source_ordinal = one_based - 1;
            return it->second;
        };
        auto language_override = [&](std::string value, const std::string& option) {
            if (value == "source" || value == "inherit" || value == "auto") return std::string{};
            if (value.size() != 3U || !std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isalpha(c) != 0; }))
                throw std::runtime_error(option + " must be a three-letter ISO 639-2 language code or 'source'");
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return value;
        };
        auto audio_channel_layout = [&](std::string value, const std::string& option) {
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (value == "source" || value == "original" || value == "inherit" || value == "0") return 0;
            if (value == "mono" || value == "1" || value == "1.0") return 1;
            if (value == "stereo" || value == "2" || value == "2.0") return 2;
            if (value == "quad" || value == "4" || value == "4.0") return 4;
            if (value == "5.1" || value == "6") return 6;
            if (value == "7.1" || value == "8") return 8;
            throw std::runtime_error(option + " must be source, mono, stereo, quad, 5.1, or 7.1");
        };
        auto current_menu_encoding = [&]() -> EncodingProfile& {
            return encoding_for_target(current_menu(), p.target);
        };
        auto require_video_codec_for_target = [&](VideoCodec codec, const std::string& option) {
            if (video_codec_allowed_for_target(p.target, codec)) return;
            if (p.target == DiscTarget::UltraHdBluRay2160) throw std::runtime_error(option + " allows hevc or x264/AVC for Ultra HD Blu-ray (AVC is limited to 1080p23.976/24)");
            if (p.target == DiscTarget::DvdVideo480p) throw std::runtime_error(option + " allows only mpeg2 for DVD-Video");
            throw std::runtime_error(option + " allows only x264 or mpeg2 for standard Blu-ray");
        };
        auto require_audio_codec_for_target = [&](AudioCodec codec, const std::string& option) {
            if (audio_codec_allowed_for_target(p.target, codec)) return;
            throw std::runtime_error(option + " allows only ac3 or lpcm for DVD-Video; DTS/DCA is intentionally excluded for player compatibility and TrueHD is not a DVD-Video codec");
        };
        auto require_video_bitrate_for_target = [&](VideoCodec codec, int bitrate, const std::string& option) {
            const int maximum=video_codec_max_bitrate_kbps(p.target,codec);
            if (bitrate < 1000 || bitrate > maximum)
                throw std::runtime_error(option + " must be between 1000 and " + std::to_string(maximum) + " kb/s for the selected target/codec");
        };
        auto require_video_minrate_for_target = [&](VideoCodec codec, int bitrate, const std::string& option) {
            const int maximum=video_codec_max_bitrate_kbps(p.target,codec);
            if (bitrate < 0 || bitrate > maximum)
                throw std::runtime_error(option + " must be between 0 and " + std::to_string(maximum) + " kb/s for the selected target/codec");
        };
        auto require_video_maxrate_for_target = [&](VideoCodec codec, int bitrate, const std::string& option) {
            const int maximum=video_codec_max_bitrate_kbps(p.target,codec);
            if (bitrate < 1 || bitrate > maximum)
                throw std::runtime_error(option + " must be between 1 and " + std::to_string(maximum) + " kb/s for the selected target/codec");
        };
        auto require_audio_bitrate_for_target = [&](AudioCodec codec, int bitrate, const std::string& option) {
            if (codec == AudioCodec::Ac3 && bitrate > target_max_ac3_bitrate_kbps(p.target))
                throw std::runtime_error(option + " exceeds the " + std::to_string(target_max_ac3_bitrate_kbps(p.target)) + " kb/s AC-3 limit for the selected target");
        };
        auto current_overlay = [&]() -> MenuOverlay& {
            if (!overlay_index) throw std::runtime_error("label option requires --text-label or --image-label first");
            auto* menu = find_menu_by_id(p.menu, overlay_menu_id);
            if (!menu || *overlay_index >= menu->overlays.size()) throw std::runtime_error("internal label selection error");
            return menu->overlays[*overlay_index];
        };
        auto next_arg = [&](int& index, const std::string& option) {
            if (index + 1 >= argc) throw std::runtime_error("missing value after " + option);
            return std::string(argv[++index]);
        };

        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            auto value = [&]() {
                if (i + 1 >= argc) throw std::runtime_error("missing value after " + a);
                return std::string(argv[++i]);
            };
            if (a == "-h" || a == "--help") { usage(); return 0; }
            else if (a == "--target") { const auto previous=p.target;p.target=parse_target(value());if(!size_target_explicit&&p.disc_capacity_bytes==default_disc_capacity_bytes(previous))p.disc_capacity_bytes=default_disc_capacity_bytes(p.target); }
            else if (a == "--size-target") { p.disc_capacity_bytes=parse_size_target(value());size_target_explicit=true; }
            else if (a == "--automatic-bitrate") p.automatic_video_bitrate=true;
            else if (a == "--manual-bitrate") p.automatic_video_bitrate=false;
            else if (a == "--frame-rate") p.frame_rate = value();
            else if (a == "--menu-resolution") menu_resolution_for_target(p,p.target) = value();
            else if (a == "--menu-aspect") menu_aspect_ratio_for_target(p,p.target) = value();
            else if (a == "-o") p.output_image = value();
            else if (a == "-V" || a == "--label") p.volume_label = value();
            else if (a == "--menu-background") { auto& m=current_menu(); m.background_image=value(); m.background_video.clear(); m.inherit_background_image=false; }
            else if (a == "--menu-background-video") { auto& m=current_menu(); m.background_video=value(); m.background_image.clear(); m.inherit_background_image=false; }
            else if (a == "--menu-audio") { auto& m=current_menu(); m.audio_source=value(); m.inherit_audio=false; }
            else if (a == "--menu-background-color") { auto& m=current_menu(); m.background_color=parse_color(value(),a); m.inherit_background_color=false; }
            else if (a == "--menu-duration") { auto& m=current_menu(); m.duration_seconds=std::stod(value()); m.inherit_duration=false; }
            else if (a == "--menu-loop") current_menu().loop_media=true;
            else if (a == "--no-menu-loop") current_menu().loop_media=false;
            else if (a == "--no-menu") { p.menu=Menu{};p.menu.id.clear();p.menu.name.clear();p.menu.submenus.clear();action_button_index.reset();overlay_index.reset(); }
            else if (a == "--menu-name") {
                auto& m=current_menu(); m.name=value();
                if (auto* parent=find_parent_menu(p.menu,m.id))
                    for (auto& b:parent->buttons) if (b.auto_submenu_link && b.target_menu_id==m.id) b.label=m.name;
            }
            else if (a == "--submenu") {
                const auto id=value();
                if (find_menu_by_id(p.menu,id)) throw std::runtime_error("duplicate menu id: "+id);
                auto& parent=current_menu();
                MenuButton link;link.label=id;link.target_kind=MenuButtonTargetKind::Menu;link.target_menu_id=id;link.auto_submenu_link=true;NavigationAction link_action;link_action.kind=NavigationActionKind::Menu;link_action.target_menu_id=id;set_single_button_action(link,std::move(link_action));
                link.bounds={1240,600+static_cast<int>(parent.buttons.size())*180,1360,140};
                parent.buttons.push_back(std::move(link));action_button_menu_id=parent.id;action_button_index=parent.buttons.size()-1U;
                Menu child;child.id=id;child.name=id;parent.submenus.push_back(std::move(child));menu_stack.push_back(id);
                overlay_index.reset();
            }
            else if (a == "--end-submenu") { if(menu_stack.size()<=1) throw std::runtime_error("--end-submenu used at top menu"); menu_stack.pop_back(); }
            else if (a == "--no-auto-back") current_menu().auto_back_button=false;
            else if (a == "--auto-back") current_menu().auto_back_button=true;
            else if (a == "--menu-link") { const auto target=value(); const auto label=next_arg(i,a); MenuButton b;b.label=label;b.target_kind=MenuButtonTargetKind::Menu;b.target_menu_id=target;NavigationAction action;action.kind=NavigationActionKind::Menu;action.target_menu_id=target;set_single_button_action(b,std::move(action));b.bounds={1240,600+static_cast<int>(current_menu().buttons.size())*180,1360,140};current_menu().buttons.push_back(std::move(b));select_latest_button(); }
            else if (a == "--title-button") { const int n=parse_int(value(),a); const auto label=next_arg(i,a); MenuButton b;b.label=label;b.target_kind=MenuButtonTargetKind::Title;b.target_title=static_cast<std::uint16_t>(n);NavigationAction action;action.kind=NavigationActionKind::PlayTitle;action.target_title=b.target_title;set_single_button_action(b,std::move(action));b.bounds={1240,600+static_cast<int>(current_menu().buttons.size())*180,1360,140};current_menu().buttons.push_back(std::move(b));select_latest_button(); }
            else if (a == "--title-button-chapter") { const int n=parse_int(value(),a); const int chapter=parse_int(next_arg(i,a),a); const auto label=next_arg(i,a); if(chapter<1||chapter>65535)throw std::runtime_error("--title-button-chapter chapter must be 1..65535"); MenuButton b;b.label=label;b.target_kind=MenuButtonTargetKind::Title;b.target_title=static_cast<std::uint16_t>(n);b.target_chapter=static_cast<std::uint16_t>(chapter);NavigationAction action;action.kind=NavigationActionKind::PlayTitle;action.target_title=b.target_title;action.target_chapter=b.target_chapter;set_single_button_action(b,std::move(action));b.bounds={1240,600+static_cast<int>(current_menu().buttons.size())*180,1360,140};current_menu().buttons.push_back(std::move(b));select_latest_button(); }
            else if (a == "--audio-button") { const int n=parse_int(value(),a); const int stream=parse_int(next_arg(i,a),a); const auto label=next_arg(i,a); if(stream<1||stream>4095)throw std::runtime_error("--audio-button stream must be 1..4095"); MenuButton b;b.label=label;b.target_kind=MenuButtonTargetKind::AudioTrack;b.target_title=static_cast<std::uint16_t>(n);b.target_stream=static_cast<std::uint16_t>(stream);NavigationAction action;action.kind=NavigationActionKind::AudioTrack;action.target_title=b.target_title;action.target_stream=b.target_stream;set_single_button_action(b,std::move(action));b.bounds={1240,600+static_cast<int>(current_menu().buttons.size())*180,1360,140};current_menu().buttons.push_back(std::move(b));select_latest_button(); }
            else if (a == "--subtitle-button") { const int n=parse_int(value(),a); const int stream=parse_int(next_arg(i,a),a); const auto label=next_arg(i,a); if(stream<1||stream>4095)throw std::runtime_error("--subtitle-button stream must be 1..4095"); MenuButton b;b.label=label;b.target_kind=MenuButtonTargetKind::SubtitleTrack;b.target_title=static_cast<std::uint16_t>(n);b.target_stream=static_cast<std::uint16_t>(stream);NavigationAction action;action.kind=NavigationActionKind::SubtitleTrack;action.target_title=b.target_title;action.target_stream=b.target_stream;set_single_button_action(b,std::move(action));b.bounds={1240,600+static_cast<int>(current_menu().buttons.size())*180,1360,140};current_menu().buttons.push_back(std::move(b));select_latest_button(); }
            else if (a == "--subtitles-off-button") { const int n=parse_int(value(),a); const auto label=next_arg(i,a); MenuButton b;b.label=label;b.target_kind=MenuButtonTargetKind::SubtitleOff;b.target_title=static_cast<std::uint16_t>(n);NavigationAction action;action.kind=NavigationActionKind::SubtitleOff;action.target_title=b.target_title;set_single_button_action(b,std::move(action));b.bounds={1240,600+static_cast<int>(current_menu().buttons.size())*180,1360,140};current_menu().buttons.push_back(std::move(b));select_latest_button(); }
            else if (a == "--button-action-title") { NavigationAction action;action.kind=NavigationActionKind::PlayTitle;action.target_title=static_cast<std::uint16_t>(parse_int(value(),a));append_button_action(std::move(action)); }
            else if (a == "--button-action-title-chapter") { NavigationAction action;action.kind=NavigationActionKind::PlayTitle;action.target_title=static_cast<std::uint16_t>(parse_int(value(),a));const int chapter=parse_int(next_arg(i,a),a);if(chapter<1||chapter>65535)throw std::runtime_error("--button-action-title-chapter chapter must be 1..65535");action.target_chapter=static_cast<std::uint16_t>(chapter);append_button_action(std::move(action)); }
            else if (a == "--button-action-menu") { NavigationAction action;action.kind=NavigationActionKind::Menu;action.target_menu_id=value();append_button_action(std::move(action)); }
            else if (a == "--button-action-audio") { NavigationAction action;action.kind=NavigationActionKind::AudioTrack;action.target_title=static_cast<std::uint16_t>(parse_int(value(),a));action.target_stream=static_cast<std::uint16_t>(parse_int(next_arg(i,a),a));append_button_action(std::move(action)); }
            else if (a == "--button-action-subtitle") { NavigationAction action;action.kind=NavigationActionKind::SubtitleTrack;action.target_title=static_cast<std::uint16_t>(parse_int(value(),a));action.target_stream=static_cast<std::uint16_t>(parse_int(next_arg(i,a),a));append_button_action(std::move(action)); }
            else if (a == "--button-action-subtitles-off") { NavigationAction action;action.kind=NavigationActionKind::SubtitleOff;action.target_title=static_cast<std::uint16_t>(parse_int(value(),a));append_button_action(std::move(action)); }
            else if (a == "--button-action-repeat") { const int n=parse_int(value(),a);if(n<0||n>1000)throw std::runtime_error("--button-action-repeat must be 0..1000");auto& b=selected_action_button();auto actions=button_action_sequence(b);if(actions.empty())throw std::runtime_error("--button-action-repeat requires a button action");if(actions.back().kind==NavigationActionKind::RepeatEnd)throw std::runtime_error("--button-action-repeat cannot modify a repeat-group end marker");actions.back().repeat_count=static_cast<std::uint16_t>(n);set_button_action_sequence(b,std::move(actions)); }
            else if (a == "--button-repeat-group-begin") { const int n=parse_int(value(),a);if(n<0||n>1000)throw std::runtime_error("--button-repeat-group-begin must be 0..1000");auto& b=selected_action_button();auto actions=button_action_sequence(b);if(actions.empty())throw std::runtime_error("--button-repeat-group-begin requires a previous button action");NavigationAction marker;marker.kind=NavigationActionKind::RepeatBegin;marker.repeat_count=static_cast<std::uint16_t>(n);actions.insert(actions.end()-1,std::move(marker));set_button_action_sequence(b,std::move(actions)); }
            else if (a == "--button-repeat-group-end") { auto& b=selected_action_button();auto actions=button_action_sequence(b);NavigationAction marker;marker.kind=NavigationActionKind::RepeatEnd;actions.push_back(std::move(marker));set_button_action_sequence(b,std::move(actions)); }
            else if (a == "--first-play-title") { NavigationAction action;action.kind=NavigationActionKind::PlayTitle;action.target_title=static_cast<std::uint16_t>(parse_int(value(),a));p.first_play_actions.push_back(std::move(action)); }
            else if (a == "--first-play-title-chapter") { NavigationAction action;action.kind=NavigationActionKind::PlayTitle;action.target_title=static_cast<std::uint16_t>(parse_int(value(),a));const int chapter=parse_int(next_arg(i,a),a);if(chapter<1||chapter>65535)throw std::runtime_error("--first-play-title-chapter chapter must be 1..65535");action.target_chapter=static_cast<std::uint16_t>(chapter);p.first_play_actions.push_back(std::move(action)); }
            else if (a == "--first-play-menu") { NavigationAction action;action.kind=NavigationActionKind::Menu;action.target_menu_id=value();p.first_play_actions.push_back(std::move(action)); }
            else if (a == "--first-play-repeat") { const int n=parse_int(value(),a);if(n<0||n>1000)throw std::runtime_error("--first-play-repeat must be 0..1000");if(p.first_play_actions.empty())throw std::runtime_error("--first-play-repeat requires a previous First Playback action");if(p.first_play_actions.back().kind==NavigationActionKind::RepeatEnd)throw std::runtime_error("--first-play-repeat cannot modify a repeat-group end marker");p.first_play_actions.back().repeat_count=static_cast<std::uint16_t>(n); }
            else if (a == "--first-play-repeat-group-begin") { const int n=parse_int(value(),a);if(n<0||n>1000)throw std::runtime_error("--first-play-repeat-group-begin must be 0..1000");if(p.first_play_actions.empty())throw std::runtime_error("--first-play-repeat-group-begin requires a previous First Playback action");NavigationAction marker;marker.kind=NavigationActionKind::RepeatBegin;marker.repeat_count=static_cast<std::uint16_t>(n);p.first_play_actions.insert(p.first_play_actions.end()-1,std::move(marker)); }
            else if (a == "--first-play-repeat-group-end") { NavigationAction marker;marker.kind=NavigationActionKind::RepeatEnd;p.first_play_actions.push_back(std::move(marker)); }
            else if (a == "--text-label") { auto& m=current_menu(); MenuOverlay o;o.kind=MenuOverlayKind::Text;o.text=value();o.bounds={320,240+static_cast<int>(m.overlays.size())*200,1440,160};m.overlays.push_back(std::move(o));overlay_menu_id=m.id;overlay_index=m.overlays.size()-1U; }
            else if (a == "--image-label") { auto& m=current_menu(); MenuOverlay o;o.kind=MenuOverlayKind::Image;o.image=value();o.bounds={320,240+static_cast<int>(m.overlays.size())*240,960,200};m.overlays.push_back(std::move(o));overlay_menu_id=m.id;overlay_index=m.overlays.size()-1U; }
            else if (a == "--label-x") current_overlay().bounds.x=parse_int(value(),a);
            else if (a == "--label-y") current_overlay().bounds.y=parse_int(value(),a);
            else if (a == "--label-width") current_overlay().bounds.width=parse_int(value(),a);
            else if (a == "--label-height") current_overlay().bounds.height=parse_int(value(),a);
            else if (a == "--label-font") current_overlay().font_family=value();
            else if (a == "--label-font-size") current_overlay().font_size_px=parse_int(value(),a);
            else if (a == "--label-color") current_overlay().text_color=parse_color(value(),a);
            else if (a == "--label-bold") current_overlay().bold=true;
            else if (a == "--label-regular") current_overlay().bold=false;
            else if (a == "--label-italic") current_overlay().italic=true;
            else if (a == "--label-roman") current_overlay().italic=false;
            else if (a == "--ffmpeg") tools.ffmpeg = value();
            else if (a == "--ffprobe") tools.ffprobe = value();
            else if (a == "--x264") tools.x264 = value();
            else if (a == "--x265") tools.x265 = value();
            else if (a == "--h264-provider") { const auto v=value();if(v=="ffmpeg")tools.h264_provider=VideoEncoderProvider::Ffmpeg;else if(v=="x264")tools.h264_provider=VideoEncoderProvider::Standalone;else throw std::runtime_error("--h264-provider must be ffmpeg or x264"); }
            else if (a == "--hevc-provider") { const auto v=value();if(v=="ffmpeg")tools.hevc_provider=VideoEncoderProvider::Ffmpeg;else if(v=="x265")tools.hevc_provider=VideoEncoderProvider::Standalone;else throw std::runtime_error("--hevc-provider must be ffmpeg or x265"); }
            else if (a == "--tsmuxer") tools.tsmuxer = value();
            else if (a == "--dvdauthor") tools.dvdauthor = value();
            else if (a == "--spumux") tools.spumux = value();
            else if (a == "--mplex") tools.mplex = value();
            else if (a == "--mkisofs") tools.mkisofs = value();
            else if (a == "--cache") { p.use_encode_cache = true; p.use_compliance_cache = true; }
            else if (a == "--no-cache") { p.use_encode_cache = false; p.use_compliance_cache = false; }
            else if (a == "--encode-cache") p.use_encode_cache = true;
            else if (a == "--no-encode-cache") p.use_encode_cache = false;
            else if (a == "--compliance-cache") p.use_compliance_cache = true;
            else if (a == "--no-compliance-cache") p.use_compliance_cache = false;
            else if (a == "--refresh-encode-cache") p.refresh_encode_cache = true;
            else if (a == "--refresh-compliance-cache") p.refresh_compliance_cache = true;
            else if (a == "--force-reencode-all") p.force_reencode = true;
            else if (a == "--force-reencode") next_title_force_reencode = true;
            else if (a == "--allow-passthrough") next_title_force_reencode = false;
            else if (a == "--title-resolution") next_title_resolution = value();
            else if (a == "--title-aspect") next_title_aspect = value();
            else if (a == "--title-frame-rate") next_title_frame_rate = value();
            else if (a == "--keyframe-interval") { const int n=parse_int(value(),a);if(n<0)throw std::runtime_error("--keyframe-interval must be >= 0");current_title_encoding().keyframe_interval_frames=n; }
            else if (a == "--video-option") { auto& e=current_title_encoding();append_advanced_codec_option(advanced_video_options_for_codec(e),value());validate_advanced_video_options(e,p.target); }
            else if (a == "--audio-sample-rate") { auto v=value();auto& e=current_title_encoding();if(!lossless_audio_codec(e.audio_codec))throw std::runtime_error(a+" requires LPCM or TrueHD");e.lpcm_sample_rate_hz=(v=="auto"?0:parse_int(v,a));if(!lossless_sample_rate_allowed_for_target(p.target,e.lpcm_sample_rate_hz))throw std::runtime_error(a+" sample rate is not legal for the selected target"); }
            else if (a == "--audio-bit-depth") { auto v=value();auto& e=current_title_encoding();if(!lossless_audio_codec(e.audio_codec))throw std::runtime_error(a+" requires LPCM or TrueHD");e.lpcm_bit_depth=(v=="auto"?0:parse_int(v,a));if(!lossless_bit_depth_supported_by_encoder(e.lpcm_bit_depth))throw std::runtime_error(a+" supports auto, 16 or 24"); }
            else if (a == "--audio-option") { auto& e=current_title_encoding();append_advanced_codec_option(advanced_audio_options_for_codec(e),value());validate_advanced_audio_options(e,p.target); }
            else if (a == "--clear-video-options") { advanced_video_options_for_codec(current_title_encoding()).clear(); }
            else if (a == "--clear-audio-options") { advanced_audio_options_for_codec(current_title_encoding()).clear(); }
            else if (a == "--audio-stream-codec") { const int stream=parse_int(value(),a);auto& setting=pending_audio_stream(stream,a);auto& e=encoding_for_target(setting,p.target);const auto codec=parse_audio_codec(next_arg(i,a));require_audio_codec_for_target(codec,a);e.audio_codec=codec;setting.override_encoding=true; }
            else if (a == "--audio-stream-bitrate") { const int stream=parse_int(value(),a);const int bitrate=parse_int(next_arg(i,a),a);auto& setting=pending_audio_stream(stream,a);auto& e=encoding_for_target(setting,p.target);if(!audio_codec_has_configurable_bitrate(e.audio_codec))throw std::runtime_error(a+" cannot directly set bitrate for the selected stream codec");require_audio_bitrate_for_target(e.audio_codec,bitrate,a);set_audio_bitrate_kbps(e,bitrate);setting.override_encoding=true; }
            else if (a == "--audio-stream-sample-rate") { const int stream=parse_int(value(),a);const auto token=next_arg(i,a);auto& setting=pending_audio_stream(stream,a);auto& e=encoding_for_target(setting,p.target);if(!lossless_audio_codec(e.audio_codec))throw std::runtime_error(a+" requires the stream codec to be LPCM or TrueHD");e.lpcm_sample_rate_hz=(token=="auto"?0:parse_int(token,a));if(!lossless_sample_rate_allowed_for_target(p.target,e.lpcm_sample_rate_hz))throw std::runtime_error(a+" sample rate is not legal for the selected target");setting.override_encoding=true; }
            else if (a == "--audio-stream-bit-depth") { const int stream=parse_int(value(),a);const auto token=next_arg(i,a);auto& setting=pending_audio_stream(stream,a);auto& e=encoding_for_target(setting,p.target);if(!lossless_audio_codec(e.audio_codec))throw std::runtime_error(a+" requires the stream codec to be LPCM or TrueHD");e.lpcm_bit_depth=(token=="auto"?0:parse_int(token,a));if(!lossless_bit_depth_supported_by_encoder(e.lpcm_bit_depth))throw std::runtime_error(a+" supports auto, 16 or 24");setting.override_encoding=true; }
            else if (a == "--audio-stream-lpcm-rate") { const int stream=parse_int(value(),a);const int rate=parse_int(next_arg(i,a),a);if(!lpcm_sample_rate_allowed_for_target(p.target,rate))throw std::runtime_error(a+" sample rate is not legal for the selected target");auto& setting=pending_audio_stream(stream,a);auto& e=encoding_for_target(setting,p.target);e.audio_codec=AudioCodec::Lpcm;e.lpcm_sample_rate_hz=rate;setting.override_encoding=true; }
            else if (a == "--audio-stream-lpcm-depth") { const int stream=parse_int(value(),a);const int bits=parse_int(next_arg(i,a),a);if(!lpcm_bit_depth_supported_by_encoder(bits))throw std::runtime_error(a+" supports 16 or 24 bits");auto& setting=pending_audio_stream(stream,a);auto& e=encoding_for_target(setting,p.target);e.audio_codec=AudioCodec::Lpcm;e.lpcm_bit_depth=bits;setting.override_encoding=true; }
            else if (a == "--audio-stream-option") { const int stream=parse_int(value(),a);auto& setting=pending_audio_stream(stream,a);auto& e=encoding_for_target(setting,p.target);append_advanced_codec_option(advanced_audio_options_for_codec(e),next_arg(i,a));validate_advanced_audio_options(e,p.target);setting.override_encoding=true; }
            else if (a == "--audio-stream-clear-options") { const int stream=parse_int(value(),a);auto& setting=pending_audio_stream(stream,a);auto& e=encoding_for_target(setting,p.target);advanced_audio_options_for_codec(e).clear();setting.override_encoding=true; }
            else if (a == "--audio-stream-default") { const int stream=parse_int(value(),a);pending_audio_stream(stream,a).override_encoding=false; }
            else if (a == "--audio-stream-language") { const int stream=parse_int(value(),a);pending_audio_stream(stream,a).language=language_override(next_arg(i,a),a); }
            else if (a == "--audio-stream-channels") { const int stream=parse_int(value(),a);pending_audio_stream(stream,a).output_channels=audio_channel_layout(next_arg(i,a),a); }
            else if (a == "--subtitle-stream-language") { const int stream=parse_int(value(),a);pending_subtitle_stream(stream,a).language=language_override(next_arg(i,a),a); }
            else if (a == "--default-audio-stream") { const auto mode=value();if(mode=="player")next_default_audio_stream=0;else{next_default_audio_stream=parse_int(mode,a);if(next_default_audio_stream<1)throw std::runtime_error(a+" expects player or a 1-based stream number");} }
            else if (a == "--default-subtitle-stream") { const auto mode=value();if(mode=="player")next_default_subtitle_stream=-1;else if(mode=="off")next_default_subtitle_stream=0;else{next_default_subtitle_stream=parse_int(mode,a);if(next_default_subtitle_stream<1)throw std::runtime_error(a+" expects player, off, or a 1-based stream number");} }
            else if (a == "--subtitle-stream-style") { const int stream=parse_int(value(),a);apply_subtitle_style_option(pending_subtitle_stream(stream,a).style,next_arg(i,a),a); }
            else if (a == "--subtitle-stream-clear-style") { const int stream=parse_int(value(),a);pending_subtitle_stream(stream,a).style=SubtitleStyle{}; }
            else if (a == "--subtitle-file") { ExternalSubtitle sub;sub.source=value();sub.language="und";next_external_subtitles.push_back(std::move(sub)); }
            else if (a == "--subtitle-file-language") { if(next_external_subtitles.empty())throw std::runtime_error(a+" requires --subtitle-file first");next_external_subtitles.back().language=language_override(value(),a);if(next_external_subtitles.back().language.empty())next_external_subtitles.back().language="und"; }
            else if (a == "--subtitle-file-style") { if(next_external_subtitles.empty())throw std::runtime_error(a+" requires --subtitle-file first");apply_subtitle_style_option(next_external_subtitles.back().style,value(),a); }
            else if (a == "--chapters-source") { next_title_chapter_mode=ChapterMode::SourceOrFiveMinute;next_title_chapters.clear();next_title_chapter_interval=300.0; }
            else if (a == "--chapters-manual") { next_title_chapter_mode=ChapterMode::Manual;next_title_chapters=parse_chapter_list(value(),a); }
            else if (a == "--chapters-every") { next_title_chapter_mode=ChapterMode::Interval;next_title_chapter_interval=parse_duration_seconds(value(),a);if(next_title_chapter_interval<=0.0)throw std::runtime_error("--chapters-every must be greater than zero");next_title_chapters.clear(); }
            else if (a == "--no-chapters") { next_title_chapter_mode=ChapterMode::None;next_title_chapters.clear(); }
            else if (a == "--video-codec") { const auto codec=parse_video_codec(value());require_video_codec_for_target(codec,a);current_title_encoding().video_codec=codec; }
            else if (a == "--video-bitrate") { const int bitrate=parse_int(value(),a);require_video_bitrate_for_target(current_title_encoding().video_codec,bitrate,a);current_title_encoding().video_bitrate_kbps=bitrate;p.automatic_video_bitrate=false; }
            else if (a == "--video-minrate") { const int bitrate=parse_int(value(),a);require_video_minrate_for_target(current_title_encoding().video_codec,bitrate,a);current_title_encoding().video_min_bitrate_kbps=bitrate; }
            else if (a == "--video-maxrate") { const int bitrate=parse_int(value(),a);require_video_maxrate_for_target(current_title_encoding().video_codec,bitrate,a);current_title_encoding().video_max_bitrate_kbps=bitrate; }
            else if (a == "--x264-preset") current_title_encoding().x264_preset = value();
            else if (a == "--x265-preset") current_title_encoding().x265_preset = value();
            else if (a == "--two-pass") current_title_encoding().two_pass = true;
            else if (a == "--single-pass") current_title_encoding().two_pass = false;
            else if (a == "--audio-codec") { const auto codec=parse_audio_codec(value());require_audio_codec_for_target(codec,a);current_title_encoding().audio_codec=codec;if(const auto it=next_title_audio_bitrate_overrides.find(p.target);it!=next_title_audio_bitrate_overrides.end()&&audio_codec_has_configurable_bitrate(codec)){require_audio_bitrate_for_target(codec,it->second,a);set_audio_bitrate_kbps(current_title_encoding(),it->second);} }
            else if (a == "--audio-bitrate") { const int bitrate=parse_int(value(),a);next_title_audio_bitrate_overrides[p.target]=bitrate;if(audio_codec_has_configurable_bitrate(current_title_encoding().audio_codec)){require_audio_bitrate_for_target(current_title_encoding().audio_codec,bitrate,a);set_audio_bitrate_kbps(current_title_encoding(),bitrate);} }
            else if (a == "--menu-video-codec") { auto& m=current_menu();const auto codec=parse_video_codec(value());require_video_codec_for_target(codec,a);current_menu_encoding().video_codec=codec;m.inherit_encoding=false; }
            else if (a == "--menu-video-bitrate") { auto& m=current_menu();const int bitrate=parse_int(value(),a);require_video_bitrate_for_target(current_menu_encoding().video_codec,bitrate,a);current_menu_encoding().video_bitrate_kbps=bitrate;m.inherit_encoding=false;p.automatic_video_bitrate=false; }
            else if (a == "--menu-video-minrate") { auto& m=current_menu();const int bitrate=parse_int(value(),a);require_video_minrate_for_target(current_menu_encoding().video_codec,bitrate,a);current_menu_encoding().video_min_bitrate_kbps=bitrate;m.inherit_encoding=false; }
            else if (a == "--menu-video-maxrate") { auto& m=current_menu();const int bitrate=parse_int(value(),a);require_video_maxrate_for_target(current_menu_encoding().video_codec,bitrate,a);current_menu_encoding().video_max_bitrate_kbps=bitrate;m.inherit_encoding=false; }
            else if (a == "--menu-x264-preset") { auto& m=current_menu(); current_menu_encoding().x264_preset=value(); m.inherit_encoding=false; }
            else if (a == "--menu-x265-preset") { auto& m=current_menu(); current_menu_encoding().x265_preset=value(); m.inherit_encoding=false; }
            else if (a == "--menu-two-pass") { auto& m=current_menu(); current_menu_encoding().two_pass=true; m.inherit_encoding=false; }
            else if (a == "--menu-single-pass") { auto& m=current_menu(); current_menu_encoding().two_pass=false; m.inherit_encoding=false; }
            else if (a == "--menu-keyframe-interval") { auto& m=current_menu();const int n=parse_int(value(),a);if(n<0)throw std::runtime_error("--menu-keyframe-interval must be >= 0");current_menu_encoding().keyframe_interval_frames=n;m.inherit_encoding=false; }
            else if (a == "--menu-video-option") { auto& m=current_menu();auto& e=current_menu_encoding();append_advanced_codec_option(advanced_video_options_for_codec(e),value());validate_advanced_video_options(e,p.target);m.inherit_encoding=false; }
            else if (a == "--menu-audio-sample-rate") { auto& m=current_menu();auto& e=current_menu_encoding();auto v=value();if(!lossless_audio_codec(e.audio_codec))throw std::runtime_error(a+" requires LPCM or TrueHD");e.lpcm_sample_rate_hz=(v=="auto"?0:parse_int(v,a));if(!lossless_sample_rate_allowed_for_target(p.target,e.lpcm_sample_rate_hz))throw std::runtime_error(a+" sample rate is not legal for the selected target");m.inherit_encoding=false; }
            else if (a == "--menu-audio-bit-depth") { auto& m=current_menu();auto& e=current_menu_encoding();auto v=value();if(!lossless_audio_codec(e.audio_codec))throw std::runtime_error(a+" requires LPCM or TrueHD");e.lpcm_bit_depth=(v=="auto"?0:parse_int(v,a));if(!lossless_bit_depth_supported_by_encoder(e.lpcm_bit_depth))throw std::runtime_error(a+" supports auto, 16 or 24");m.inherit_encoding=false; }
            else if (a == "--menu-audio-option") { auto& m=current_menu();auto& e=current_menu_encoding();append_advanced_codec_option(advanced_audio_options_for_codec(e),value());validate_advanced_audio_options(e,p.target);m.inherit_encoding=false; }
            else if (a == "--menu-clear-video-options") { auto& m=current_menu();advanced_video_options_for_codec(current_menu_encoding()).clear();m.inherit_encoding=false; }
            else if (a == "--menu-clear-audio-options") { auto& m=current_menu();advanced_audio_options_for_codec(current_menu_encoding()).clear();m.inherit_encoding=false; }
            else if (a == "--menu-audio-codec") { auto& m=current_menu();const auto codec=parse_audio_codec(value());require_audio_codec_for_target(codec,a);current_menu_encoding().audio_codec=codec;const auto key=std::make_pair(p.target,m.id);if(const auto it=menu_audio_bitrate_overrides.find(key);it!=menu_audio_bitrate_overrides.end()&&audio_codec_has_configurable_bitrate(codec)){require_audio_bitrate_for_target(codec,it->second,a);set_audio_bitrate_kbps(current_menu_encoding(),it->second);}m.inherit_encoding=false; }
            else if (a == "--menu-audio-bitrate") { auto& m=current_menu();const int bitrate=parse_int(value(),a);menu_audio_bitrate_overrides[std::make_pair(p.target,m.id)]=bitrate;if(audio_codec_has_configurable_bitrate(current_menu_encoding().audio_codec)){require_audio_bitrate_for_target(current_menu_encoding().audio_codec,bitrate,a);set_audio_bitrate_kbps(current_menu_encoding(),bitrate);}m.inherit_encoding=false; }
            else if (a == "--button-style") {
                const int n = parse_int(value(), a);
                if (n < 1) throw std::runtime_error("--button-style index must be >= 1");
                auto& o = button_overrides[static_cast<std::size_t>(n)];
                if (!o.initialized) { o.style = p.menu.default_button_style; o.initialized = true; }
                current_button = &o;
            } else if (a == "--button-label") {
                if (!current_button) throw std::runtime_error("--button-label requires --button-style N first");
                current_button->label = value(); current_button->has_label = true;
            } else if (a == "--button-x" || a == "--button-y" || a == "--button-width" || a == "--button-height") {
                if (!current_button) throw std::runtime_error(a + " requires --button-style N first");
                const int n = parse_int(value(), a);
                if (a == "--button-x") current_button->x = n;
                else if (a == "--button-y") current_button->y = n;
                else if (a == "--button-width") current_button->width = n;
                else current_button->height = n;
            } else if (a == "--button-image") {
                if (!current_button) throw std::runtime_error("--button-image requires --button-style N first");
                current_button->kind = MenuButtonKind::Image; current_button->normal_image = value();
            } else if (a == "--button-selected-image") {
                if (!current_button) throw std::runtime_error("--button-selected-image requires --button-style N first");
                current_button->kind = MenuButtonKind::Image; current_button->selected_image = value();
            } else if (a == "--button-highlight") {
                if (!current_button) throw std::runtime_error("--button-highlight requires --button-style N first");
                current_button->image_highlight_color = parse_color(value(),a);
            } else if (a == "--button-text") {
                if (!current_button) throw std::runtime_error("--button-text requires --button-style N first");
                current_button->kind = MenuButtonKind::Text; current_button->normal_image.clear(); current_button->selected_image.clear();
            } else if (a.rfind("--menu-button-",0) == 0) {
                current_menu().inherit_button_style=false;
                if (!apply_style_option(a,"--menu-button-",current_menu().default_button_style,value))
                    throw std::runtime_error("unknown menu button style option: " + a);
            } else if (a.rfind("--button-",0) == 0) {
                if (!current_button) throw std::runtime_error(a + " requires --button-style N first");
                if (!apply_style_option(a,"--button-",current_button->style,value))
                    throw std::runtime_error("unknown button style option: " + a);
                current_button->style_modified = true;
            } else if (a == "--keep-work") p.keep_work_directory = true;
            else if (a == "--work") { p.work_directory = value(); p.keep_work_directory = true; }
            else if (!a.empty() && a[0] == '-') throw std::runtime_error("unknown option: " + a);
            else {
                Title t; t.source = a; t.name = std::filesystem::path(a).stem().string(); t.encoding = next_title; t.uhd_encoding = next_title_uhd; t.dvd_encoding = next_title_dvd;
                t.force_reencode = next_title_force_reencode;
                resolution_for_target(t, p.target) = next_title_resolution;
                aspect_ratio_for_target(t, p.target) = next_title_aspect;
                frame_rate_for_target(t, p.target) = next_title_frame_rate;
                t.chapter_mode = next_title_chapter_mode; t.chapters_seconds = next_title_chapters;
                t.chapter_interval_seconds = next_title_chapter_interval;
                for (const auto& [stream, setting] : next_audio_stream_settings) { (void)stream; if (setting.override_encoding || setting.output_channels > 0 || !setting.language.empty()) t.audio_stream_settings.push_back(setting); }
                for (const auto& [stream, setting] : next_subtitle_stream_settings) { (void)stream; if (!setting.language.empty()) t.subtitle_stream_settings.push_back(setting); }
                t.external_subtitles = next_external_subtitles;
                t.default_audio_stream = next_default_audio_stream;
                t.default_subtitle_stream = next_default_subtitle_stream;
                p.titles.push_back(std::move(t));
            }
        }

        if (p.titles.empty()) { usage(); return 2; }
        for (const auto& [n,o] : button_overrides)
            if (n > p.titles.size()) throw std::runtime_error("button style index exceeds number of titles: " + std::to_string(n));
        for (std::size_t i = 0; i < p.titles.size(); ++i) {
            MenuButton b;
            b.label = p.titles[i].name;
            b.target_title = static_cast<std::uint16_t>(i + 1);
            NavigationAction action;action.kind=NavigationActionKind::PlayTitle;action.target_title=b.target_title;set_single_button_action(b,std::move(action));
            b.bounds = {1240, 600 + static_cast<int>(i) * 180, 1360, 140};
            const auto it = button_overrides.find(i + 1U);
            if (it != button_overrides.end()) {
                b.use_custom_style = it->second.style_modified;
                if (b.use_custom_style) b.style = it->second.style;
                b.kind = it->second.kind;
                b.normal_image = it->second.normal_image;
                b.selected_image = it->second.selected_image;
                b.image_highlight_color = it->second.image_highlight_color;
                if (it->second.has_label) b.label = it->second.label;
                if (it->second.x) b.bounds.x = *it->second.x;
                if (it->second.y) b.bounds.y = *it->second.y;
                if (it->second.width) b.bounds.width = *it->second.width;
                if (it->second.height) b.bounds.height = *it->second.height;
            }
            if (project_has_menu(p)) p.menu.buttons.push_back(std::move(b));
        }

        if (project_has_menu(p) && p.target == DiscTarget::UltraHdBluRay2160 && p.menu_uhd_resolution == "3840x2160") {
            std::cerr << "bdmvauthor: warning: 3840x2160 UHD menus trigger a known VLC Blu-ray mouse-coordinate bug; "
                         "mouse hit areas may appear up-left of the visible buttons. Arrow-key navigation and compliant hardware players are unaffected. "
                         "Use --menu-resolution 1920x1080 for VLC compatibility.\n";
        }

        AuthorEngine e(tools);
        e.author(p, [&](double pct, const std::string& m) {
            std::ostringstream line;
            line << "[" << std::fixed << std::setprecision(2) << pct << "%] " << m;
            if (m.rfind("NONCOMPLIANT:", 0) == 0) {
                std::cerr << "\r" << std::string(160, ' ') << "\r" << line.str() << "\n" << std::flush;
                return;
            }
            std::cerr << "\r" << std::left << std::setw(140) << line.str() << std::flush;
            if (pct >= 100.0) std::cerr << "\n";
        });
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "bdmvauthor: " << e.what() << "\n";
        return 1;
    }
}
