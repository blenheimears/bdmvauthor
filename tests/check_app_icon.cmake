if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "SOURCE_DIR not supplied")
endif()
file(READ "${SOURCE_DIR}/src/gui.cpp" GUI)
file(READ "${SOURCE_DIR}/CMakeLists.txt" CMAKE)
file(READ "${SOURCE_DIR}/data/bdmvauthor.desktop" DESKTOP)
file(SIZE "${SOURCE_DIR}/resources/bdmvauthor-icon.svg" ICON_SIZE)
if(ICON_SIZE LESS 10000)
  message(FATAL_ERROR "application icon unexpectedly small/missing")
endif()
file(SIZE "${SOURCE_DIR}/resources/bdmvauthor.ico" WIN_ICON_SIZE)
if(WIN_ICON_SIZE LESS 10000)
  message(FATAL_ERROR "Windows executable icon unexpectedly small/missing")
endif()
file(READ "${SOURCE_DIR}/resources/bdmvauthor.ico" WIN_ICON_HEADER HEX LIMIT 6)
if(NOT WIN_ICON_HEADER STREQUAL "000001000900")
  message(FATAL_ERROR "Windows icon must be an ICO with nine embedded sizes")
endif()
file(READ "${SOURCE_DIR}/resources/bdmvauthor.rc.in" WIN_RC)
file(READ "${SOURCE_DIR}/resources/bdmvauthor-icon.svg" ICON_SVG LIMIT 512)
string(FIND "${ICON_SVG}" "data:image/png;base64," ICON_DATA_POS)
if(ICON_DATA_POS EQUAL -1)
  message(FATAL_ERROR "Linux SVG icon does not embed the approved PNG artwork")
endif()
foreach(needle
  "app_icon_data.inc"
  "bdmvauthor_app_icon"
  "QApplication::setWindowIcon(icon)"
  "w.setWindowIcon(icon)"
  "setIconPixmap(bdmvauthor_app_icon().pixmap(128,128))"
  "setDesktopFileName(QStringLiteral(\"bdmvauthor\"))")
  string(FIND "${GUI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing application-icon GUI marker: ${needle}")
  endif()
endforeach()
foreach(needle
    "resources/bdmvauthor-icon.svg"
    "share/icons/hicolor/scalable/apps"
    "data/bdmvauthor.desktop"
    "share/applications"
    "resources/bdmvauthor.ico"
    "resources/bdmvauthor.rc.in"
    "configure_file("
    "target_sources(bdmvauthor PRIVATE")
  string(FIND "${CMAKE}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing icon install marker: ${needle}")
  endif()
endforeach()
foreach(needle "IDI_BDMVAUTHOR ICON" "@BDMVAUTHOR_WINDOWS_ICON@")
  string(FIND "${WIN_RC}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing Windows executable icon resource marker: ${needle}")
  endif()
endforeach()
foreach(needle "Name=BDMV Author" "Exec=bdmvauthor" "Icon=bdmvauthor" "StartupWMClass=bdmvauthor")
  string(FIND "${DESKTOP}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing desktop icon marker: ${needle}")
  endif()
endforeach()
message(STATUS "application-icon source checks ok")
