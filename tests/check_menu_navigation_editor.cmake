file(READ "${SOURCE_DIR}/src/hdmv.cpp" HDMV)
file(READ "${SOURCE_DIR}/src/gui.cpp" GUI)

foreach(needle
    "geometric_neighbor"
    "NavDirection::Up"
    "NavDirection::Down"
    "NavDirection::Left"
    "NavDirection::Right"
    "left=geometric_neighbor"
    "right=geometric_neighbor"
)
  string(FIND "${HDMV}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing geometry-based HDMV navigation marker: ${needle}")
  endif()
endforeach()

foreach(needle
    "class MenuCanvasHost"
    "const double aspect=rect.height()>0.0?rect.width()/rect.height():16.0/9.0"
    "Current menu — 3840×2160 design space; 4:3 menus use the centered 2880×2160 aperture"
    "QGraphicsScene(0,0,kProjectDesignWidth,kProjectDesignHeight)"
    "background.scaled(aperture.size().toSize(),Qt::KeepAspectRatio"
    "scene_->setSceneRect(aperture)"
)
  string(FIND "${GUI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing exact-aspect menu editor marker: ${needle}")
  endif()
endforeach()

string(FIND "${GUI}" "BDMV AUTHOR" phantom_title)
if(NOT phantom_title EQUAL -1)
  message(FATAL_ERROR "editor-only BDMV AUTHOR text must not be present")
endif()
