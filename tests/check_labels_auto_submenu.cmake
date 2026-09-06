file(READ "${SOURCE_DIR}/include/bdmvauthor/model.hpp" MODEL)
file(READ "${SOURCE_DIR}/src/author.cpp" AUTHOR)
file(READ "${SOURCE_DIR}/src/gui.cpp" GUI)
file(READ "${SOURCE_DIR}/src/cli.cpp" CLI)

foreach(needle
    "enum class MenuOverlayKind { Text, Image }"
    "struct MenuOverlay"
    "std::vector<MenuOverlay> overlays"
    "bool auto_submenu_link = false"
)
  string(FIND "${MODEL}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing label/submenu model marker: ${needle}")
  endif()
endforeach()

foreach(needle
    "prepare_menu_background"
    "Compositing labels for "
    "detail::render_button_text_mask"
    "normalize_button_image(tools, o.image"
    "alpha_over"
)
  string(FIND "${AUTHOR}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing burned-in label authoring marker: ${needle}")
  endif()
endforeach()

foreach(needle
    "Labels burned into menu video"
    "OverlayPropertiesDialog"
    "OverlayPreviewItem"
    "add_text_label()"
    "add_image_label()"
    "link.auto_submenu_link=true"
    "parent->buttons.push_back(std::move(link))"
)
  string(FIND "${GUI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing label/submenu GUI marker: ${needle}")
  endif()
endforeach()

foreach(needle
    "--text-label TEXT"
    "--image-label FILE"
    "--label-font-size PX"
    "link.auto_submenu_link=true"
    "parent.buttons.push_back(std::move(link))"
)
  string(FIND "${CLI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing label/submenu CLI marker: ${needle}")
  endif()
endforeach()
