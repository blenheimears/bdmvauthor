// SPDX-License-Identifier: Apache-2.0
#include "bdmvauthor/menu_image.hpp"
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;
void expect(bool ok, const std::string& msg) {
    if (!ok) throw std::runtime_error(msg);
}
std::string quote(const std::string& s) {
    // Test paths are generated in a temporary directory; avoid shell metacharacters.
    if (s.find('"') != std::string::npos) throw std::runtime_error("unexpected quote in test path");
    return "\"" + s + "\"";
}
std::array<unsigned char,4> pixel(const std::vector<unsigned char>& rgba, int w, int x, int y) {
    const auto p = (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)) * 4;
    return {rgba[p], rgba[p+1], rgba[p+2], rgba[p+3]};
}
void test_case(const std::string& ffmpeg, const fs::path& work, int w, int h, const char* dar,
               const char* sar, int src_w, int src_h, bool full_frame) {
    const auto filter = bdmvauthor::menu_still_background_fit_filter(w,h,dar,sar);
    const auto output = work / (std::to_string(w)+"x"+std::to_string(h)+"-"+dar+"-"+std::to_string(src_w)+".rgba");
    fs::create_directories(output.parent_path());
    const std::string command = quote(ffmpeg) +
        " -hide_banner -loglevel error -y -filter_threads 1 -f lavfi -i " +
        quote("color=c=red:s="+std::to_string(src_w)+"x"+std::to_string(src_h)+":r=1") +
        " -frames:v 1 -vf " + quote(filter) + " -pix_fmt rgba -f rawvideo " + quote(output.string());
    expect(std::system(command.c_str()) == 0, "FFmpeg rejected menu image fit: "+command);
    std::ifstream in(output, std::ios::binary);
    expect(static_cast<bool>(in), "cannot read generated menu image");
    std::vector<unsigned char> rgba((std::istreambuf_iterator<char>(in)),std::istreambuf_iterator<char>());
    expect(rgba.size() == static_cast<std::size_t>(w)*h*4U, "menu image has incorrect coded size");
    auto red = [&](int x, int y) {
        const auto c = pixel(rgba,w,x,y);
        return c[0]>225 && c[1]<40 && c[2]<40 && c[3]>225;
    };
    auto transparent = [&](int x, int y) { return pixel(rgba,w,x,y)[3]<20; };
    expect(red(w/2,h/2), "center background is not red");
    if (full_frame) {
        // In particular y=12 and y=h-13 must NOT be blank under text overlay coordinates.
        for (auto [x,y] : {std::pair{w/2,12}, {w/2,h-13}, {12,h/2}, {w-13,h/2}})
            expect(red(x,y), "false black bar under menu overlay in "+std::to_string(w)+"x"+std::to_string(h));
    } else if (src_w*3>src_h*4 && std::string(dar)=="4:3") {
        expect(transparent(w/2,10) && transparent(w/2,h-11), "wide source must letterbox in a 4:3 display");
        expect(red(10,h/2) && red(w-11,h/2), "wide source 4:3 must reach horizontal edges");
    } else {
        expect(transparent(10,h/2) && transparent(w-11,h/2), "4:3 source must pillarbox in a 16:9 display");
        expect(red(w/2,10) && red(w/2,h-11), "4:3 source 16:9 must reach vertical edges");
    }
    fs::remove(output);
}
}
int main(int argc, char** argv) {
    try {
        auto filter = bdmvauthor::menu_still_background_fit_filter(720,480,"16:9","32/27");
        expect(filter.find("dar,16/9")!=std::string::npos,"filter must use target display aspect");
        expect(filter.find("480*dar/(32/27)")!=std::string::npos,"filter must use DVD sample aspect");
        bool invalid_rejected=false;
        try { (void)bdmvauthor::menu_still_background_fit_filter(720,480,"16:9","1/1;evil"); }
        catch (const std::invalid_argument&) {invalid_rejected=true;}
        expect(invalid_rejected,"invalid sample aspect must not enter FFmpeg filter");
        if(argc<2) {std::cout<<"menu geometry tests passed (FFmpeg runtime unavailable)\n";return 0;}
        const std::string ffmpeg=argv[1];
        const auto dir = fs::temp_directory_path() / ("bdmvauthor-menu-fit-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(dir);
        try {
            test_case(ffmpeg,dir,720,480,"16:9","32/27",160,90,true);
            test_case(ffmpeg,dir,720,576,"16:9","64/45",160,90,true);
            test_case(ffmpeg,dir,704,480,"16:9","40/33",160,90,true);
            test_case(ffmpeg,dir,720,480,"4:3","8/9",120,90,true);
            test_case(ffmpeg,dir,720,576,"4:3","16/15",120,90,true);
            test_case(ffmpeg,dir,720,480,"4:3","8/9",160,90,false);
            test_case(ffmpeg,dir,720,480,"16:9","32/27",120,90,false);
        } catch (...) { fs::remove_all(dir); throw; }
        fs::remove_all(dir);
        std::cout << "7 real FFmpeg menu image raster tests passed\n";
    } catch (const std::exception& e) {
        std::cerr << "menu image regression: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
