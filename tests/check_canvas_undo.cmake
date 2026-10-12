# This source-level regression is usable without Qt headers. Runtime GUI
# interactions still require a Qt6 build and GUI integration test.
file(READ "${SOURCE_DIR}/src/gui.cpp" GUI)
file(READ "${SOURCE_DIR}/src/project_file.cpp" PROJECT)
foreach(token IN ITEMS
  "layout_dialog_=new QDialog(this)"
  "objects_dialog_=new QDialog(this)"
  "menu_settings_dialog_=new QDialog(this)"
  "rv->addWidget(canvas_host_,1)"
  "split->setStretchFactor(1,4)"
  "titles_->horizontalHeader()->moveSection(19,6)"
  "passesLayout->addWidget(two);passesLayout->addWidget(fast)"
  "passesLayout->addWidget(menu_two_pass_);passesLayout->addWidget(menu_fast_first_)"
  "undo_action_=editMenu->addAction"
  "redo_action_=editMenu->addAction"
  "QKeySequence::Undo"
  "QKeySequence::Redo"
  "history_timer_.setSingleShot(true)"
  "history_.size()>80"
  "void reset_history()"
  "void commit_history()"
  "void restore_history(int index)"
  "void undo_edit()"
  "void redo_edit()"
  "if(history_timer_.isActive())commit_history()"
  "reset_history();mark_clean()"
  "mark_modified();commit_history();refresh_canvas()"
)
  string(FIND "${GUI}" "${token}" found)
  if(found EQUAL -1)
    message(FATAL_ERROR "Missing canvas/undo feature marker: ${token}")
  endif()
endforeach()
# Undo storage must be part of Window only, never part of serialized Project.
foreach(token IN ITEMS "history_" "history_index_" "saved_history_index_")
  string(FIND "${PROJECT}" "${token}" found)
  if(NOT found EQUAL -1)
    message(FATAL_ERROR "History unexpectedly added to project serialization: ${token}")
  endif()
endforeach()
