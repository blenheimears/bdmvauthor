// SPDX-License-Identifier: Apache-2.0
#include "bdmvauthor/hdmv.hpp"
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#ifdef __unix__
#include <dlfcn.h>
#endif

namespace fs = std::filesystem;

int main() {
#ifndef __unix__
    std::cout << "SKIP: runtime libbluray probe is Unix-only\n";
    return 0;
#else
    void* so = dlopen("libbluray.so.2", RTLD_NOW | RTLD_LOCAL);
    if (!so) so = dlopen("libbluray.so", RTLD_NOW | RTLD_LOCAL);
    if (!so) {
        std::cout << "SKIP: libbluray runtime not installed\n";
        return 0;
    }
    using Init = void* (*)();
    using OpenDisc = int (*)(void*, const char*, const void*);
    using Close = void (*)(void*);
    using Version = void (*)(int*, int*, int*);
    auto init = reinterpret_cast<Init>(dlsym(so, "bd_init"));
    auto open_disc = reinterpret_cast<OpenDisc>(dlsym(so, "bd_open_disc"));
    auto close = reinterpret_cast<Close>(dlsym(so, "bd_close"));
    auto version = reinterpret_cast<Version>(dlsym(so, "bd_get_version"));
    if (!init || !open_disc || !close) {
        dlclose(so);
        throw std::runtime_error("libbluray runtime lacks required public API symbols");
    }

    const auto root = fs::temp_directory_path() / "bdmvauthor-libbluray-runtime";
    fs::remove_all(root);
    fs::create_directories(root / "BDMV/PLAYLIST");
    fs::create_directories(root / "BDMV/CLIPINF");
    fs::create_directories(root / "BDMV/STREAM");
    fs::create_directories(root / "CERTIFICATE/BACKUP");
    bdmvauthor::hdmv::write_control_files(root / "BDMV", 2, 3);

    void* bd = init();
    if (!bd || open_disc(bd, root.string().c_str(), nullptr) != 1) {
        if (bd) close(bd);
        dlclose(so);
        fs::remove_all(root);
        throw std::runtime_error("installed libbluray rejected generated HDMV index/MovieObject files");
    }
    close(bd);

    // A menu-less disc reserves MovieObject 0 as an empty terminal object,
    // uses MovieObjects 1..N for titles, and places the startup sequence after
    // them.  Confirm libbluray accepts that zero-visible-menu layout too.
    const auto menuless_root = fs::temp_directory_path() / "bdmvauthor-libbluray-runtime-menuless";
    fs::remove_all(menuless_root);
    fs::create_directories(menuless_root / "BDMV/PLAYLIST");
    fs::create_directories(menuless_root / "BDMV/CLIPINF");
    fs::create_directories(menuless_root / "BDMV/STREAM");
    fs::create_directories(menuless_root / "CERTIFICATE/BACKUP");
    const auto startup_actions = bdmvauthor::default_menuless_startup_actions(2);
    bdmvauthor::hdmv::MovieObject startup_object;
    startup_object.commands = bdmvauthor::hdmv::make_navigation_sequence_commands(
        startup_actions, {}, 0, 0);
    const std::array<bdmvauthor::hdmv::MovieObject, 1> startup_objects{startup_object};
    bdmvauthor::hdmv::write_control_files(
        menuless_root / "BDMV", 2, 0, false, startup_objects, 3);

    bd = init();
    if (!bd || open_disc(bd, menuless_root.string().c_str(), nullptr) != 1) {
        if (bd) close(bd);
        dlclose(so);
        fs::remove_all(root);
        fs::remove_all(menuless_root);
        throw std::runtime_error("installed libbluray rejected generated menu-less HDMV controls");
    }
    close(bd);
    fs::remove_all(menuless_root);

    // Confirm the probe actually exercises index parsing rather than merely
    // accepting the directory name.
    for (const auto& rel : {fs::path("BDMV/index.bdmv"), fs::path("BDMV/BACKUP/index.bdmv")}) {
        std::fstream f(root / rel, std::ios::in | std::ios::out | std::ios::binary);
        char bad = 'X'; f.write(&bad, 1);
    }
    bd = init();
    const int corrupt_result = bd ? open_disc(bd, root.string().c_str(), nullptr) : 0;
    if (bd) close(bd);
    if (corrupt_result != 0) {
        dlclose(so);
        fs::remove_all(root);
        throw std::runtime_error("libbluray probe unexpectedly accepted a corrupt index.bdmv");
    }

    int major=0, minor=0, micro=0;
    if (version) version(&major, &minor, &micro);
    std::cout << "libbluray accepted generated nested-menu and menu-less HDMV controls";
    if (version) std::cout << " (" << major << "." << minor << "." << micro << ")";
    std::cout << "\n";
    dlclose(so);
    fs::remove_all(root);
    return 0;
#endif
}
