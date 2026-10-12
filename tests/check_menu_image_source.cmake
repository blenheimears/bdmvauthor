file(READ "${SOURCE_DIR}/src/author.cpp" AUTHOR)
file(READ "${SOURCE_DIR}/src/gui.cpp" GUI)
file(READ "${SOURCE_DIR}/src/menu_image.cpp" FIT)
# The background and labels must share one raster, with a separate transform
# only for the background. The text/overlay coordinate transform is untouched.
foreach(needle
    "prepare_menu_background(const ToolPaths& tools, const Menu& menu, const fs::path& work,"
    "menu_still_background_fit_filter("
    "composite_rgba(canvas, W, H, rgba, W, H, 0, 0)"
    "scaled_design_x(o.bounds.x,target_width,aspect_ratio)"
    "scaled_design_y(o.bounds.y,target_height)"
    "prepare_menu_background(tools_, menu, md, dvd_timing)"
    "prepare_menu_background(tools_, video_menu, md, menu_timing)")
  string(FIND "${AUTHOR}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "still-menu alignment regression: ${needle}")
  endif()
endforeach()
foreach(needle "design_aperture(stored_menu_aspect(displayed_target_))" "Qt::KeepAspectRatio" "QPixmap background")
  string(FIND "${GUI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "menu preview alignment regression: ${needle}")
  endif()
endforeach()
foreach(needle "dar," "color=black@0.0" "format=rgba,pad=" "sample_aspect")
  string(FIND "${FIT}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "DVD pixel aspect fit regression: ${needle}")
  endif()
endforeach()
