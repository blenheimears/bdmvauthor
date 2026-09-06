file(READ "${SOURCE_DIR}/include/bdmvauthor/model.hpp" MODEL)
file(READ "${SOURCE_DIR}/src/gui.cpp" GUI)
file(READ "${SOURCE_DIR}/src/cli.cpp" CLI)

foreach(needle
  "std::string menu_resolution = \"1920x1080\""
  "std::string menu_uhd_resolution = \"1920x1080\""
  "std::string menu_dvd_resolution = \"720x480\""
  "project.menu_resolution = defaults.menu_resolution"
  "project.menu_uhd_resolution = defaults.menu_uhd_resolution"
  "project.menu_dvd_resolution = defaults.menu_dvd_resolution")
  string(FIND "${MODEL}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing new-project menu-resolution default marker: ${needle}")
  endif()
endforeach()

foreach(needle
  "defaults/menu/blurayResolution"
  "defaults/menu/uhdResolution"
  "defaults/menu/dvdResolution"
  "bluray_menu_resolution_default_"
  "uhd_menu_resolution_default_"
  "dvd_menu_resolution_default_"
  "populate_default_menu_resolution_combo"
  "1920×1080 (recommended for VLC)"
  "warn_uhd_4k_menu_vlc_mouse_bug"
  "VLC currently has a UHD Blu-ray mouse-coordinate bug"
  "currentData().toString()!=QStringLiteral(\"1920x1080\")"
  "displayed_target_==DiscTarget::UltraHdBluRay2160&&stored_menu_resolution(displayed_target_)==\"3840x2160\"")
  string(FIND "${GUI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing GUI menu-resolution default/warning marker: ${needle}")
  endif()
endforeach()

foreach(needle
  "Blu-ray/UHD default 1920x1080"
  "3840x2160 UHD menus trigger a known VLC Blu-ray mouse-coordinate bug"
  "Use --menu-resolution 1920x1080 for VLC compatibility")
  string(FIND "${CLI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing CLI UHD menu compatibility marker: ${needle}")
  endif()
endforeach()

message(STATUS "per-target default menu resolutions and UHD VLC warning checks ok")
