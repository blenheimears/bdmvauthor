if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "SOURCE_DIR not supplied")
endif()
file(READ "${SOURCE_DIR}/include/bdmvauthor/model.hpp" MODEL)
file(READ "${SOURCE_DIR}/src/project_file.cpp" PROJECT)
file(READ "${SOURCE_DIR}/src/author.cpp" AUTHOR)
file(READ "${SOURCE_DIR}/src/gui.cpp" GUI)

foreach(needle
    "bool removed = false"
    "audio_stream_removed"
    "subtitle_stream_removed")
  string(FIND "${MODEL}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing stream-removal model marker: ${needle}")
  endif()
endforeach()

foreach(needle
    "kProjectFormatVersion = 29"
    "{\"removed\", stream.removed}"
    "stream.removed = o.value(\"removed\").toBool(false)")
  string(FIND "${PROJECT}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing stream-removal persistence marker: ${needle}")
  endif()
endforeach()

foreach(needle
    "catalog.audio.erase(std::remove_if"
    "audio_stream_removed(p.titles[i], track.source_ordinal)"
    "catalog.subtitles.erase(std::remove_if"
    "subtitle_stream_removed(p.titles[i], track.source_ordinal)"
    "after stream removal")
  string(FIND "${AUTHOR}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing stream-removal authoring marker: ${needle}")
  endif()
endforeach()

foreach(needle
    "Check Remove to omit an embedded audio or subtitle stream entirely"
    "Omit this audio stream from the authored title."
    "Omit this subtitle stream from the authored title."
    "row.value.removed=row.remove->isChecked()"
    "if(row.value.removed||row.value.override_encoding"
    "if(row.value.removed||!row.value.language.empty()"
    "audio_removed(source_ordinal)"
    "subtitle_removed(source_ordinal)")
  string(FIND "${GUI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing Streams-dialog removal marker: ${needle}")
  endif()
endforeach()
