if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "SOURCE_DIR not supplied")
endif()
file(READ "${SOURCE_DIR}/include/bdmvauthor/model.hpp" MODEL)
file(READ "${SOURCE_DIR}/include/bdmvauthor/hdmv.hpp" HDMV_HEADER)
file(READ "${SOURCE_DIR}/src/project_file.cpp" PROJECT)
file(READ "${SOURCE_DIR}/src/author.cpp" AUTHOR)
file(READ "${SOURCE_DIR}/src/hdmv.cpp" HDMV)
file(READ "${SOURCE_DIR}/src/cli.cpp" CLI)
file(READ "${SOURCE_DIR}/src/gui.cpp" GUI)
foreach(needle
  "enum class ChapterMode { SourceOrFiveMinute, Manual, Interval, None }"
  "target_chapter = 0"
  "chapter_mode = ChapterMode::SourceOrFiveMinute"
  "chapter_interval_seconds = 300.0"
)
  string(FIND "${MODEL}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing chapter model marker: ${needle}")
  endif()
endforeach()
foreach(needle
  "kProjectFormatVersion = 24"
  "chapterMode"
  "chapterIntervalSeconds"
  "targetChapter"
  "source-or-5min"
)
  string(FIND "${PROJECT}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing chapter project-file marker: ${needle}")
  endif()
endforeach()
foreach(needle
  "chapter=start_time"
  "resolve_title_chapters"
  "interval_chapter_starts(duration_seconds, 300.0)"
  "dvd_chapter_dispatch_pre_commands"
  "jump title 1 chapter"
  "DVD-Video supports at most 99 chapters per title"
  "chapter_time(0.0)"
  "resolved_title_chapters"
)
  string(FIND "${AUTHOR}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing chapter authoring marker: ${needle}")
  endif()
endforeach()
foreach(needle
  "play_pl_pm"
  "ChapterTargetGpr=4094"
  "title_chapter_counts"
  "action.target_chapter>1U"
)
  string(FIND "${HDMV}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing HDMV chapter-navigation marker: ${needle}")
  endif()
endforeach()
string(FIND "${HDMV_HEADER}" "play_pl_pm" pos)
if(pos EQUAL -1)
  message(FATAL_ERROR "missing HDMV Play_PL_PM API")
endif()
string(FIND "${CLI}" "#include <cmath>" cmath_pos)
if(cmath_pos EQUAL -1)
  message(FATAL_ERROR "chapter CLI duration parser must include <cmath> for std::isfinite")
endif()
foreach(needle
  "--chapters-source"
  "--chapters-manual LIST"
  "--chapters-every DURATION"
  "--no-chapters"
  "--title-button-chapter N C LABEL"
  "--button-action-title-chapter N C"
  "--first-play-title-chapter N C"
)
  string(FIND "${CLI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing chapter CLI marker: ${needle}")
  endif()
endforeach()
foreach(needle
  "Chapter settings"
  "Use source chapters; if none, every 5 minutes"
  "Manual chapter points"
  "New chapter every interval"
  "No additional chapters"
  "Add title/chapter button"
  "RoleChapterMode"
  "RoleChapterInterval"
)
  string(FIND "${GUI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing chapter GUI marker: ${needle}")
  endif()
endforeach()
message(STATUS "chapter policies and chapter-target navigation source checks ok")
