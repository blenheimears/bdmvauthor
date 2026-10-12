file(READ "${SOURCE_DIR}/src/gui.cpp" GUI)

foreach(needle
    "#ifdef Q_OS_MACOS"
    "class PersistentScrollBarStyle final : public QProxyStyle"
    "QStyle::SH_ScrollBar_Transient"
    "titleSettingsHorizontalScrollBar"
    "titles_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff)"
    "connect(tableHorizontalScroll,&QScrollBar::rangeChanged,titleSettingsScroll,syncTitleScrollRange)"
    "connect(tableHorizontalScroll,&QScrollBar::valueChanged,titleSettingsScroll,&QScrollBar::setValue)"
    "connect(titleSettingsScroll,&QScrollBar::valueChanged,tableHorizontalScroll,&QScrollBar::setValue)"
    "titles_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded)"
)
  string(FIND "${GUI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing macOS title-scrollbar marker: ${needle}")
  endif()
endforeach()
