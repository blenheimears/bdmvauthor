if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "SOURCE_DIR not supplied")
endif()
file(READ "${SOURCE_DIR}/include/bdmvauthor/author.hpp" H)
file(READ "${SOURCE_DIR}/src/author.cpp" A)
file(READ "${SOURCE_DIR}/src/gui.cpp" G)
file(READ "${SOURCE_DIR}/src/cli.cpp" C)

foreach(needle
  "std::string failure_type;"
  "std::string diagnostic;")
  string(FIND "${H}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing encode-failure diagnostic API marker: ${needle}")
  endif()
endforeach()

foreach(needle
  "start.failure_type=\"permanent\""
  "start.failure_type=transient?\"transient\":\"permanent\""
  "start.status=\"transient error\";emit(start)"
  "read_all_text(stderr_file)"
  "remote encoding failed after "
  "original compressed bitstream with configured remote decoder"
  "original compressed bitstream with remote software decoder"
  "lossless FFVHUFF relay after remote source-decoder fallback")
  string(FIND "${A}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing remote diagnostic implementation marker: ${needle}")
  endif()
endforeach()

# Failed FFmpeg commands must no longer use the 16 KiB helper.
string(FIND "${A}" "trim_copy(read_small_text(stderr_file))" old_truncated)
if(NOT old_truncated EQUAL -1)
  message(FATAL_ERROR "FFmpeg failure diagnostics are still truncated through read_small_text")
endif()

foreach(needle
  "Encoding details…"
  "Error history / complete tool output"
  "complete FFmpeg stderr"
  "setColumnCount(9)"
  "failure_type"
  "diagnostics_->appendPlainText"
  "encode_progress_dialog_->activateWindow()")
  string(FIND "${G}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing GUI error-history/reopen marker: ${needle}")
  endif()
endforeach()

foreach(needle
  "ev.diagnostic.empty()"
  "error on "
  "ev.diagnostic")
  string(FIND "${C}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing CLI live diagnostic marker: ${needle}")
  endif()
endforeach()

message(STATUS "remote transient/permanent/full-FFmpeg diagnostic and progress-reopen source checks ok")
