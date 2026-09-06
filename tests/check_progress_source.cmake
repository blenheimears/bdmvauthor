file(READ "${SOURCE_DIR}/include/bdmvauthor/author.hpp" AUTHOR_H)
file(READ "${SOURCE_DIR}/src/author.cpp" AUTHOR)
file(READ "${SOURCE_DIR}/src/gui.cpp" GUI)
file(READ "${SOURCE_DIR}/src/cli.cpp" CLI)
file(READ "${SOURCE_DIR}/src/progress.cpp" PROGRESS)

foreach(needle
    "std::function<void(double, const std::string&)>"
)
  string(FIND "${AUTHOR_H}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing high-resolution progress API marker: ${needle}")
  endif()
endforeach()

foreach(needle
    "-nostats -stats_period 0.25 -progress"
    "std::async(std::launch::async"
    "read_ffmpeg_progress_seconds"
    "probe_duration_seconds"
    "total_video_work"
    "completed_video_work"
    "pass 1/2"
    "encode_label << \"Encoding \" << title_label.str()"
    "dvd_progress_for_video"
)
  string(FIND "${AUTHOR}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing live video-progress implementation marker: ${needle}")
  endif()
endforeach()

foreach(needle
    "out_time_us="
    "encoded_seconds / duration_seconds"
)
  string(FIND "${PROGRESS}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing FFmpeg progress parser marker: ${needle}")
  endif()
endforeach()

foreach(needle
    "setRange(0,10000)"
    "QString::number(v,'f',2)+\"%\""
)
  string(FIND "${GUI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing high-resolution GUI progress marker: ${needle}")
  endif()
endforeach()

foreach(needle
    "std::setprecision(2) << pct"
)
  string(FIND "${CLI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing high-resolution CLI progress marker: ${needle}")
  endif()
endforeach()
