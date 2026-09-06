file(READ "${SOURCE_DIR}/src/gui.cpp" GUI)
file(READ "${SOURCE_DIR}/include/bdmvauthor/model.hpp" MODEL)

foreach(NEEDED
    "titles_->setSelectionBehavior(QAbstractItemView::SelectRows)"
    "titles_->setSelectionMode(QAbstractItemView::ExtendedSelection)"
    "if(rows.empty()&&titles_->currentRow()>=0)rows.push_back(titles_->currentRow())"
    "remove_title_references(root_menu_,static_cast<std::uint16_t>(n))"
    "auto* m=current_menu();if(!m)m=&root_menu_"
    "b.bounds=clamp_rect_to_aperture(default_bounds(m->buttons.size()),stored_menu_aspect(displayed_target_))"
    "m->buttons.push_back(std::move(b))"
)
  string(FIND "${GUI}" "${NEEDED}" POS)
  if(POS EQUAL -1)
    message(FATAL_ERROR "missing title-management GUI behavior: ${NEEDED}")
  endif()
endforeach()

foreach(BAD
    "b.bounds=default_bounds(root_menu_.buttons.size());root_menu_.buttons.push_back(std::move(b))"
)
  string(FIND "${GUI}" "${BAD}" POS)
  if(NOT POS EQUAL -1)
    message(FATAL_ERROR "new titles are still forced onto the top menu: ${BAD}")
  endif()
endforeach()

foreach(NEEDED
    "inline void remove_title_references(Menu& menu, std::uint16_t removed_title)"
    "button.target_title == removed_title"
    "button.target_title > removed_title"
    "for (auto& child : menu.submenus) remove_title_references(child, removed_title)"
)
  string(FIND "${MODEL}" "${NEEDED}" POS)
  if(POS EQUAL -1)
    message(FATAL_ERROR "missing recursive title-reference cleanup behavior: ${NEEDED}")
  endif()
endforeach()
