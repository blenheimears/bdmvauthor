file(READ "${SOURCE_DIR}/include/bdmvauthor/model.hpp" MODEL)
file(READ "${SOURCE_DIR}/src/hdmv.cpp" HDMV)
file(READ "${SOURCE_DIR}/src/font_renderer.cpp" FONT)
file(READ "${SOURCE_DIR}/src/gui.cpp" GUI)
file(READ "${SOURCE_DIR}/src/cli.cpp" CLI)

foreach(needle
    "struct Rgba"
    "struct ButtonStateStyle"
    "struct ButtonStyle"
    "ButtonStyle default_button_style"
    "bool use_custom_style = false"
    "std::string font_family;"
    "int font_size_px = 60"
    "MenuButtonKind { Text, Image }"
    "Rgba image_highlight_color{128,200,255,112}"
    "ButtonStateStyle active{{255,215,0,255}"
    "ButtonStateStyle selected{{128,200,255,255}"
)
  string(FIND "${MODEL}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing button-style model marker: ${needle}")
  endif()
endforeach()

foreach(needle
    "PaletteBuilder"
    "button.use_custom_style ? button.style : menu.default_button_style"
    "render_button_text_mask"
    "append_palette_entry"
    "Rec.709"
)
  string(FIND "${HDMV}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing styled IG rendering marker: ${needle}")
  endif()
endforeach()

foreach(needle
    "FcFontMatch"
    "FC_FAMILY"
    "FT_Render_Glyph"
    "builtin-5x7-fallback"
)
  string(FIND "${FONT}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing font renderer marker: ${needle}")
  endif()
endforeach()

foreach(needle
    "Label"
    "Edit default text-button style"
    "Override menu default style"
    "Text color"
    "Background"
    "Border color"
    "Active option"
    "Selected"
    "Activated"
    "Image-only button"
    "Unselected image"
    "Selected image (optional)"
    "Automatic highlight"
    "resize_handle().contains(e->pos())"
    "Qt::SizeFDiagCursor"
    "ButtonPreviewItem"
    "ItemIsMovable|ItemIsSelectable|ItemSendsGeometryChanges"
)
  string(FIND "${GUI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing GUI button-style control: ${needle}")
  endif()
endforeach()

foreach(needle
    "--menu-button-font FAMILY"
    "--menu-button-normal-text COLOR"
    "--menu-button-active-text COLOR"
    "--menu-button-active-border COLOR"
    "--menu-button-selected-bg COLOR"
    "--button-style N"
    "--button-label TEXT"
    "--button-font FAMILY"
    "--button-x PX"
    "--button-width PX"
    "--button-image FILE"
    "--button-selected-image FILE"
    "--button-active-text COLOR"
    "--button-active-border COLOR"
    "--button-highlight COLOR"
    "#RRGGBBAA"
)
  string(FIND "${CLI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing CLI button-style option: ${needle}")
  endif()
endforeach()
