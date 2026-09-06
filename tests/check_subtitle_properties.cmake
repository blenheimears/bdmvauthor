file(READ "${SOURCE_DIR}/include/bdmvauthor/model.hpp" model)
file(READ "${SOURCE_DIR}/src/project_file.cpp" project)
file(READ "${SOURCE_DIR}/src/author.cpp" author)
file(READ "${SOURCE_DIR}/src/gui.cpp" gui)
file(READ "${SOURCE_DIR}/src/cli.cpp" cli)
file(READ "${SOURCE_DIR}/src/font_renderer.cpp" font_renderer)
file(READ "${SOURCE_DIR}/include/bdmvauthor/font_renderer.hpp" font_header)
file(READ "${SOURCE_DIR}/CMakeLists.txt" cmake)
foreach(needle
    "struct SubtitleStyle"
    "std::string font_family;"
    "font_size_px = 80"
    "bottom_offset_px = 80"
    "dvd_outline_color"
    "dvd_shadow_offset_x"
    "dvd_horizontal_alignment"
    "dvd_force_display")
  if(NOT model MATCHES "${needle}")
    message(FATAL_ERROR "missing subtitle-style model marker: ${needle}")
  endif()
endforeach()
foreach(needle
    "kProjectFormatVersion = 24"
    "subtitle_style_json"
    "fontSizeUnits"
    "bottomOffsetUnits"
    "overrideStyle"
    "dvdOutlineColor"
    "dvdForceDisplay"
    "subtitleDefaults"
    "migrate_legacy_subtitle_defaults")
  if(NOT project MATCHES "${needle}")
    message(FATAL_ERROR "missing subtitle-style persistence marker: ${needle}")
  endif()
endforeach()
foreach(needle
    "subtitle_tsmuxer_style_meta"
    "font-name="
    "font-color="
    "font-strike-out"
    "bottom-offset="
    "fadein-time="
    "shadow-offset="
    "horizontal-alignment="
    "vertical-alignment="
    "dvd_force_display"
    "spumux_dvd_text_subtitle"
    "dvd_font_file"
    "subtitle_units_to_pixels"
    "resolve_font_family"
    "title subtitle defaults")
  if(NOT author MATCHES "${needle}")
    message(FATAL_ERROR "missing subtitle backend marker: ${needle}")
  endif()
endforeach()

# Default-style text subtitles must still carry an explicit opaque color.
# tsMuxer's Win32 GDI+ renderer treats its built-in 0x00ffffff default as
# transparent, so font-color must be emitted before the override_style early
# return rather than only for advanced style overrides.
string(FIND "${author}" "font-color=\"<<subtitle_argb_hex(st.font_color)" subtitle_color_pos)
string(FIND "${author}" "if(!st.override_style) return meta.str();" subtitle_default_return_pos)
if(subtitle_color_pos EQUAL -1 OR subtitle_default_return_pos EQUAL -1 OR
   subtitle_color_pos GREATER subtitle_default_return_pos)
  message(FATAL_ERROR "text subtitle font-color must be emitted before the default-style early return")
endif()

foreach(needle
    "class SubtitleStyleDialog"
    "Override backend subtitle rendering properties"
    "Font family"
    "Font color"
    "Bold"
    "Italic"
    "Common"
    "Blu-ray / UHD"
    "Line spacing"
    "DVD"
    "Edit default subtitle properties…"
    "defaults/subtitle/style"
    "RoleSubtitleDefaultStyle"
    "Properties…"
    "subtitle_style_map"
    "subtitle_defaults_map.contains"
    "Resolution-independent subtitle units")
  if(NOT gui MATCHES "${needle}")
    message(FATAL_ERROR "missing subtitle properties GUI marker: ${needle}")
  endif()
endforeach()
foreach(needle
    "--subtitle-stream-style"
    "--subtitle-stream-clear-style"
    "--subtitle-file-style"
    "apply_subtitle_style_option"
    "80 su = 80 px at 1080p/UHD")
  if(NOT cli MATCHES "${needle}")
    message(FATAL_ERROR "missing subtitle properties CLI marker: ${needle}")
  endif()
endforeach()

foreach(needle
    "resolve_font_file"
    "FcFontMatch"
    "FC_FILE"
    "resolve_font_family"
    "Choose a suitable replacement font in the project")
  if(NOT font_renderer MATCHES "${needle}" AND NOT font_header MATCHES "${needle}")
    message(FATAL_ERROR "missing DVD subtitle font resolver marker: ${needle}")
  endif()
endforeach()
foreach(needle
    "BDMVAUTHOR_HAVE_FONTCONFIG=1"
    "DVD subtitle families resolve")
  if(NOT cmake MATCHES "${needle}")
    message(FATAL_ERROR "missing Fontconfig build integration marker: ${needle}")
  endif()
endforeach()
