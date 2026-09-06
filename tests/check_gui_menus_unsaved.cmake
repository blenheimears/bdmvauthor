file(READ "${SOURCE_DIR}/src/gui.cpp" GUI)

foreach(needle
  "#include <QCloseEvent>"
  "fileMenu->addAction(\"&Quit\")"
  "QKeySequence::Quit"
  "menuBar()->addMenu(\"&Help\")"
  "helpMenu->addMenu(\"&Help Topics\")"
  "&Getting Started"
  "&Titles and Video Modes"
  "&Chapters and Navigation"
  "&Menus and Buttons"
  "&Building Disc Images"
  "&About BDMV Author"
  "show_help_topic"
  "show_about"
  "bool project_modified_=false"
  "void mark_modified()"
  "void mark_clean()"
  "bool maybe_save_modified()"
  "QMessageBox::Save|QMessageBox::Discard|QMessageBox::Cancel"
  "bool save_project(bool save_as)"
  "void closeEvent(QCloseEvent* event) override"
  "if(maybe_save_modified())event->accept();else event->ignore();"
  "if(!maybe_save_modified())return;"
  "setWindowModified(project_modified_)"
  "suppress_modified_=false;mark_clean()")
  string(FIND "${GUI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "GUI menus/unsaved-project regression: missing ${needle}")
  endif()
endforeach()

foreach(forbidden
  "Discard the current project and create a new one?"
  "void save_project(bool save_as)")
  string(FIND "${GUI}" "${forbidden}" pos)
  if(NOT pos EQUAL -1)
    message(FATAL_ERROR "stale project-close/save behavior remains: ${forbidden}")
  endif()
endforeach()

message(STATUS "GUI File/Help menus and unsaved-project checks ok")
