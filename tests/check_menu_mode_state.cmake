file(READ "${SOURCE_DIR}/include/bdmvauthor/model.hpp" MODEL)
file(READ "${SOURCE_DIR}/src/project_file.cpp" PROJECT)
file(READ "${SOURCE_DIR}/src/gui.cpp" GUI)

foreach(needle
  "std::string menu_resolution = \"1920x1080\""
  "std::string menu_uhd_resolution = \"1920x1080\""
  "std::string menu_dvd_resolution = \"720x480\""
  "menu_resolution_for_target"
  "menu_aspect_ratio_for_target")
  string(FIND "${MODEL}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing per-target menu-mode model marker: ${needle}")
  endif()
endforeach()

foreach(needle
  "kProjectFormatVersion = 24"
  "blurayMenuResolution"
  "uhdMenuResolution"
  "dvdMenuResolution"
  "blurayMenuAspectRatio"
  "uhdMenuAspectRatio"
  "dvdMenuAspectRatio"
  "version >= 11"
  "legacy_resolution == \"highest\"")
  string(FIND "${PROJECT}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing per-target menu-mode persistence marker: ${needle}")
  endif()
endforeach()

foreach(needle
  "menu_hd_resolution_=\"1920x1080\""
  "menu_uhd_resolution_=\"1920x1080\""
  "menu_dvd_resolution_=\"720x480\""
  "store_visible_menu_mode"
  "stored_menu_resolution(displayed_target_)"
  "stored_menu_aspect(displayed_target_)"
  "kProjectDesignHeight*4/3"
  "wide_bounds_snapshot_"
  "capture_menu_bounds(root_menu_)"
  "restore_menu_bounds(root_menu_"
  "invalidate_reversible_aspect_positions"
  "clamp_rect_to_aperture"
  "scene_->setSceneRect(aperture)"
  "canvas_host_->refit()")
  string(FIND "${GUI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing menu-mode GUI/bounds marker: ${needle}")
  endif()
endforeach()

string(FIND "${GUI}" "Highest legal" highest_pos)
if(NOT highest_pos EQUAL -1)
  message(FATAL_ERROR "Highest legal should not remain in the GUI")
endif()
message(STATUS "per-target menu mode and reversible 4:3 bounds checks ok")
