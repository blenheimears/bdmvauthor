// SPDX-License-Identifier: Apache-2.0
#include "bdmvauthor/hdmv.hpp"
#include "bdmvauthor/font_renderer.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <map>
#include <optional>
#include <stdexcept>
#include <tuple>

namespace bdmvauthor::hdmv {
namespace {
using Bytes = std::vector<std::uint8_t>;
void u16(Bytes& b, std::uint16_t v){ b.push_back(static_cast<std::uint8_t>(v>>8)); b.push_back(static_cast<std::uint8_t>(v)); }
void u24(Bytes& b, std::uint32_t v){ b.push_back(static_cast<std::uint8_t>(v>>16)); b.push_back(static_cast<std::uint8_t>(v>>8)); b.push_back(static_cast<std::uint8_t>(v)); }
void u32(Bytes& b, std::uint32_t v){ b.push_back(static_cast<std::uint8_t>(v>>24)); b.push_back(static_cast<std::uint8_t>(v>>16)); b.push_back(static_cast<std::uint8_t>(v>>8)); b.push_back(static_cast<std::uint8_t>(v)); }
void append(Bytes& a, const Bytes& b){ a.insert(a.end(), b.begin(), b.end()); }
void zeros(Bytes& b, std::size_t n){ b.insert(b.end(), n, 0); }

NavCommand nav(std::uint16_t mnemonic, unsigned operands, bool imm1, bool imm2,
               std::uint32_t a, std::uint32_t c) {
    const unsigned grp=(mnemonic>>8)&3, sub=(mnemonic>>5)&7, opt=mnemonic&31;
    const std::uint8_t b0=static_cast<std::uint8_t>((operands<<5)|(grp<<3)|sub);
    const std::uint8_t b1=static_cast<std::uint8_t>((imm1?0x80:0)|(imm2?0x40:0)|(grp==0?opt:0));
    const std::uint8_t b2=static_cast<std::uint8_t>(grp==1?opt:0);
    const std::uint8_t b3=static_cast<std::uint8_t>(grp==2?opt:0);
    return {std::uint32_t(b0)<<24|std::uint32_t(b1)<<16|std::uint32_t(b2)<<8|b3,a,c};
}
void cmd(Bytes& b,const NavCommand& c){u32(b,c.opcode);u32(b,c.operand1);u32(b,c.operand2);}

Bytes index_entry(std::uint16_t mobj, bool interactive) {
    Bytes b;
    b.push_back(0x40); // object_type=HDMV (01), access_type=00
    zeros(b,3);
    b.push_back(interactive ? 0x40 : 0x00); // playback_type=interactive/movie
    b.push_back(0);
    u16(b,mobj);
    zeros(b,4);
    return b;
}

struct PaletteBuilder {
    std::vector<Rgba> colors{{0,0,0,0}};

    std::uint8_t index(const Rgba& requested) {
        const Rgba c = requested.a == 0 ? Rgba{0,0,0,0} : requested;
        const auto it = std::find(colors.begin(), colors.end(), c);
        if (it != colors.end()) return static_cast<std::uint8_t>(std::distance(colors.begin(), it));
        if (colors.size() >= 256) throw std::runtime_error("menu button styles require more than 256 IG palette colors");
        colors.push_back(c);
        return static_cast<std::uint8_t>(colors.size() - 1U);
    }
};

const ButtonStyle& button_style(const Menu& menu, const MenuButton& button) {
    return button.use_custom_style ? button.style : menu.default_button_style;
}

const ButtonStateStyle& state_style(const ButtonStyle& style, int state) {
    if (state == 1) return style.selected;
    if (state == 2) return style.activated;
    return style.normal;
}

bool rounded_inside(int x, int y, int w, int h, int radius) {
    radius = std::clamp(radius, 0, std::min(w, h) / 2);
    if (radius <= 0) return true;
    if ((x >= radius && x < w - radius) || (y >= radius && y < h - radius)) return true;
    const int cx = x < radius ? radius - 1 : w - radius;
    const int cy = y < radius ? radius - 1 : h - radius;
    const int dx = x - cx, dy = y - cy;
    return dx * dx + dy * dy <= radius * radius;
}

std::vector<Rgba> read_rgba(const std::filesystem::path& path, int w, int h) {
    if (path.empty()) throw std::runtime_error("image-only menu button has no normalized image");
    const auto pixels = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    const auto expected = pixels * 4U;
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot read normalized menu button image: " + path.string());
    std::vector<std::uint8_t> raw(expected);
    f.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
    if (f.gcount() != static_cast<std::streamsize>(expected) || f.peek() != std::char_traits<char>::eof())
        throw std::runtime_error("normalized menu button image has unexpected dimensions: " + path.string());
    std::vector<Rgba> out(pixels);
    for (std::size_t i = 0; i < pixels; ++i)
        out[i] = {raw[i*4U], raw[i*4U+1U], raw[i*4U+2U], raw[i*4U+3U]};
    return out;
}

std::uint8_t quantize_channel(std::uint8_t v) {
    const unsigned level = (static_cast<unsigned>(v) * 4U + 127U) / 255U;
    return static_cast<std::uint8_t>((level * 255U + 2U) / 4U);
}

Rgba quantize_image_color(Rgba c) {
    if (c.a < 128U) return {0,0,0,0};
    return {quantize_channel(c.r), quantize_channel(c.g), quantize_channel(c.b), 255};
}

Rgba highlighted(Rgba src, const Rgba& tint) {
    if (src.a == 0U) return src;
    const unsigned a = tint.a;
    const unsigned inv = 255U - a;
    auto mix = [&](std::uint8_t s, std::uint8_t t) {
        return static_cast<std::uint8_t>((static_cast<unsigned>(s) * inv + static_cast<unsigned>(t) * a + 127U) / 255U);
    };
    return {mix(src.r,tint.r), mix(src.g,tint.g), mix(src.b,tint.b), src.a};
}

Bytes image_bitmap(const MenuButton& button, int state, PaletteBuilder& palette, int w, int h) {
    const bool selected = state != 0;
    const auto& source = selected && !button.selected_image.empty() ? button.selected_image : button.normal_image;
    auto rgba_pixels = read_rgba(source, w, h);
    const bool synthesize_selected = selected && button.selected_image.empty();
    Bytes out(rgba_pixels.size(), 0);
    for (std::size_t i = 0; i < rgba_pixels.size(); ++i) {
        auto c = rgba_pixels[i];
        if (synthesize_selected) c = highlighted(c, button.image_highlight_color);
        out[i] = palette.index(quantize_image_color(c));
    }
    return out;
}

Bytes bitmap(const Menu& menu, const MenuButton& button, int state, PaletteBuilder& palette) {
    const int w = std::max(16, button.bounds.width);
    const int h = std::max(12, button.bounds.height);
    if (button.kind == MenuButtonKind::Image) return image_bitmap(button,state,palette,w,h);

    const auto width = static_cast<std::size_t>(w);
    const auto height = static_cast<std::size_t>(h);
    std::vector<std::uint8_t> p(width * height, 0);
    const auto& style = button_style(menu, button);
    const auto& visual = state_style(style, state);
    const auto bg = palette.index(visual.background_color);
    const auto border_color = palette.index(visual.border_color);
    const auto text = palette.index(visual.text_color);
    const int border = std::clamp(visual.border_width, 0, std::min(w, h) / 2);
    const int radius = std::clamp(style.corner_radius, 0, std::min(w, h) / 2);
    const auto pixel_index = [width](int x, int y) {
        return static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x);
    };

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (!rounded_inside(x, y, w, h, radius)) continue;
            bool is_border = false;
            if (border > 0) {
                if (x < border || y < border || x >= w - border || y >= h - border) is_border = true;
                else if (radius > border && !rounded_inside(x - border, y - border,
                                                            w - 2 * border, h - 2 * border,
                                                            radius - border)) is_border = true;
            }
            p[pixel_index(x, y)] = is_border ? border_color : bg;
        }
    }

    const int padding = std::max(3, border + 4);
    const auto mask = detail::render_button_text_mask(button.label, style, w, h, padding);
    if (mask.width == w && mask.height == h && mask.pixels.size() == p.size()) {
        for (std::size_t i = 0; i < p.size(); ++i) if (mask.pixels[i]) p[i] = text;
    }
    return p;
}

std::uint8_t studio(double v) {
    return static_cast<std::uint8_t>(std::clamp(std::lround(v), 0L, 255L));
}

void append_palette_entry(Bytes& pal, std::uint8_t id, const Rgba& c) {
    // HDMV palettes use Y, Cr, Cb, alpha. Rec.709 coefficients are appropriate
    // for the 1920x1080 Blu-ray Interactive Graphics plane.
    const double r=c.r, g=c.g, b=c.b;
    const auto y  = studio(16.0  + 0.182586*r + 0.614231*g + 0.062007*b);
    const auto cb = studio(128.0 - 0.100644*r - 0.338572*g + 0.439216*b);
    const auto cr = studio(128.0 + 0.439216*r - 0.398942*g - 0.040274*b);
    pal.push_back(id); pal.push_back(y); pal.push_back(cr); pal.push_back(cb); pal.push_back(c.a);
}

void emit_run(Bytes& out,std::uint8_t color,unsigned len){
    while(len){ unsigned n=std::min(len,16383u); if(color && n==1){out.push_back(color);} else if(color){out.push_back(0); if(n<64){out.push_back(static_cast<std::uint8_t>(0x80U|n));}else{out.push_back(static_cast<std::uint8_t>(0xC0U|((n>>8)&0x3fU)));out.push_back(static_cast<std::uint8_t>(n));} out.push_back(color);} else {out.push_back(0); if(n<64)out.push_back(static_cast<std::uint8_t>(n)); else {out.push_back(static_cast<std::uint8_t>(0x40U|((n>>8)&0x3fU)));out.push_back(static_cast<std::uint8_t>(n));}} len-=n; }
}
Bytes rle(const Bytes& pix, int w, int h) {
    const auto width = static_cast<std::size_t>(w);
    const auto height = static_cast<std::size_t>(h);
    Bytes out;
    for (std::size_t y = 0; y < height; ++y) {
        std::size_t x = 0;
        while (x < width) {
            const auto color = pix[y * width + x];
            std::size_t run = 1;
            while (x + run < width && pix[y * width + x + run] == color && run < 16383U)
                ++run;
            emit_run(out, color, static_cast<unsigned>(run));
            x += run;
        }
        out.push_back(0);
        out.push_back(0);
    }
    return out;
}

IgsSegment make_ods(std::uint16_t id,const Menu& menu,const MenuButton& b,int state,PaletteBuilder& palette){
    int w=std::max(16,b.bounds.width),h=std::max(12,b.bounds.height); auto px=bitmap(menu,b,state,palette); auto rr=rle(px,w,h); Bytes p;u16(p,id);p.push_back(0);p.push_back(0xC0);u24(p,static_cast<std::uint32_t>(rr.size()+4));u16(p,static_cast<std::uint16_t>(w));u16(p,static_cast<std::uint16_t>(h));append(p,rr);return {0x15,kMenuIgsPts90k,kMenuIgsPts90k,std::move(p)};
}

struct ActiveIndicatorGeometry { Rect bounds{}; Bytes pixels; };
Rect active_indicator_bounds(const Menu& menu,std::span<const std::size_t> button_indices){
    bool have=false;int left=menu.width,top=menu.height,right=0,bottom=0;
    for(const auto index:button_indices){
        if(index>=menu.buttons.size())continue;
        const auto& b=menu.buttons[index];const auto& active=button_style(menu,b).active;const int border=std::clamp(active.border_width,0,128);
        const bool draw_border=border>0&&active.border_color.a!=0;const bool draw_text=b.kind==MenuButtonKind::Text&&active.text_color.a!=0;if(!draw_border&&!draw_text)continue;
        const int margin=draw_border?border:0;left=std::min(left,std::max(0,b.bounds.x-margin));top=std::min(top,std::max(0,b.bounds.y-margin));right=std::max(right,std::min(menu.width,b.bounds.x+b.bounds.width+margin));bottom=std::max(bottom,std::min(menu.height,b.bounds.y+b.bounds.height+margin));have=true;
    }
    if(!have)return {0,0,16,12};
    return {left,top,std::max(1,right-left),std::max(1,bottom-top)};
}
ActiveIndicatorGeometry active_indicator_geometry(const Menu& menu,std::span<const std::size_t> button_indices,PaletteBuilder& palette,const Rect* fixed_bounds=nullptr){
    ActiveIndicatorGeometry out;out.bounds=fixed_bounds?*fixed_bounds:active_indicator_bounds(menu,button_indices);out.pixels.assign(static_cast<std::size_t>(out.bounds.width)*static_cast<std::size_t>(out.bounds.height),0);
    const auto width=static_cast<std::size_t>(out.bounds.width);
    for(const auto index:button_indices){
        if(index>=menu.buttons.size())continue;
        const auto& b=menu.buttons[index];const auto& style=button_style(menu,b);const auto& active=style.active;const int border=std::clamp(active.border_width,0,128);
        if(border>0&&active.border_color.a!=0){const auto color=palette.index(active.border_color);const int ox=b.bounds.x-border,oy=b.bounds.y-border,ow=b.bounds.width+2*border,oh=b.bounds.height+2*border;const int outer_radius=std::max(0,style.corner_radius+border);const int x0=std::max(out.bounds.x,ox),y0=std::max(out.bounds.y,oy),x1=std::min(out.bounds.x+out.bounds.width,ox+ow),y1=std::min(out.bounds.y+out.bounds.height,oy+oh);for(int gy=y0;gy<y1;++gy)for(int gx=x0;gx<x1;++gx){const int outer_x=gx-ox,outer_y=gy-oy;if(!rounded_inside(outer_x,outer_y,ow,oh,outer_radius))continue;const int inner_x=gx-b.bounds.x,inner_y=gy-b.bounds.y;const bool inside_inner=inner_x>=0&&inner_y>=0&&inner_x<b.bounds.width&&inner_y<b.bounds.height&&rounded_inside(inner_x,inner_y,b.bounds.width,b.bounds.height,style.corner_radius);if(!inside_inner)out.pixels[static_cast<std::size_t>(gy-out.bounds.y)*width+static_cast<std::size_t>(gx-out.bounds.x)]=color;}}
        if(b.kind==MenuButtonKind::Text&&active.text_color.a!=0){const auto color=palette.index(active.text_color);const int padding=std::max(3,std::clamp(active.border_width,0,128)+4);const auto mask=detail::render_button_text_mask(b.label,style,b.bounds.width,b.bounds.height,padding);if(mask.width==b.bounds.width&&mask.height==b.bounds.height){for(int y=0;y<mask.height;++y)for(int x=0;x<mask.width;++x)if(mask.pixels[static_cast<std::size_t>(y)*static_cast<std::size_t>(mask.width)+static_cast<std::size_t>(x)]){const int gx=b.bounds.x+x,gy=b.bounds.y+y;if(gx>=out.bounds.x&&gy>=out.bounds.y&&gx<out.bounds.x+out.bounds.width&&gy<out.bounds.y+out.bounds.height)out.pixels[static_cast<std::size_t>(gy-out.bounds.y)*width+static_cast<std::size_t>(gx-out.bounds.x)]=color;}}}
    }
    return out;
}
IgsSegment make_active_indicator_ods(std::uint16_t id,const Menu& menu,std::span<const std::size_t> button_indices,PaletteBuilder& palette,Rect* bounds_out,const Rect* fixed_bounds=nullptr){
    auto geometry=active_indicator_geometry(menu,button_indices,palette,fixed_bounds);if(bounds_out)*bounds_out=geometry.bounds;auto rr=rle(geometry.pixels,geometry.bounds.width,geometry.bounds.height);Bytes p;u16(p,id);p.push_back(0);p.push_back(0xC0);u24(p,static_cast<std::uint32_t>(rr.size()+4));u16(p,static_cast<std::uint16_t>(geometry.bounds.width));u16(p,static_cast<std::uint16_t>(geometry.bounds.height));append(p,rr);return {0x15,kMenuIgsPts90k,kMenuIgsPts90k,std::move(p)};
}
IgsSegment make_transparent_ods(std::uint16_t id){
    constexpr int w=16,h=12;Bytes px(static_cast<std::size_t>(w*h),0);auto rr=rle(px,w,h);Bytes p;u16(p,id);p.push_back(0);p.push_back(0xC0);u24(p,static_cast<std::uint32_t>(rr.size()+4));u16(p,w);u16(p,h);append(p,rr);return {0x15,kMenuIgsPts90k,kMenuIgsPts90k,std::move(p)};
}
}

NavCommand play_pl(std::uint16_t p){return nav(0b0001000000,1,true,false,p,0);} // Play_PL
NavCommand play_pl_pm(std::uint16_t p,std::uint16_t mark){return nav(0b0001000010,2,true,true,p,mark);} // Play_PL_PM
NavCommand jump_title(std::uint16_t t){return nav(0b0000100001,1,true,false,t,0);} // Jump_Title
NavCommand jump_object(std::uint16_t o){return nav(0b0000100000,1,true,false,o,0);} // Jump_Object

std::uint16_t title_audio_state_gpr(std::uint16_t title){
    if(title==0||title>999)throw std::runtime_error("HDMV title stream state requires title number 1..999");
    return static_cast<std::uint16_t>(4090U-2U*static_cast<unsigned>(title-1U));
}
std::uint16_t title_subtitle_state_gpr(std::uint16_t title){return static_cast<std::uint16_t>(title_audio_state_gpr(title)-1U);}
std::uint16_t menu_selection_state_gpr(std::uint16_t menu_object){
    // Combined visible menus + titles are capped at 999 playlists. Menu-selection
    // state therefore fits safely in GPR0..998, far below the per-title stream
    // state block (GPR2093..4090). GPRs power up as zero, which means "no saved
    // selection yet" and lets the IGS initializer choose button 1 on first entry.
    if(menu_object>998U)throw std::runtime_error("HDMV menu selection state requires menu object 0..998");
    return menu_object;
}

MenuStreamIndicatorPlan make_menu_stream_indicator_plan(const Menu& menu){
    struct Key { StreamIndicatorKind kind;std::uint16_t title;std::uint16_t stream;
        bool operator<(const Key& o)const{return std::tie(kind,title,stream)<std::tie(o.kind,o.title,o.stream);} };
    std::map<Key,std::vector<std::size_t>> grouped;
    for(std::size_t bi=0;bi<menu.buttons.size();++bi){
        const auto actions=button_action_sequence(menu.buttons[bi]);
        std::optional<NavigationAction> stream_action;
        bool ambiguous=false;
        for(const auto& action:actions){
            if(action.kind!=NavigationActionKind::AudioTrack&&action.kind!=NavigationActionKind::SubtitleTrack&&action.kind!=NavigationActionKind::SubtitleOff)continue;
            if(stream_action){ambiguous=true;break;}stream_action=action;
        }
        if(!stream_action||ambiguous||stream_action->target_title==0)continue;
        const auto kind=stream_action->kind==NavigationActionKind::AudioTrack?StreamIndicatorKind::Audio:StreamIndicatorKind::Subtitle;
        const auto stream=stream_action->kind==NavigationActionKind::SubtitleOff?0U:stream_action->target_stream;
        grouped[{kind,stream_action->target_title,static_cast<std::uint16_t>(stream)}].push_back(bi);
    }
    MenuStreamIndicatorPlan plan;
    std::uint32_t next=static_cast<std::uint32_t>(menu.buttons.size())+1U;
    for(auto& [key,indices]:grouped){
        if(next>0xffffU)throw std::runtime_error("HDMV active-stream indicator button id overflow");
        plan.variants.push_back({key.kind,key.title,key.stream,static_cast<std::uint16_t>(next++),std::move(indices)});
    }
    plan.indicator_bog_count=plan.variants.size();
    if(!plan.variants.empty()){
        if(next>0xffffU)throw std::runtime_error("HDMV active-stream initializer button id overflow");
        plan.initializer_button_id=static_cast<std::uint16_t>(next);
    }
    return plan;
}

namespace {
constexpr std::uint16_t ReturnMenuGpr=4091;
// GPR2092 is immediately below the lowest per-title stream-state register
// (GPR2093 for title 999), and above the per-menu selection-state block.
// A numbered title sets it immediately before Jump_Title 0; the Top Menu
// MovieObject consumes it to route back to the remembered submenu only after
// the player has re-entered interactive title 0.
constexpr std::uint16_t ReturnDispatchGpr=2092;
constexpr std::uint16_t MenuCallContextGpr=4092;
constexpr std::uint16_t ChapterTargetGpr=4094;
constexpr std::uint16_t RepeatTargetGpr=4093;
NavCommand goto_command(std::uint32_t command_id){return nav(0x001,1,true,false,command_id,0);} // GoTo
NavCommand move_gpr_immediate(std::uint16_t gpr,std::uint32_t value){return nav(0x201,2,false,true,gpr,value);} // Move
NavCommand move_gpr_from_psr(std::uint16_t gpr,std::uint8_t psr){return nav(0x201,2,false,false,gpr,0x80000000U|psr);} // Move
NavCommand and_gpr_immediate(std::uint16_t gpr,std::uint32_t value){return nav(0x209,2,false,true,gpr,value);} // And
NavCommand sub_gpr_immediate(std::uint16_t gpr,std::uint32_t value){return nav(0x204,2,false,true,gpr,value);} // Sub
NavCommand eq_gpr_immediate(std::uint16_t gpr,std::uint32_t value){return nav(0x102,2,false,true,gpr,value);} // Eq
NavCommand set_stream_command(std::uint32_t primary){return nav(0x221,2,true,true,primary,0);} // SetStream
NavCommand set_stream_command_indirect(std::uint32_t primary){return nav(0x221,2,false,true,primary,0);} // SetStream using GPR operands
NavCommand set_button_page_command(std::uint16_t button){return nav(0x223,2,true,true,0x80000000U|button,0);} // SetButtonPage
NavCommand set_button_page_command_indirect(std::uint16_t gpr){return nav(0x223,2,false,true,0x80000000U|gpr,0);} // SetButtonPage, button id from GPR
NavCommand enable_button_command(std::uint16_t button){return nav(0x224,1,true,false,button,0);} // EnableButton
NavCommand disable_button_command(std::uint16_t button){return nav(0x225,1,true,false,button,0);} // DisableButton

std::vector<NavCommand> active_indicator_switch_commands(const Menu* menu,const NavigationAction& action){
    if(!menu)return {};
    if(action.kind!=NavigationActionKind::AudioTrack&&action.kind!=NavigationActionKind::SubtitleTrack&&action.kind!=NavigationActionKind::SubtitleOff)return {};
    const auto kind=action.kind==NavigationActionKind::AudioTrack?StreamIndicatorKind::Audio:StreamIndicatorKind::Subtitle;
    const auto stream=action.kind==NavigationActionKind::SubtitleOff?0U:action.target_stream;
    const auto plan=make_menu_stream_indicator_plan(*menu);
    const StreamIndicatorVariant* desired=nullptr;
    for(const auto& v:plan.variants)if(v.kind==kind&&v.target_title==action.target_title&&v.target_stream==stream){desired=&v;break;}
    if(!desired)return {};

    // The variants below are authored into one BOG per title/category. Keep
    // the transition explicit for player compatibility: disable every conflicting
    // variant before enabling the desired one. The 0.1.70 shared-canvas layout
    // ensures the subsequent BOG redraw also replaces stale pixels in place.
    std::vector<NavCommand> out;
    for(const auto& v:plan.variants){
        if(v.kind==kind&&v.target_title==action.target_title&&v.button_id!=desired->button_id)out.push_back(disable_button_command(v.button_id));
    }
    out.push_back(enable_button_command(desired->button_id));
    return out;
}

std::vector<NavCommand> apply_title_stream_state_commands(std::uint16_t title,std::uint32_t command_base){
    constexpr std::uint16_t ScratchGpr=4095;
    const auto audio=title_audio_state_gpr(title),subtitle=title_subtitle_state_gpr(title);
    std::vector<NavCommand> out;out.reserve(16);
    // Audio state 0 means leave the player's current/default selection alone.
    out.push_back(eq_gpr_immediate(audio,0));
    out.push_back(goto_command(command_base+9U));
    // SetStream always writes the subtitle display bit, even when changing only
    // audio. Preserve PSR2's display bit and then apply the title's subtitle
    // state separately below.
    out.push_back(move_gpr_from_psr(ScratchGpr,2));
    out.push_back(and_gpr_immediate(ScratchGpr,0x80000000U));
    out.push_back(eq_gpr_immediate(ScratchGpr,0));
    out.push_back(goto_command(command_base+8U));
    out.push_back(set_stream_command_indirect(0x80000000U|(static_cast<std::uint32_t>(audio)<<16U)|0x00004000U));
    out.push_back(goto_command(command_base+9U));
    out.push_back(set_stream_command_indirect(0x80000000U|(static_cast<std::uint32_t>(audio)<<16U)));
    // Subtitle 0xffffffff means player default/current; zero means explicitly
    // off; a positive value selects and enables that one-based PG/TextST stream.
    out.push_back(eq_gpr_immediate(subtitle,0xffffffffU));
    out.push_back(goto_command(command_base+16U));
    out.push_back(eq_gpr_immediate(subtitle,0));
    out.push_back(goto_command(command_base+15U));
    out.push_back(set_stream_command_indirect(0x0000c000U|subtitle));
    out.push_back(goto_command(command_base+16U));
    out.push_back(set_stream_command(0));
    return out;
}

std::vector<NavCommand> button_navigation_commands_impl(const MenuButton& b,const MenuObjectMap& menu_objects,std::uint32_t command_base=0){
    if(b.target_kind==MenuButtonTargetKind::Menu){
        const auto it=menu_objects.find(b.target_menu_id);
        if(it==menu_objects.end())throw std::runtime_error("menu button targets unknown menu: "+b.target_menu_id);
        return {jump_object(it->second)};
    }
    if(b.target_kind==MenuButtonTargetKind::Title)return {jump_title(b.target_title)};
    if(b.target_kind==MenuButtonTargetKind::SubtitleTrack){
        const auto stream=static_cast<std::uint32_t>(b.target_stream)&0x0fffU;
        return {set_stream_command(0x0000c000U|stream)}; // change PG/TextST stream and enable display
    }
    if(b.target_kind==MenuButtonTargetKind::SubtitleOff){
        return {set_stream_command(0)}; // leave selected stream number intact, clear display flag
    }
    if(b.target_kind!=MenuButtonTargetKind::AudioTrack)throw std::runtime_error("unsupported menu button target kind");

    // SetStream always writes the subtitle display flag as part of its first
    // operand. Preserve the current PSR2 display bit when selecting audio so an
    // audio-language button neither turns subtitles on nor turns them off.
    constexpr std::uint16_t ScratchGpr=4095;
    const auto stream=(static_cast<std::uint32_t>(b.target_stream)&0x0fffU)<<16;
    return {
        move_gpr_from_psr(ScratchGpr,2),
        and_gpr_immediate(ScratchGpr,0x80000000U),
        eq_gpr_immediate(ScratchGpr,0),
        goto_command(command_base+6U),
        set_stream_command(0x80000000U|stream|0x00004000U),
        goto_command(command_base+7U),
        set_stream_command(0x80000000U|stream)
    };
}

std::vector<NavCommand> action_commands(const NavigationAction& action,const MenuObjectMap& menu_objects,std::uint16_t menu_count,std::uint32_t command_base,const Menu* current_menu){
    if(action.kind==NavigationActionKind::PlayTitle){
        if(action.target_title==0)throw std::runtime_error("navigation action has invalid title 0");
        const auto playlist=static_cast<std::uint16_t>(menu_count + action.target_title - 1U);
        std::vector<NavCommand> out;
        out.push_back(move_gpr_immediate(MenuCallContextGpr,action.target_title));
        auto stream=apply_title_stream_state_commands(action.target_title,command_base+static_cast<std::uint32_t>(out.size()));out.insert(out.end(),stream.begin(),stream.end());
        if(action.target_chapter>1U)out.push_back(play_pl_pm(playlist,static_cast<std::uint16_t>(action.target_chapter-1U)));
        else out.push_back(play_pl(playlist));
        out.push_back(move_gpr_immediate(MenuCallContextGpr,0));
        return out;
    }
    if(action.kind==NavigationActionKind::AudioTrack||action.kind==NavigationActionKind::SubtitleTrack||action.kind==NavigationActionKind::SubtitleOff){
        if(action.target_title==0)throw std::runtime_error("stream-selection action has invalid title 0");
        const auto gpr=action.kind==NavigationActionKind::AudioTrack?title_audio_state_gpr(action.target_title):title_subtitle_state_gpr(action.target_title);
        const auto value=action.kind==NavigationActionKind::SubtitleOff?0U:static_cast<std::uint32_t>(action.target_stream);
        std::vector<NavCommand> out{move_gpr_immediate(gpr,value)};
        auto indicators=active_indicator_switch_commands(current_menu,action);
        out.insert(out.end(),indicators.begin(),indicators.end());
        return out;
    }
    MenuButton compatibility;
    compatibility.target_title=action.target_title;
    compatibility.target_chapter=action.target_chapter;
    compatibility.target_stream=action.target_stream;
    compatibility.target_menu_id=action.target_menu_id;
    switch(action.kind){
        case NavigationActionKind::Menu: compatibility.target_kind=MenuButtonTargetKind::Menu; break;
        case NavigationActionKind::AudioTrack: compatibility.target_kind=MenuButtonTargetKind::AudioTrack; break;
        case NavigationActionKind::SubtitleTrack: compatibility.target_kind=MenuButtonTargetKind::SubtitleTrack; break;
        case NavigationActionKind::SubtitleOff: compatibility.target_kind=MenuButtonTargetKind::SubtitleOff; break;
        case NavigationActionKind::PlayTitle: break;
        case NavigationActionKind::RepeatBegin:
        case NavigationActionKind::RepeatEnd: throw std::runtime_error("repeat-group marker reached HDMV action compiler");
    }
    return button_navigation_commands_impl(compatibility,menu_objects,command_base);
}
}

std::vector<NavCommand> make_title_stream_initialization_commands(std::span<const TitleStreamDefaults> defaults,std::uint32_t command_base){
    (void)command_base;
    std::vector<NavCommand> out;out.reserve(defaults.size()*2U);
    for(std::size_t i=0;i<defaults.size();++i){
        const auto title=static_cast<std::uint16_t>(i+1U);
        if(defaults[i].audio_stream<0||defaults[i].audio_stream>4095)throw std::runtime_error("default audio stream is outside HDMV range");
        if(defaults[i].subtitle_stream<-1||defaults[i].subtitle_stream>4095)throw std::runtime_error("default subtitle stream is outside HDMV range");
        out.push_back(move_gpr_immediate(title_audio_state_gpr(title),static_cast<std::uint32_t>(defaults[i].audio_stream)));
        out.push_back(move_gpr_immediate(title_subtitle_state_gpr(title),defaults[i].subtitle_stream<0?0xffffffffU:static_cast<std::uint32_t>(defaults[i].subtitle_stream)));
    }
    return out;
}

std::vector<NavCommand> make_button_navigation_commands(const MenuButton& button,const MenuObjectMap& menu_objects){
    return button_navigation_commands_impl(button,menu_objects);
}

std::vector<NavCommand> make_navigation_sequence_commands(std::span<const NavigationAction> actions,const MenuObjectMap& menu_objects,std::uint16_t menu_count,std::uint16_t return_menu_object,const Menu* current_menu){
    const std::vector<NavigationAction> source(actions.begin(),actions.end());
    const auto expanded=expand_navigation_repeat_groups(source);
    const auto& sequence=expanded.actions;
    std::vector<NavCommand> commands;
    bool terminal=false;
    std::uint32_t infinite_loop_command=0;
    bool loop_command_set=false;
    for(std::size_t i=0;i<sequence.size();++i){
        if(terminal)throw std::runtime_error("navigation menu/title jump must be the final action");
        if(expanded.has_infinite_loop()&&i==expanded.infinite_loop_start){
            infinite_loop_command=static_cast<std::uint32_t>(commands.size());
            loop_command_set=true;
        }
        const auto& action=sequence[i];
        const bool final_action=i+1==sequence.size();
        if(navigation_action_is_repeat_marker(action.kind))throw std::runtime_error("repeat-group marker survived navigation expansion");
        if(action.repeat_count==0U&&action.kind!=NavigationActionKind::PlayTitle)
            throw std::runtime_error("only a PlayTitle navigation action can repeat forever");
        if(action.repeat_count==0U&&!final_action)
            throw std::runtime_error("an infinitely repeated PlayTitle action must be final");
        if(action.kind==NavigationActionKind::Menu&&action.repeat_count!=1U)
            throw std::runtime_error("a menu jump cannot be repeated");

        // A terminal title action enters the numbered Blu-ray title with Jump_Title
        // so the player gets ordinary title timing/seek/OSD semantics even when
        // the action originates in a submenu. Carry the invoking menu, chapter,
        // and repeat state through reserved GPRs; the numbered title MovieObject
        // performs the repeat loop and returns to the invoking menu. A forever
        // repeat group must resume after every title, so it remains in this
        // synthetic MovieObject and uses Play_PL like other ordered sequences.
        if(!expanded.has_infinite_loop()&&action.kind==NavigationActionKind::PlayTitle&&final_action){
            if(action.target_title==0)throw std::runtime_error("navigation action has invalid title 0");
            commands.push_back(move_gpr_immediate(ReturnMenuGpr,return_menu_object));
            commands.push_back(move_gpr_immediate(ChapterTargetGpr,action.target_chapter>1U?action.target_chapter:0U));
            commands.push_back(move_gpr_immediate(RepeatTargetGpr,action.repeat_count==0U?0xffffffffU:static_cast<std::uint32_t>(action.repeat_count-1U)));
            commands.push_back(jump_title(static_cast<std::uint16_t>(action.target_title)));
            terminal=true;
            continue;
        }

        if(action.kind==NavigationActionKind::PlayTitle&&action.repeat_count==0U){
            const auto loop_command=static_cast<std::uint32_t>(commands.size());
            auto part=action_commands(action,menu_objects,menu_count,loop_command,current_menu);
            commands.insert(commands.end(),part.begin(),part.end());
            commands.push_back(goto_command(loop_command));
            terminal=true;
            continue;
        }

        const std::uint16_t repeat=std::max<std::uint16_t>(1U,action.repeat_count);
        for(std::uint16_t r=0;r<repeat;++r){
            auto part=action_commands(action,menu_objects,menu_count,static_cast<std::uint32_t>(commands.size()),current_menu);
            commands.insert(commands.end(),part.begin(),part.end());
        }
        terminal=action.kind==NavigationActionKind::Menu;
    }
    if(expanded.has_infinite_loop()){
        if(!loop_command_set)throw std::runtime_error("infinite repeat group expanded without a loop body");
        if(terminal)throw std::runtime_error("infinite repeat group cannot terminate with a menu jump");
        commands.push_back(goto_command(infinite_loop_command));
        terminal=true;
    }
    if(!terminal)commands.push_back(jump_object(return_menu_object));
    if(commands.size()>65535U)throw std::runtime_error("navigation sequence expands to more than 65535 HDMV commands");
    return commands;
}


std::optional<std::vector<NavCommand>> make_immediate_navigation_sequence_commands(
    std::span<const NavigationAction> actions,const MenuObjectMap& menu_objects,
    std::uint16_t return_menu_object,const Menu* current_menu){
    const std::vector<NavigationAction> source(actions.begin(),actions.end());
    const auto expanded=expand_navigation_repeat_groups(source);
    const auto& sequence=expanded.actions;
    if(expanded.has_infinite_loop())return std::nullopt;

    // IG button commands can change streams and perform terminal Jump_Object /
    // Jump_Title operations immediately. They cannot safely use Play_PL when
    // later commands must resume, because that requires MovieObject execution
    // context. Keep only those genuinely returning sequences in a synthetic
    // MovieObject; this avoids deferring ordinary button activation until the
    // current menu Play_PL reaches the end of its clip.
    std::size_t play_title_count=0;
    for(std::size_t i=0;i<sequence.size();++i){
        const auto& action=sequence[i];
        if(navigation_action_is_repeat_marker(action.kind))throw std::runtime_error("repeat-group marker survived navigation expansion");
        if(action.kind==NavigationActionKind::PlayTitle){
            ++play_title_count;
            if(i+1U!=sequence.size()||play_title_count>1U)return std::nullopt;
        }
    }

    std::vector<NavCommand> commands;
    bool terminal=false;
    for(std::size_t i=0;i<sequence.size();++i){
        if(terminal)throw std::runtime_error("navigation menu/title jump must be the final action");
        const auto& action=sequence[i];
        const bool final_action=i+1U==sequence.size();
        if(action.repeat_count==0U&&action.kind!=NavigationActionKind::PlayTitle)
            throw std::runtime_error("only a PlayTitle navigation action can repeat forever");
        if(action.kind==NavigationActionKind::Menu&&action.repeat_count!=1U)
            throw std::runtime_error("a menu jump cannot be repeated");

        if(action.kind==NavigationActionKind::PlayTitle){
            if(!final_action||action.target_title==0)return std::nullopt;
            commands.push_back(move_gpr_immediate(ReturnMenuGpr,return_menu_object));
            commands.push_back(move_gpr_immediate(ChapterTargetGpr,action.target_chapter>1U?action.target_chapter:0U));
            commands.push_back(move_gpr_immediate(RepeatTargetGpr,action.repeat_count==0U?0xffffffffU:static_cast<std::uint32_t>(action.repeat_count-1U)));
            commands.push_back(jump_title(static_cast<std::uint16_t>(action.target_title)));
            terminal=true;
            continue;
        }

        const std::uint16_t repeat=std::max<std::uint16_t>(1U,action.repeat_count);
        for(std::uint16_t r=0;r<repeat;++r){
            auto part=action_commands(action,menu_objects,0,static_cast<std::uint32_t>(commands.size()),current_menu);
            commands.insert(commands.end(),part.begin(),part.end());
        }
        terminal=action.kind==NavigationActionKind::Menu;
    }
    return commands;
}

std::vector<std::uint8_t> make_index(std::uint16_t title_count,std::uint16_t menu_count,bool version3,std::uint16_t first_play_object){
    const std::uint16_t menu_object_count = menu_count == 0U ? 1U : menu_count;
    Bytes app; u32(app,34); app.push_back(0); app.push_back(0x61); zeros(app,32); // 1080p / 23.976 app-info hint
    Bytes ix; append(ix,index_entry(first_play_object,true)); append(ix,index_entry(0,true)); u16(ix,title_count);
    // Visible menu MovieObjects occupy 0..menu_count-1. A menu-less disc still
    // reserves MovieObject 0 as an empty terminal object so Top Menu and the
    // end of a startup sequence have a stable non-looping destination. Numbered
    // titles follow that object.
    for(std::uint16_t i=0;i<title_count;i++) append(ix,index_entry(std::uint16_t(menu_object_count+i),false));
    Bytes out; if(version3) out.insert(out.end(),{'I','N','D','X','0','3','0','0'}); else out.insert(out.end(),{'I','N','D','X','0','2','0','0'}); u32(out,40+static_cast<std::uint32_t>(app.size()));u32(out,0);zeros(out,24);append(out,app);u32(out,static_cast<std::uint32_t>(ix.size()));append(out,ix);return out;
}

std::vector<std::uint8_t> make_movie_object(std::uint16_t title_count,std::uint16_t menu_count,bool version3,
                                            std::span<const MovieObject> extra_objects,
                                            std::span<const std::uint16_t> title_chapter_counts,
                                            std::span<const std::uint16_t> title_menu_call_objects,
                                            std::uint16_t menu_call_default_object,bool use_title_stream_state){
    constexpr std::uint16_t NoObject=0xffffU;
    const std::uint16_t menu_object_count = menu_count == 0U ? 1U : menu_count;
    if(!title_chapter_counts.empty()&&title_chapter_counts.size()!=title_count)throw std::runtime_error("HDMV title chapter-count table does not match title count");
    if(!title_menu_call_objects.empty()&&title_menu_call_objects.size()!=title_count)throw std::runtime_error("HDMV title Menu-Call table does not match title count");
    std::vector<MovieObject> objs;objs.reserve(static_cast<std::size_t>(menu_object_count)+title_count+extra_objects.size());
    std::vector<std::pair<std::uint16_t,std::uint16_t>> menu_call_dispatch;
    for(std::uint16_t i=0;i<title_count&&!title_menu_call_objects.empty();++i)if(title_menu_call_objects[i]!=NoObject)menu_call_dispatch.emplace_back(static_cast<std::uint16_t>(i+1U),title_menu_call_objects[i]);
    if(menu_count==0U){
        if(menu_call_dispatch.empty())objs.push_back({false,false,false,{}});
        else{
            if(menu_call_default_object==NoObject||menu_call_default_object==0U)throw std::runtime_error("menu-less HDMV Menu-Call dispatcher needs a terminal default object");
            std::vector<NavCommand> commands;const std::uint32_t branch_base=static_cast<std::uint32_t>(menu_call_dispatch.size()*2U+2U);
            for(std::size_t i=0;i<menu_call_dispatch.size();++i){commands.push_back(eq_gpr_immediate(MenuCallContextGpr,menu_call_dispatch[i].first));commands.push_back(goto_command(branch_base+static_cast<std::uint32_t>(i*2U)));}
            commands.push_back(move_gpr_immediate(MenuCallContextGpr,0));commands.push_back(jump_object(menu_call_default_object));
            for(const auto& entry:menu_call_dispatch){commands.push_back(move_gpr_immediate(MenuCallContextGpr,0));commands.push_back(jump_object(entry.second));}
            objs.push_back({false,false,false,std::move(commands)});
        }
    }else{
        // MovieObject 0 is the interactive Top Menu title. A numbered title
        // must return through Jump_Title 0 before routing to a submenu; jumping
        // directly to a menu MovieObject leaves players in numbered-title
        // timing context and can expose elapsed time while the menu is visible.
        // ReturnDispatchGpr distinguishes a natural title-end return from an
        // explicit Top Menu/Menu-Call request made while a title is playing.
        std::vector<NavCommand> commands;
        commands.push_back(eq_gpr_immediate(ReturnDispatchGpr,1));
        const std::size_t return_dispatch_goto=commands.size();
        commands.push_back(goto_command(0));

        std::vector<std::size_t> menu_call_gotos;
        menu_call_gotos.reserve(menu_call_dispatch.size());
        for(const auto& entry:menu_call_dispatch){
            commands.push_back(eq_gpr_immediate(MenuCallContextGpr,entry.first));
            menu_call_gotos.push_back(commands.size());
            commands.push_back(goto_command(0));
        }
        commands.push_back(move_gpr_immediate(MenuCallContextGpr,0));
        const std::uint32_t play_command=static_cast<std::uint32_t>(commands.size());
        commands.push_back(play_pl(0));
        commands.push_back(move_gpr_from_psr(menu_selection_state_gpr(0),10));
        commands.push_back(goto_command(play_command));

        for(std::size_t i=0;i<menu_call_dispatch.size();++i){
            commands[menu_call_gotos[i]].operand1=static_cast<std::uint32_t>(commands.size());
            commands.push_back(move_gpr_immediate(MenuCallContextGpr,0));
            commands.push_back(jump_object(menu_call_dispatch[i].second));
        }

        commands[return_dispatch_goto].operand1=static_cast<std::uint32_t>(commands.size());
        commands.push_back(move_gpr_immediate(ReturnDispatchGpr,0));
        std::vector<std::size_t> submenu_return_gotos;
        submenu_return_gotos.reserve(menu_count>0U?static_cast<std::size_t>(menu_count-1U):0U);
        for(std::uint16_t menu_object=1;menu_object<menu_count;++menu_object){
            commands.push_back(eq_gpr_immediate(ReturnMenuGpr,menu_object));
            submenu_return_gotos.push_back(commands.size());
            commands.push_back(goto_command(0));
        }
        commands.push_back(move_gpr_immediate(ReturnMenuGpr,0));
        commands.push_back(goto_command(play_command));
        for(std::uint16_t menu_object=1;menu_object<menu_count;++menu_object){
            commands[submenu_return_gotos[static_cast<std::size_t>(menu_object-1U)]].operand1=static_cast<std::uint32_t>(commands.size());
            commands.push_back(move_gpr_immediate(ReturnMenuGpr,0));
            commands.push_back(jump_object(menu_object));
        }
        objs.push_back({false,false,false,std::move(commands)});
        for(std::uint16_t i=1;i<menu_count;i++)objs.push_back({false,false,false,{play_pl(i),move_gpr_from_psr(menu_selection_state_gpr(i),10),goto_command(0)}});
    }
    for(std::uint16_t i=0;i<title_count;i++){
        const std::uint16_t playlist=static_cast<std::uint16_t>(menu_count+i);const std::uint16_t chapter_count=title_chapter_counts.empty()?1U:std::max<std::uint16_t>(1U,title_chapter_counts[i]);std::vector<NavCommand> commands;
        const std::size_t dispatch_count=chapter_count>1U?static_cast<std::size_t>(chapter_count-1U):0U;
        commands.push_back(move_gpr_immediate(MenuCallContextGpr,static_cast<std::uint32_t>(i+1U)));
        if(use_title_stream_state){auto stream=apply_title_stream_state_commands(static_cast<std::uint16_t>(i+1U),static_cast<std::uint32_t>(commands.size()));commands.insert(commands.end(),stream.begin(),stream.end());}
        const std::uint32_t dispatch_start=static_cast<std::uint32_t>(commands.size());const std::uint32_t default_start=dispatch_start+static_cast<std::uint32_t>(dispatch_count*2U);const std::uint32_t branch_base=default_start+2U;const std::uint32_t post_start=branch_base+static_cast<std::uint32_t>(dispatch_count*2U);const std::uint32_t finish=post_start+6U;
        commands.reserve(commands.size()+dispatch_count*4U+13U);
        for(std::uint16_t chapter=2;chapter<=chapter_count;++chapter){commands.push_back(eq_gpr_immediate(ChapterTargetGpr,chapter));commands.push_back(goto_command(branch_base+static_cast<std::uint32_t>(chapter-2U)*2U));}
        commands.push_back(play_pl(playlist));commands.push_back(goto_command(post_start));
        for(std::uint16_t chapter=2;chapter<=chapter_count;++chapter){commands.push_back(play_pl_pm(playlist,static_cast<std::uint16_t>(chapter-1U)));commands.push_back(goto_command(post_start));}
        commands.push_back(eq_gpr_immediate(RepeatTargetGpr,0));
        commands.push_back(goto_command(finish));
        commands.push_back(eq_gpr_immediate(RepeatTargetGpr,0xffffffffU));
        commands.push_back(goto_command(0));
        commands.push_back(sub_gpr_immediate(RepeatTargetGpr,1));
        commands.push_back(goto_command(0));
        commands.push_back(move_gpr_immediate(MenuCallContextGpr,0));
        commands.push_back(move_gpr_immediate(ChapterTargetGpr,0));
        commands.push_back(move_gpr_immediate(RepeatTargetGpr,0));
        // Terminal menu buttons enter a numbered title with Jump_Title so the
        // player exposes ordinary title timing/seek state.  On natural title
        // completion, re-enter the interactive Top Menu title (title 0) before
        // routing to the remembered submenu.  This resets player UI/title
        // context so menu playback does not inherit the movie's elapsed time.
        if(menu_count==0U){
            commands.push_back(move_gpr_immediate(ReturnMenuGpr,0));
            commands.push_back(jump_object(0));
        }else{
            commands.push_back(move_gpr_immediate(ReturnDispatchGpr,1));
            commands.push_back(jump_title(0));
        }
        objs.push_back({false,false,false,std::move(commands)});
    }
    objs.insert(objs.end(),extra_objects.begin(),extra_objects.end());if(objs.size()>65535U)throw std::runtime_error("too many HDMV MovieObjects");Bytes body;zeros(body,4);u16(body,static_cast<std::uint16_t>(objs.size()));for(auto& o:objs){body.push_back((o.resume_intention?0x80:0)|(o.menu_call_mask?0x40:0)|(o.title_search_mask?0x20:0));body.push_back(0);u16(body,static_cast<std::uint16_t>(o.commands.size()));for(auto& c:o.commands)cmd(body,c);}Bytes out;if(version3)out.insert(out.end(),{'M','O','B','J','0','3','0','0'});else out.insert(out.end(),{'M','O','B','J','0','2','0','0'});u32(out,0);zeros(out,28);u32(out,static_cast<std::uint32_t>(body.size()));append(out,body);return out;
}

static void write_file(const std::filesystem::path& p,const Bytes& b){std::filesystem::create_directories(p.parent_path());std::ofstream f(p,std::ios::binary);if(!f)throw std::runtime_error("cannot write "+p.string());f.write(reinterpret_cast<const char*>(b.data()),std::streamsize(b.size()));}
void write_control_files(const std::filesystem::path& bdmv,std::uint16_t n,std::uint16_t menus,bool version3,
                         std::span<const MovieObject> extra_objects,std::uint16_t first_play_object,
                         std::span<const std::uint16_t> title_chapter_counts,
                         std::span<const std::uint16_t> title_menu_call_objects,
                         std::uint16_t menu_call_default_object,bool use_title_stream_state){
    auto i=make_index(n,menus,version3,first_play_object),m=make_movie_object(n,menus,version3,extra_objects,title_chapter_counts,title_menu_call_objects,menu_call_default_object,use_title_stream_state);
    write_file(bdmv/"index.bdmv",i);write_file(bdmv/"MovieObject.bdmv",m);write_file(bdmv/"BACKUP/index.bdmv",i);write_file(bdmv/"BACKUP/MovieObject.bdmv",m);
}

void add_playlist_user_operation_mask(const std::filesystem::path& playlist,std::uint64_t index_mask){
    if(index_mask==0)return;
    std::fstream f(playlist,std::ios::in|std::ios::out|std::ios::binary);
    if(!f)throw std::runtime_error("cannot patch Blu-ray playlist UOP mask: "+playlist.string());
    f.seekg(0,std::ios::end);
    const auto size=f.tellg();
    if(size<std::streamoff(56))throw std::runtime_error("Blu-ray playlist is too small for AppInfoPlayList UOP mask: "+playlist.string());
    f.seekg(0);
    std::array<char,4> magic{};
    f.read(magic.data(),4);
    if(!f||magic!=std::array<char,4>{'M','P','L','S'})throw std::runtime_error("invalid Blu-ray playlist while patching UOP mask: "+playlist.string());
    std::array<std::uint8_t,8> mask{};
    f.seekg(48);
    f.read(reinterpret_cast<char*>(mask.data()),8);
    if(!f)throw std::runtime_error("cannot read Blu-ray playlist UOP mask: "+playlist.string());
    for(unsigned bit=0;bit<64;++bit){
        if(index_mask&(std::uint64_t{1}<<bit))mask[bit/8U]|=static_cast<std::uint8_t>(1U<<(7U-(bit%8U)));
    }
    f.clear();
    f.seekp(48);
    f.write(reinterpret_cast<const char*>(mask.data()),8);
    if(!f)throw std::runtime_error("cannot write Blu-ray playlist UOP mask: "+playlist.string());
}


enum class NavDirection { Up, Down, Left, Right };

std::uint16_t geometric_neighbor(const Menu& menu, std::size_t current, NavDirection direction) {
    const auto& a = menu.buttons.at(current).bounds;
    const double ax = static_cast<double>(a.x) + static_cast<double>(a.width) / 2.0;
    const double ay = static_cast<double>(a.y) + static_cast<double>(a.height) / 2.0;

    auto deltas = [&](const Rect& b) {
        const double bx = static_cast<double>(b.x) + static_cast<double>(b.width) / 2.0;
        const double by = static_cast<double>(b.y) + static_cast<double>(b.height) / 2.0;
        switch (direction) {
        case NavDirection::Up:    return std::pair<double,double>{ay - by, bx - ax};
        case NavDirection::Down:  return std::pair<double,double>{by - ay, bx - ax};
        case NavDirection::Left:  return std::pair<double,double>{ax - bx, by - ay};
        case NavDirection::Right: return std::pair<double,double>{bx - ax, by - ay};
        }
        return std::pair<double,double>{0.0,0.0};
    };

    // First choose the closest button actually lying in the requested half-plane.
    // Weight perpendicular displacement so rows/columns are preferred over diagonals.
    bool found = false;
    std::size_t best = current;
    double best_score = 0.0;
    for (std::size_t i = 0; i < menu.buttons.size(); ++i) {
        if (i == current) continue;
        const auto [primary, perpendicular] = deltas(menu.buttons[i].bounds);
        if (primary <= 0.5) continue;
        const double score = primary + 2.0 * std::abs(perpendicular);
        if (!found || score < best_score) { found = true; best = i; best_score = score; }
    }
    if (found) return static_cast<std::uint16_t>(best + 1U);

    // Preserve convenient wrap-around at the end of a row/column. Among buttons
    // on the opposite side, prefer the same row/column, then the far edge.
    bool wrap_found = false;
    double best_perpendicular = 0.0;
    double best_primary = 0.0;
    for (std::size_t i = 0; i < menu.buttons.size(); ++i) {
        if (i == current) continue;
        const auto [primary, perpendicular] = deltas(menu.buttons[i].bounds);
        if (primary >= -0.5) continue;
        const double perp = std::abs(perpendicular);
        const double span = std::abs(primary);
        if (!wrap_found || perp < best_perpendicular - 0.5 ||
            (std::abs(perp - best_perpendicular) <= 0.5 && span > best_primary)) {
            wrap_found = true; best = i; best_perpendicular = perp; best_primary = span;
        }
    }
    return static_cast<std::uint16_t>((wrap_found ? best : current) + 1U);
}

std::vector<IgsSegment> make_menu_display_set(const Menu& menu,const MenuObjectMap& menu_objects,std::span<const std::vector<NavCommand>> button_commands,std::uint8_t frame_rate_code,std::uint16_t selection_state_gpr){
    if(menu.buttons.empty())throw std::runtime_error("menu requires at least one button");
    if(menu.buttons.size()>255)throw std::runtime_error("menu supports at most 255 buttons");
    if(!button_commands.empty()&&button_commands.size()!=menu.buttons.size())throw std::runtime_error("button command count does not match menu button count");
    const auto indicators=make_menu_stream_indicator_plan(menu);
    const std::size_t bog_count=menu.buttons.size()+indicators.indicator_bog_count+(indicators.variants.empty()?0U:1U);
    if(bog_count>255U)throw std::runtime_error("menu has too many HDMV button-overlap groups after active-stream indicators are added");

    PaletteBuilder palette;
    struct IndicatorObject { const StreamIndicatorVariant* variant=nullptr;std::uint16_t object_id=0;Rect bounds{}; };
    std::vector<IndicatorObject> indicator_objects;indicator_objects.reserve(indicators.variants.size());
    std::uint32_t next_object=static_cast<std::uint32_t>(menu.buttons.size()*3U);
    for(const auto& variant:indicators.variants){
        if(next_object>0xffffU)throw std::runtime_error("HDMV active-stream object id overflow");
        const auto bounds=active_indicator_bounds(menu,variant.source_button_indices);
        indicator_objects.push_back({&variant,static_cast<std::uint16_t>(next_object++),bounds});
    }
    std::uint16_t initializer_object_id=0xffffU;
    if(!indicators.variants.empty()){
        if(next_object>0xffffU)throw std::runtime_error("HDMV active-stream initializer object id overflow");
        initializer_object_id=static_cast<std::uint16_t>(next_object++);
    }

    // Build the automatic initializer. Each active-stream visual variant has
    // its own BOG and starts disabled. On menu entry this transparent auto-action
    // button clears all variants, reads the title-specific stream-state GPR and
    // enables exactly the matching visual marker. It then restores ordinary
    // keyboard focus.
    std::vector<NavCommand> initializer_commands;
    if(!indicators.variants.empty()){
        std::map<std::pair<StreamIndicatorKind,std::uint16_t>,std::vector<const StreamIndicatorVariant*>> groups;
        for(const auto& v:indicators.variants)groups[{v.kind,v.target_title}].push_back(&v);
        for(const auto& [key,variants]:groups){
            const auto base=static_cast<std::uint32_t>(initializer_commands.size());
            const auto n=static_cast<std::uint32_t>(variants.size());
            // Each visual variant has its own BOG for hardware-player
            // compositing compatibility. Explicitly clear every variant first
            // so a persistent player-side BOG state can never leave two active
            // markers visible after a menu loop/re-entry.
            const auto checks_base=base+n;
            const auto arm_base=checks_base+2U*n+1U;
            const auto group_end=arm_base+2U*n;
            const auto gpr=key.first==StreamIndicatorKind::Audio?title_audio_state_gpr(key.second):title_subtitle_state_gpr(key.second);
            for(const auto* variant:variants)initializer_commands.push_back(disable_button_command(variant->button_id));
            for(std::uint32_t i=0;i<n;++i){initializer_commands.push_back(eq_gpr_immediate(gpr,variants[i]->target_stream));initializer_commands.push_back(goto_command(arm_base+2U*i));}
            initializer_commands.push_back(goto_command(group_end));
            for(const auto* variant:variants){initializer_commands.push_back(enable_button_command(variant->button_id));initializer_commands.push_back(goto_command(group_end));}
        }
        // The initializer itself is selected statically so it can run on every
        // new IGS composition. Restore the ordinary button saved by the menu
        // MovieObject after the previous Play_PL. A zero GPR means this is the
        // menu's first entry, so select button 1. SetButtonPage supports an
        // indirect GPR operand, avoiding any page-per-button duplication.
        const auto restore_base=static_cast<std::uint32_t>(initializer_commands.size());
        initializer_commands.push_back(eq_gpr_immediate(selection_state_gpr,0));
        initializer_commands.push_back(goto_command(restore_base+4U));
        initializer_commands.push_back(set_button_page_command_indirect(selection_state_gpr));
        initializer_commands.push_back(goto_command(restore_base+5U));
        initializer_commands.push_back(set_button_page_command(1));
        if(initializer_commands.size()>65535U)throw std::runtime_error("HDMV active-stream initializer exceeds command limit");
    }

    // Preloaded Interactive Graphics use stream_model=1. libbluray's public
    // IG definitions use ui_model=0 for Always-On and ui_model=1 for Pop-Up,
    // so the packed model byte is 0x80. Unlike multiplexed stream_model=0, the
    // preloaded composition does not carry composition/selection timeout clocks;
    // it remains resident independently of the main-path video timeline.
    Bytes rest; rest.push_back(0x80); // stream_model=1 (preloaded), ui_model=0 (Always-On)
    u24(rest,0); rest.push_back(1); // infinite user timeout, one page
    rest.push_back(0);rest.push_back(0);zeros(rest,8);
    rest.push_back(0);rest.push_back(0);rest.push_back(0);rest.push_back(0);
    // Without an initializer, 0xffff tells the player to honor PSR10 and only
    // fall back to the first valid button when PSR10 is not valid. This preserves
    // ordinary focus across a menu-video loop. When active-stream indicators are
    // present, their transparent initializer must be selected statically; it then
    // restores the saved ordinary selection from selection_state_gpr.
    rest.push_back(0);u16(rest,indicators.variants.empty()?0xffffU:indicators.initializer_button_id);u16(rest,0x0000);rest.push_back(0);
    rest.push_back(static_cast<std::uint8_t>(bog_count));

    // Ordinary interactive buttons retain one independent BOG each.
    for(std::size_t i=0;i<menu.buttons.size();i++){
        const auto& b=menu.buttons[i];const std::uint16_t id=static_cast<std::uint16_t>(i+1);
        const std::uint16_t up=geometric_neighbor(menu,i,NavDirection::Up),down=geometric_neighbor(menu,i,NavDirection::Down),left=geometric_neighbor(menu,i,NavDirection::Left),right=geometric_neighbor(menu,i,NavDirection::Right);
        u16(rest,id);rest.push_back(1);
        u16(rest,id);u16(rest,0xffff);rest.push_back(0);
        u16(rest,static_cast<std::uint16_t>(std::clamp(b.bounds.x,0,65535)));u16(rest,static_cast<std::uint16_t>(std::clamp(b.bounds.y,0,65535)));
        u16(rest,up);u16(rest,down);u16(rest,left);u16(rest,right);
        const std::uint16_t base=static_cast<std::uint16_t>(i*3U);
        u16(rest,base);u16(rest,base);rest.push_back(0);
        rest.push_back(0xff);u16(rest,static_cast<std::uint16_t>(base+1));u16(rest,static_cast<std::uint16_t>(base+1));rest.push_back(0);
        rest.push_back(0xff);u16(rest,static_cast<std::uint16_t>(base+2));u16(rest,static_cast<std::uint16_t>(base+2));
        const auto commands=button_commands.empty()?make_button_navigation_commands(b,menu_objects):button_commands[i];
        u16(rest,static_cast<std::uint16_t>(commands.size()));for(const auto& command:commands)cmd(rest,command);
    }

    // Keep every persistent active-stream indicator in its own fixed-position
    // BOG. Earlier releases put all choices for one title/category into a single
    // union-sized transparent canvas. VLC composites that canvas as intended,
    // but some hardware players treat transparent pixels in an enabled BOG as
    // replacing the ordinary button artwork underneath, making the other audio
    // or subtitle choices disappear. A one-button BOG only overlaps the active
    // choice. Stream-change commands explicitly disable the old indicator BOGs
    // before enabling the new one, so ordinary button artwork remains untouched.
    for(const auto& object:indicator_objects){
        const auto id=object.variant->button_id;
        u16(rest,0xffff);rest.push_back(1);
        u16(rest,id);u16(rest,0xffff);rest.push_back(0);
        u16(rest,static_cast<std::uint16_t>(std::clamp(object.bounds.x,0,65535)));u16(rest,static_cast<std::uint16_t>(std::clamp(object.bounds.y,0,65535)));
        u16(rest,0xffff);u16(rest,0xffff);u16(rest,0xffff);u16(rest,0xffff);
        u16(rest,object.object_id);u16(rest,object.object_id);rest.push_back(0);
        rest.push_back(0xff);u16(rest,0xffff);u16(rest,0xffff);rest.push_back(0);
        rest.push_back(0xff);u16(rest,0xffff);u16(rest,0xffff);u16(rest,0);
    }

    if(!indicators.variants.empty()){
        // Transparent initialization BOG. auto_action_flag=1; its final
        // SetButtonPage restores the ordinary button saved across the menu loop
        // (or chooses button 1 on the menu's first entry).
        u16(rest,indicators.initializer_button_id);rest.push_back(1);
        u16(rest,indicators.initializer_button_id);u16(rest,0xffff);rest.push_back(0x80);
        u16(rest,0);u16(rest,0);u16(rest,0xffff);u16(rest,0xffff);u16(rest,0xffff);u16(rest,0xffff);
        u16(rest,initializer_object_id);u16(rest,initializer_object_id);rest.push_back(0);
        rest.push_back(0xff);u16(rest,initializer_object_id);u16(rest,initializer_object_id);rest.push_back(0);
        rest.push_back(0xff);u16(rest,initializer_object_id);u16(rest,initializer_object_id);
        u16(rest,static_cast<std::uint16_t>(initializer_commands.size()));for(const auto& command:initializer_commands)cmd(rest,command);
    }

    if(frame_rate_code==0||frame_rate_code>7||frame_rate_code==5)throw std::runtime_error("invalid HDMV IG frame-rate code");
    Bytes ics;u16(ics,static_cast<std::uint16_t>(menu.width));u16(ics,static_cast<std::uint16_t>(menu.height));ics.push_back(static_cast<std::uint8_t>(frame_rate_code<<4));
    u16(ics,0);ics.push_back(0x80);ics.push_back(0xC0);u24(ics,static_cast<std::uint32_t>(rest.size()));append(ics,rest);
    std::vector<IgsSegment> segs;segs.push_back({0x18,kMenuIgsPts90k,kMenuIgsPts90k,std::move(ics)});

    std::vector<IgsSegment> objects;objects.reserve(menu.buttons.size()*3U+indicator_objects.size()+1U);
    for(std::size_t i=0;i<menu.buttons.size();++i)for(int state=0;state<3;++state)objects.push_back(make_ods(static_cast<std::uint16_t>(i*3U+static_cast<std::size_t>(state)),menu,menu.buttons[i],state,palette));
    for(const auto& object:indicator_objects){Rect ignored;objects.push_back(make_active_indicator_ods(object.object_id,menu,object.variant->source_button_indices,palette,&ignored,&object.bounds));}
    if(!indicators.variants.empty())objects.push_back(make_transparent_ods(initializer_object_id));

    Bytes pal={0,0};for(std::size_t i=0;i<palette.colors.size();++i)append_palette_entry(pal,static_cast<std::uint8_t>(i),palette.colors[i]);
    segs.push_back({0x14,kMenuIgsPts90k,kMenuIgsPts90k,std::move(pal)});for(auto& object:objects)segs.push_back(std::move(object));segs.push_back({0x80,kMenuIgsPts90k,kMenuIgsPts90k,{}});return segs;
}

std::vector<std::uint8_t> serialize_igs_file(std::span<const IgsSegment> s){Bytes out;for(auto& x:s){out.push_back('I');out.push_back('G');u32(out,x.pts90k);u32(out,x.dts90k);out.push_back(x.type);u16(out,static_cast<std::uint16_t>(x.payload.size()));append(out,x.payload);}return out;}
void write_igs(const std::filesystem::path& p,const Menu& m,const MenuObjectMap& menu_objects,std::span<const std::vector<NavCommand>> button_commands,std::uint8_t frame_rate_code,std::uint16_t selection_state_gpr){write_file(p,serialize_igs_file(make_menu_display_set(m,menu_objects,button_commands,frame_rate_code,selection_state_gpr)));}

} // namespace bdmvauthor::hdmv
