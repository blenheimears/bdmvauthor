file(READ "${SOURCE_DIR}/include/bdmvauthor/model.hpp" MODEL)
file(READ "${SOURCE_DIR}/src/author.cpp" AUTHOR)
file(READ "${SOURCE_DIR}/src/hdmv.cpp" HDMV)
file(READ "${SOURCE_DIR}/src/gui.cpp" GUI)
file(READ "${SOURCE_DIR}/src/cli.cpp" CLI)

foreach(needle
    "MenuButtonKind { Text, Image }"
    "std::filesystem::path normal_image"
    "std::filesystem::path selected_image"
    "Rgba image_highlight_color{128,200,255,112}"
)
  string(FIND "${MODEL}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing image-button model marker: ${needle}")
  endif()
endforeach()

foreach(needle
    "normalize_button_image"
    "force_original_aspect_ratio=decrease"
    "-pix_fmt rgba -f rawvideo"
    "prepare_menu_button_images"
    "Preparing image buttons for"
)
  string(FIND "${AUTHOR}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing image normalization marker: ${needle}")
  endif()
endforeach()

foreach(needle
    "image_bitmap"
    "quantize_image_color"
    "highlighted(c, button.image_highlight_color)"
    "button.selected_image.empty()"
)
  string(FIND "${HDMV}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing HDMV image-button rendering marker: ${needle}")
  endif()
endforeach()

foreach(needle
    "ButtonPreviewItem"
    "ItemIsMovable|ItemIsSelectable|ItemSendsGeometryChanges"
    "resize_handle"
    "prepareGeometryChange"
    "Position"
    "Button size"
    "Image-only button"
    "Automatic highlight"
)
  string(FIND "${GUI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing GUI layout/image-button marker: ${needle}")
  endif()
endforeach()

foreach(needle
    "--button-x PX"
    "--button-y PX"
    "--button-width PX"
    "--button-height PX"
    "--button-image FILE"
    "--button-selected-image FILE"
    "--button-highlight COLOR"
)
  string(FIND "${CLI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing CLI layout/image-button marker: ${needle}")
  endif()
endforeach()
