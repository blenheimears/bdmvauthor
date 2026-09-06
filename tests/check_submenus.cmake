file(READ "${SOURCE_DIR}/include/bdmvauthor/model.hpp" MODEL)
file(READ "${SOURCE_DIR}/src/author.cpp" AUTHOR)
file(READ "${SOURCE_DIR}/src/hdmv.cpp" HDMV)
file(READ "${SOURCE_DIR}/src/gui.cpp" GUI)
file(READ "${SOURCE_DIR}/src/cli.cpp" CLI)

foreach(needle
    "MenuButtonTargetKind { Title, Menu, AudioTrack, SubtitleTrack, SubtitleOff }"
    "std::string target_menu_id"
    "bool inherit_background_image = true"
    "bool inherit_background_color = true"
    "bool inherit_button_style = true"
    "bool inherit_encoding = true"
    "bool inherit_audio = true"
    "bool inherit_duration = true"
    "Rgba background_color{0,0,0,255}"
    "bool auto_back_button = true"
    "MenuButton back_button"
    "std::vector<Menu> submenus"
)
  string(FIND "${MODEL}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing submenu model marker: ${needle}")
  endif()
endforeach()

foreach(needle
    "resolve_menu_tree"
    "source.inherit_background_image"
    "source.inherit_background_color"
    "source.inherit_button_style"
    "source.inherit_encoding"
    "source.inherit_audio"
    "source.inherit_duration"
    "rgb_hex(background_color)"
    "geometry.video_width"
    "back.target_menu_id = parent_id"
    "for (std::size_t mi = 0; mi < menus.size(); ++mi)"
    "menu_count + static_cast<unsigned>(i)"
    "write_control_files(disc / \"BDMV\""
)
  string(FIND "${AUTHOR}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing submenu authoring marker: ${needle}")
  endif()
endforeach()

foreach(needle
    "menu_count+i"
    "for(std::uint16_t i=1;i<menu_count;i++)"
    "MenuButtonTargetKind::Menu"
    "jump_object(it->second)"
)
  string(FIND "${HDMV}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing submenu HDMV navigation marker: ${needle}")
  endif()
endforeach()

foreach(needle
    "Add submenu"
    "Remove menu"
    "Add title/chapter button"
    "Add menu link"
    "Automatically add Back button"
    "Background color"
    "Inherit from parent"
    "Inherit encoding from parent"
    "a title may be linked from any number of menu pages"
    "std::vector<TargetChoice> target_choices() const"
)
  string(FIND "${GUI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing submenu GUI marker: ${needle}")
  endif()
endforeach()

foreach(needle
    "--submenu ID"
    "--end-submenu"
    "--menu-link ID LABEL"
    "--title-button N LABEL"
    "--menu-background-color C"
    "--no-auto-back"
)
  string(FIND "${CLI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing submenu CLI marker: ${needle}")
  endif()
endforeach()
