# Source-contract regression checks. GUI compilation and end-to-end x265 runs
# require Qt6 and the user's installed encoders, respectively.
foreach(name IN ITEMS include/bdmvauthor/model.hpp include/bdmvauthor/menu_layout.hpp src/gui.cpp src/project_file.cpp src/author.cpp src/cli.cpp)
  file(READ "${SOURCE_DIR}/${name}" contents)
  string(REPLACE "/" "_" var "${name}")
  set("${var}" "${contents}")
endforeach()
file(READ "${SOURCE_DIR}/src/author.cpp" AUTHOR)
file(READ "${SOURCE_DIR}/src/gui.cpp" GUI)
file(READ "${SOURCE_DIR}/src/project_file.cpp" PROJECT)
file(READ "${SOURCE_DIR}/src/cli.cpp" CLI)
file(READ "${SOURCE_DIR}/include/bdmvauthor/model.hpp" MODEL)
file(READ "${SOURCE_DIR}/include/bdmvauthor/menu_layout.hpp" LAYOUT)
foreach(item IN ITEMS
    "MODEL|bool fast_first_pass = true"
    "MODEL|slow-firstpass\",\"no-slow-firstpass"
    "PROJECT|{\"fastFirstPass\", e.fast_first_pass}"
    "PROJECT|o.value(\"fastFirstPass\")"
    "AUTHOR|e.fast_first_pass ? \" --no-slow-firstpass\" : \" --slow-firstpass\""
    "AUTHOR|:no-slow-firstpass=1"
    "AUTHOR|if (pass == 1) params"
    "AUTHOR|if(pass==1 && !e.fast_first_pass)"
    "GUI|passesLayout->addWidget(menu_two_pass_);passesLayout->addWidget(menu_fast_first_)"
    "GUI|titles_->setCellWidget(r,19,fast)"
    "GUI|align_menu_rects(targets"
    "GUI|space_menu_rects(targets"
    "GUI|All buttons and labels"
    "CLI|--fast-first-pass"
    "CLI|--menu-fast-first-pass"
    "LAYOUT|inline bool align_menu_rects"
    "LAYOUT|inline bool space_menu_rects"
    "LAYOUT|if (span > extent) return false"
)
  string(REPLACE "|" ";" parts "${item}")
  list(GET parts 0 source)
  list(GET parts 1 needle)
  string(FIND "${${source}}" "${needle}" found)
  if(found EQUAL -1)
    message(FATAL_ERROR "Missing first-pass/menu-layout integration: ${source}: ${needle}")
  endif()
endforeach()
