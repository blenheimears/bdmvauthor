if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "SOURCE_DIR not supplied")
endif()
file(READ "${SOURCE_DIR}/include/bdmvauthor/model.hpp" MODEL)
file(READ "${SOURCE_DIR}/src/gui.cpp" GUI)
file(READ "${SOURCE_DIR}/src/project_file.cpp" PROJECT)
file(READ "${SOURCE_DIR}/src/author.cpp" AUTHOR)

foreach(needle
  "std::filesystem::path output_image;"
  "if (p.output_image.empty()) throw std::runtime_error(\"output image path is empty\")")
  string(FIND "${MODEL}${GUI}${AUTHOR}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "explicit output-image regression: missing ${needle}")
  endif()
endforeach()

string(FIND "${MODEL}" "output_image = \"disc.iso\"" pos)
if(NOT pos EQUAL -1)
  message(FATAL_ERROR "new Project still has a default disc.iso output path")
endif()
string(FIND "${MODEL}" "struct NewProjectDefaults {" defaults_pos)
string(FIND "${MODEL}" "inline void apply_new_project_defaults" apply_pos)
string(SUBSTRING "${MODEL}" ${defaults_pos} 1800 DEFAULTS_BLOCK)
string(FIND "${DEFAULTS_BLOCK}" "output_image" pos)
if(NOT pos EQUAL -1)
  message(FATAL_ERROR "NewProjectDefaults must not contain an output image path")
endif()

foreach(needle
  "settings.remove(\"defaults/project/outputImage\")"
  "Disc image files are chosen separately for each project."
  "output_->setPlaceholderText(\"Choose a disc image file…\")"
  "output_->clear();"
  "bool choose_output_image()"
  "if(output_->text().trimmed().isEmpty()&&!choose_output_image())return;"
  "connect(outBrowse,&QPushButton::clicked,this,[this]{choose_output_image();});")
  string(FIND "${GUI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "explicit output-image GUI regression: missing ${needle}")
  endif()
endforeach()
string(FIND "${GUI}" "Default image name" pos)
if(NOT pos EQUAL -1)
  message(FATAL_ERROR "Settings still exposes a default image name")
endif()

foreach(needle
  "{\"outputImage\", path_qstring(project.output_image)}"
  "p.output_image = fs_path(o.value(\"outputImage\").toString());")
  string(FIND "${PROJECT}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "project output-image persistence regression: missing ${needle}")
  endif()
endforeach()
string(FIND "${PROJECT}" "toString(\"disc.iso\")" pos)
if(NOT pos EQUAL -1)
  message(FATAL_ERROR "project loader still silently defaults outputImage to disc.iso")
endif()
