file(READ "${SOURCE_DIR}/src/gui.cpp" GUI)
file(READ "${SOURCE_DIR}/src/font_renderer.cpp" FONT)
file(READ "${SOURCE_DIR}/src/project_file.cpp" PROJECT)
foreach(needle
  "#include <QFontDatabase>"
  "#include \"bdmvauthor/font_renderer.hpp\""
  "bdmvauthor::detail::is_generic_font_family(family)"
  "std::string concrete_gui_font_family(const std::string& family)"
  "Arial"
  "Segoe UI"
  "Tahoma"
  "Liberation Sans"
  "DejaVu Sans"
  "Noto Sans"
  "d.button_style.font_family=concrete_gui_font_family(d.button_style.font_family)"
  "d.label_font_family=concrete_gui_font_family(d.label_font_family)"
  "d.subtitle_style.font_family=concrete_gui_font_family(d.subtitle_style.font_family)")
  string(FIND "${GUI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing concrete default-font resolution marker: ${needle}")
  endif()
endforeach()
foreach(needle
  "resolve_font_family"
  "is_generic_font_family"
  "exact_installed_font"
  "No suitable sans-serif font is installed"
  "Choose a suitable replacement font in the project")
  string(FIND "${FONT}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing strict installed-font resolver marker: ${needle}")
  endif()
endforeach()
foreach(needle
  "concretize_project_fonts"
  "migrate_legacy_subtitle_defaults"
  "version <= 22"
  "Project font unavailable"
  "QFontDialog::getFont"
  "font_replacements_applied"
  "Choose replacement font…"
  "but that font is not installed on this system"
  "Choose a suitable replacement font")
  string(FIND "${PROJECT}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing project font validation marker: ${needle}")
  endif()
endforeach()

string(FIND "${FONT}" "#if !defined(BDMVAUTHOR_HAVE_FREETYPE_FONTCONFIG)\nstd::array<std::uint8_t,7> glyph" fallback_guard_pos)
if(fallback_guard_pos EQUAL -1)
  message(FATAL_ERROR "built-in glyph/mask fallback must be excluded from FreeType+Fontconfig builds")
endif()
string(FIND "${FONT}" "#else\n    return builtin_mask(text,style,width,height,padding);" fallback_use_pos)
if(fallback_use_pos EQUAL -1)
  message(FATAL_ERROR "non-FreeType font-renderer branch must retain builtin_mask fallback")
endif()

message(STATUS "automatic fonts use concrete installed sans-serif families and project fonts are validated")
