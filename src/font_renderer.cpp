// SPDX-License-Identifier: Apache-2.0
#include "bdmvauthor/font_renderer.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <memory>
#include <filesystem>
#include <stdexcept>
#include <utility>
#include <cstdlib>

#if defined(_WIN32) && (defined(BDMVAUTHOR_HAVE_FONTCONFIG) || defined(BDMVAUTHOR_HAVE_FREETYPE_FONTCONFIG))
#include <windows.h>
#endif

#if defined(BDMVAUTHOR_HAVE_FONTCONFIG) || defined(BDMVAUTHOR_HAVE_FREETYPE_FONTCONFIG)
#include <fontconfig/fontconfig.h>
#endif
#if defined(BDMVAUTHOR_HAVE_FREETYPE_FONTCONFIG)
#include <ft2build.h>
#include FT_FREETYPE_H
#endif

namespace bdmvauthor::detail {
namespace {
#if !defined(BDMVAUTHOR_HAVE_FREETYPE_FONTCONFIG)
std::array<std::uint8_t,7> glyph(char ch) {
    ch=static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    switch(ch){
#define G(C,a,b,c,d,e,f,g) case C:return {a,b,c,d,e,f,g};
G('A',14,17,17,31,17,17,17) G('B',30,17,17,30,17,17,30)
G('C',14,17,16,16,16,17,14) G('D',30,17,17,17,17,17,30)
G('E',31,16,16,30,16,16,31) G('F',31,16,16,30,16,16,16)
G('G',14,17,16,23,17,17,15) G('H',17,17,17,31,17,17,17)
G('I',31,4,4,4,4,4,31) G('J',7,2,2,2,18,18,12)
G('K',17,18,20,24,20,18,17) G('L',16,16,16,16,16,16,31)
G('M',17,27,21,21,17,17,17) G('N',17,25,21,19,17,17,17)
G('O',14,17,17,17,17,17,14) G('P',30,17,17,30,16,16,16)
G('Q',14,17,17,17,21,18,13) G('R',30,17,17,30,20,18,17)
G('S',15,16,16,14,1,1,30) G('T',31,4,4,4,4,4,4)
G('U',17,17,17,17,17,17,14) G('V',17,17,17,17,17,10,4)
G('W',17,17,17,21,21,21,10) G('X',17,17,10,4,10,17,17)
G('Y',17,17,10,4,4,4,4) G('Z',31,1,2,4,8,16,31)
G('0',14,17,19,21,25,17,14) G('1',4,12,4,4,4,4,14)
G('2',14,17,1,2,4,8,31) G('3',30,1,1,14,1,1,30)
G('4',2,6,10,18,31,2,2) G('5',31,16,16,30,1,1,30)
G('6',14,16,16,30,17,17,14) G('7',31,1,2,4,8,8,8)
G('8',14,17,17,14,17,17,14) G('9',14,17,17,15,1,1,14)
G('-',0,0,0,31,0,0,0) G('_',0,0,0,0,0,0,31) G('.',0,0,0,0,0,12,12)
G(':',0,12,12,0,12,12,0) G('/',1,2,4,8,16,0,0) G(' ',0,0,0,0,0,0,0)
G('?',14,17,1,2,4,0,4)
#undef G
    default:return {14,17,1,2,4,0,4}; }
}

TextMask builtin_mask(const std::string& text, const ButtonStyle& style,
                      int width, int height, int padding) {
    TextMask out{width,height,std::vector<std::uint8_t>(static_cast<std::size_t>(width)*static_cast<std::size_t>(height),0)};
    if (width <= 0 || height <= 0 || text.empty()) return out;
    const int avail_w = std::max(1,width-2*padding);
    const int avail_h = std::max(1,height-2*padding);
    const int chars = std::max(1,static_cast<int>(text.size()));
    int scale = std::max(1, style.font_size_px / 7);
    scale = std::min(scale, std::max(1, avail_h/7));
    scale = std::min(scale, std::max(1, avail_w/(chars*6)));
    const int text_w = chars*6*scale - scale;
    const int text_h = 7*scale;
    int x0 = std::max(padding,(width-text_w)/2);
    const int y0 = std::max(padding,(height-text_h)/2);
    for(char ch:text){
        auto g=glyph(ch);
        for(int gy=0;gy<7;++gy){
            const int italic_shift = style.italic ? (6-gy)*scale/3 : 0;
            for(int gx=0;gx<5;++gx){
                if(!(g[static_cast<std::size_t>(gy)]&(1<<(4-gx)))) continue;
                for(int sy=0;sy<scale;++sy) for(int sx=0;sx<scale;++sx){
                    const int x=x0+gx*scale+sx+italic_shift, y=y0+gy*scale+sy;
                    if(x>=0&&x<width&&y>=0&&y<height) out.pixels[static_cast<std::size_t>(y)*static_cast<std::size_t>(width)+static_cast<std::size_t>(x)]=1;
                    if(style.bold && x+1<width) out.pixels[static_cast<std::size_t>(y)*static_cast<std::size_t>(width)+static_cast<std::size_t>(x+1)]=1;
                }
            }
        }
        x0 += 6*scale;
        if(x0>=width-padding) break;
    }
    return out;
}
#endif

#if defined(BDMVAUTHOR_HAVE_FONTCONFIG) || defined(BDMVAUTHOR_HAVE_FREETYPE_FONTCONFIG)
#if defined(_WIN32)
void configure_bundled_fontconfig() {
    static const bool configured=[] {
        // Respect an explicit developer/user override.
        if(const wchar_t* existing=_wgetenv(L"FONTCONFIG_FILE");existing&&*existing) return true;
        std::array<wchar_t,32768> module{};
        const DWORD n=GetModuleFileNameW(nullptr,module.data(),static_cast<DWORD>(module.size()));
        if(n==0||n>=module.size()) return true;
        const auto config=std::filesystem::path(std::wstring(module.data(),n)).parent_path()/L"fontconfig"/L"fonts.conf";
        std::error_code ec;
        if(std::filesystem::is_regular_file(config,ec)&&!ec) (void)_wputenv_s(L"FONTCONFIG_FILE",config.c_str());
        return true;
    }();
    (void)configured;
}
#else
void configure_bundled_fontconfig() {}
#endif
struct FcPatternDeleter { void operator()(FcPattern* p) const { if(p) FcPatternDestroy(p); } };
#endif
#if defined(BDMVAUTHOR_HAVE_FREETYPE_FONTCONFIG)
struct FtLibrary { FT_Library value=nullptr; FtLibrary(){ if(FT_Init_FreeType(&value)) throw std::runtime_error("FreeType initialization failed"); } ~FtLibrary(){ if(value) FT_Done_FreeType(value); } };
struct FtFace { FT_Face value=nullptr; explicit FtFace(FT_Library lib,const char* file){ if(FT_New_Face(lib,file,0,&value)) throw std::runtime_error(std::string("cannot load font: ")+file); } ~FtFace(){ if(value) FT_Done_Face(value); } };

std::vector<std::uint32_t> utf8_codepoints(const std::string& s){
    std::vector<std::uint32_t> out;
    for(std::size_t i=0;i<s.size();){
        const auto c=static_cast<unsigned char>(s[i]);
        std::uint32_t cp=0; std::size_t n=0;
        if(c<0x80){cp=c;n=1;}
        else if((c&0xe0)==0xc0){cp=c&0x1f;n=2;}
        else if((c&0xf0)==0xe0){cp=c&0x0f;n=3;}
        else if((c&0xf8)==0xf0){cp=c&0x07;n=4;}
        else {cp='?';n=1;}
        if(i+n>s.size()){out.push_back('?');break;}
        bool ok=true;
        for(std::size_t j=1;j<n;++j){auto cc=static_cast<unsigned char>(s[i+j]);if((cc&0xc0)!=0x80){ok=false;break;}cp=(cp<<6)|(cc&0x3f);}
        out.push_back(ok?cp:static_cast<std::uint32_t>('?'));
        i += ok?n:1;
    }
    return out;
}

struct Metrics { int width=0; int asc=0; int desc=0; };
Metrics metrics(FT_Face face,const std::vector<std::uint32_t>& cps){
    Metrics m; FT_UInt prev=0; const bool kern=FT_HAS_KERNING(face);
    for(auto cp:cps){
        FT_UInt idx=FT_Get_Char_Index(face,cp);
        if(kern&&prev&&idx){FT_Vector k{};FT_Get_Kerning(face,prev,idx,FT_KERNING_DEFAULT,&k);m.width+=static_cast<int>(k.x>>6);}
        if(FT_Load_Glyph(face,idx,FT_LOAD_DEFAULT)==0) m.width+=static_cast<int>(face->glyph->advance.x>>6);
        prev=idx;
    }
    m.asc=static_cast<int>(face->size->metrics.ascender>>6);
    m.desc=static_cast<int>(-(face->size->metrics.descender>>6));
    return m;
}

TextMask freetype_mask(const std::string& text,const ButtonStyle& style,int width,int height,int padding){
    TextMask out{width,height,std::vector<std::uint8_t>(static_cast<std::size_t>(width)*static_cast<std::size_t>(height),0)};
    if(width<=0||height<=0||text.empty()) return out;
    FtLibrary lib; const auto file=resolve_font_file(style.font_family,style.bold,style.italic); FtFace face(lib.value,file.c_str());
    const auto cps=utf8_codepoints(text);
    const int avail_w=std::max(1,width-2*padding), avail_h=std::max(1,height-2*padding);
    int px=std::max(6,style.font_size_px);
    Metrics met;
    while(px>=6){
        if(FT_Set_Pixel_Sizes(face.value,0,static_cast<FT_UInt>(px))) throw std::runtime_error("FreeType font-size setup failed");
        met=metrics(face.value,cps);
        if(met.width<=avail_w && met.asc+met.desc<=avail_h) break;
        --px;
    }
    int pen_x=std::max(padding,(width-met.width)/2);
    const int baseline=std::max(padding+met.asc,(height-(met.asc+met.desc))/2+met.asc);
    FT_UInt prev=0; const bool kern=FT_HAS_KERNING(face.value);
    for(auto cp:cps){
        FT_UInt idx=FT_Get_Char_Index(face.value,cp);
        if(kern&&prev&&idx){FT_Vector k{};FT_Get_Kerning(face.value,prev,idx,FT_KERNING_DEFAULT,&k);pen_x+=static_cast<int>(k.x>>6);}
        if(FT_Load_Glyph(face.value,idx,FT_LOAD_DEFAULT)!=0){prev=idx;continue;}
        if(FT_Render_Glyph(face.value->glyph,FT_RENDER_MODE_MONO)!=0){prev=idx;continue;}
        const auto& bm=face.value->glyph->bitmap;
        const int x0=pen_x+face.value->glyph->bitmap_left;
        const int y0=baseline-face.value->glyph->bitmap_top;
        for(unsigned y=0;y<bm.rows;++y){
            const auto* row = bm.pitch >= 0 ? bm.buffer + static_cast<std::ptrdiff_t>(y) * bm.pitch
                                            : bm.buffer + static_cast<std::ptrdiff_t>(bm.rows - 1U - y) * (-bm.pitch);
            for(unsigned x=0;x<bm.width;++x){
                if((row[x>>3]&(0x80U>>(x&7U)))==0) continue;
                const int dx=x0+static_cast<int>(x),dy=y0+static_cast<int>(y);
                if(dx>=0&&dx<width&&dy>=0&&dy<height) out.pixels[static_cast<std::size_t>(dy)*static_cast<std::size_t>(width)+static_cast<std::size_t>(dx)]=1;
            }
        }
        pen_x+=static_cast<int>(face.value->glyph->advance.x>>6); prev=idx;
    }
    return out;
}
#endif
}

void configure_fontconfig_runtime_environment() {
#if defined(BDMVAUTHOR_HAVE_FONTCONFIG) || defined(BDMVAUTHOR_HAVE_FREETYPE_FONTCONFIG)
    configure_bundled_fontconfig();
#endif
}

bool is_generic_font_family(const std::string& family) {
    std::string lower;
    lower.reserve(family.size());
    for (char c : family) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return lower == "sans-serif" || lower == "sans serif" || lower == "serif" ||
           lower == "monospace" || lower == "system-ui" || lower == "system ui" ||
           lower == "default";
}

#if defined(BDMVAUTHOR_HAVE_FONTCONFIG) || defined(BDMVAUTHOR_HAVE_FREETYPE_FONTCONFIG)
struct InstalledFontRecord { std::string family; std::string file; };

std::string ascii_lower(std::string value) {
    for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

const std::vector<InstalledFontRecord>& installed_font_records() {
    static const std::vector<InstalledFontRecord> records = [] {
        configure_bundled_fontconfig();
        if (!FcInit()) throw std::runtime_error("Fontconfig initialization failed while enumerating installed fonts");
        std::vector<InstalledFontRecord> out;
        std::unique_ptr<FcPattern,FcPatternDeleter> pattern(FcPatternCreate());
        if (!pattern) throw std::runtime_error("Fontconfig pattern allocation failed while enumerating installed fonts");
        FcObjectSet* objects = FcObjectSetBuild(FC_FAMILY, FC_FILE, nullptr);
        if (!objects) throw std::runtime_error("Fontconfig object-set allocation failed while enumerating installed fonts");
        FcFontSet* fonts = FcFontList(nullptr, pattern.get(), objects);
        FcObjectSetDestroy(objects);
        if (!fonts) throw std::runtime_error("Fontconfig could not enumerate installed fonts");
        for (int i = 0; i < fonts->nfont; ++i) {
            FcChar8* file = nullptr;
            if (FcPatternGetString(fonts->fonts[i], FC_FILE, 0, &file) != FcResultMatch || !file) continue;
            for (int fi = 0;; ++fi) {
                FcChar8* family = nullptr;
                if (FcPatternGetString(fonts->fonts[i], FC_FAMILY, fi, &family) != FcResultMatch || !family) break;
                out.push_back({reinterpret_cast<const char*>(family), reinterpret_cast<const char*>(file)});
            }
        }
        FcFontSetDestroy(fonts);
        return out;
    }();
    return records;
}

const InstalledFontRecord* exact_installed_font(const std::string& family) {
    const auto wanted = ascii_lower(family);
    for (const auto& record : installed_font_records())
        if (ascii_lower(record.family) == wanted) return &record;
    return nullptr;
}
#endif

std::string resolve_font_family(const std::string& family, bool bold, bool italic) {
    (void)bold;
    (void)italic;
    namespace fs = std::filesystem;
    if (!family.empty()) {
        std::error_code ec;
        if (fs::is_regular_file(fs::path(family), ec))
            throw std::runtime_error("A font file path was supplied where a concrete installed font family is required: " + family +
                                     ". Choose a suitable installed replacement font in the project.");
        if (is_generic_font_family(family))
            throw std::runtime_error("Generic font family '" + family + "' is not a concrete installed font. Choose a suitable replacement font in the project.");
    }
#if defined(BDMVAUTHOR_HAVE_FONTCONFIG) || defined(BDMVAUTHOR_HAVE_FREETYPE_FONTCONFIG)
    configure_bundled_fontconfig();
    if (!family.empty()) {
        if (const auto* found = exact_installed_font(family)) return found->family;
        throw std::runtime_error("Font '" + family + "' is not installed. Choose a suitable replacement font in the project.");
    }
    static constexpr std::array<const char*,14> fallbacks{{
        "Arial", "Segoe UI", "Tahoma", "Verdana", "Calibri", "Liberation Sans", "DejaVu Sans",
        "Noto Sans", "Carlito", "Ubuntu", "FreeSans", "Nimbus Sans", "TeX Gyre Heros", "Helvetica"
    }};
    for (const char* candidate : fallbacks)
        if (const auto* found = exact_installed_font(candidate)) return found->family;
    throw std::runtime_error("No suitable sans-serif font is installed. Install Arial or another common sans-serif font, or choose a suitable replacement font in the project.");
#else
    if (family.empty())
        throw std::runtime_error("No font resolver is available and no font was specified. Install/build BDMV Author with Fontconfig support and choose a suitable font.");
    throw std::runtime_error("Font '" + family + "' cannot be verified because this BDMV Author build has no Fontconfig support. Choose a suitable replacement font or use a build with Fontconfig support.");
#endif
}

std::string resolve_font_file(const std::string& family, bool bold, bool italic) {
    namespace fs = std::filesystem;
    if (!family.empty()) {
        std::error_code ec;
        const fs::path as_path(family);
        if (fs::is_regular_file(as_path, ec)) return fs::absolute(as_path, ec).string();
    }
#if defined(BDMVAUTHOR_HAVE_FONTCONFIG) || defined(BDMVAUTHOR_HAVE_FREETYPE_FONTCONFIG)
    const std::string concrete = resolve_font_family(family, bold, italic);
    configure_bundled_fontconfig();
    std::unique_ptr<FcPattern,FcPatternDeleter> pat(FcPatternCreate());
    if(!pat) throw std::runtime_error("Fontconfig pattern allocation failed");
    FcPatternAddString(pat.get(),FC_FAMILY,reinterpret_cast<const FcChar8*>(concrete.c_str()));
    FcPatternAddInteger(pat.get(),FC_WEIGHT,bold?FC_WEIGHT_BOLD:FC_WEIGHT_REGULAR);
    FcPatternAddInteger(pat.get(),FC_SLANT,italic?FC_SLANT_ITALIC:FC_SLANT_ROMAN);
    FcConfigSubstitute(nullptr,pat.get(),FcMatchPattern);
    FcDefaultSubstitute(pat.get());
    FcResult result=FcResultNoMatch;
    std::unique_ptr<FcPattern,FcPatternDeleter> match(FcFontMatch(nullptr,pat.get(),&result));
    FcChar8* file=nullptr;
    if(!match || FcPatternGetString(match.get(),FC_FILE,0,&file)!=FcResultMatch || !file)
        throw std::runtime_error("Fontconfig could not resolve installed font '" + concrete + "' to a font file. Choose a suitable replacement font in the project.");
    bool same_family = false;
    for (int i = 0;; ++i) {
        FcChar8* matched_family = nullptr;
        if (FcPatternGetString(match.get(), FC_FAMILY, i, &matched_family) != FcResultMatch || !matched_family) break;
        if (ascii_lower(reinterpret_cast<const char*>(matched_family)) == ascii_lower(concrete)) { same_family = true; break; }
    }
    if (!same_family)
        throw std::runtime_error("Fontconfig substituted a different font for '" + concrete + "'. Choose a suitable replacement font in the project.");
    const fs::path resolved(reinterpret_cast<const char*>(file));
    std::error_code ec;
    if(!fs::is_regular_file(resolved,ec))
        throw std::runtime_error("Installed font '" + concrete + "' resolved to a missing file: " + resolved.string());
    return resolved.string();
#else
    (void)bold;
    (void)italic;
    throw std::runtime_error("A concrete font file path, or a BDMV Author build with Fontconfig support, is required for this font.");
#endif
}

TextMask render_button_text_mask(const std::string& text, const ButtonStyle& style,
                                 int width, int height, int padding) {
#if defined(BDMVAUTHOR_HAVE_FREETYPE_FONTCONFIG)
    return freetype_mask(text,style,width,height,padding);
#else
    return builtin_mask(text,style,width,height,padding);
#endif
}

const char* font_renderer_backend(){
#if defined(BDMVAUTHOR_HAVE_FREETYPE_FONTCONFIG)
    return "freetype-fontconfig";
#else
    return "builtin-5x7-fallback";
#endif
}
}
