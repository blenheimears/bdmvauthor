file(READ "${SOURCE_DIR}/src/author.cpp" A)
file(READ "${SOURCE_DIR}/src/gui.cpp" G)
file(READ "${SOURCE_DIR}/src/project_file.cpp" P)
file(READ "${SOURCE_DIR}/include/bdmvauthor/author.hpp" H)

foreach(needle
  "storage_.temporary_root.empty() ? default_temporary_root() : storage_.temporary_root"
  "storage_.cache_root.empty() ? default_encode_cache_root() : storage_.cache_root"
  "BDMVAUTHOR_TEMP_DIR"
  "-f null -"
  "--output \" + shq_s(null_device())"
  "move_playlist_clips"
  "fs::rename(src, dst, ec)"
  "if (!p.keep_work_directory) fs::remove_all(md)"
  "if (!p.keep_work_directory) fs::remove_all(td)")
  string(FIND "${A}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "storage/I/O regression: missing author marker: ${needle}")
  endif()
endforeach()


foreach(needle
  "struct StoragePaths"
  "std::filesystem::path cache_root"
  "std::filesystem::path temporary_root")
  string(FIND "${H}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "storage API regression: missing ${needle}")
  endif()
endforeach()
foreach(forbidden
  ".pass1.264"
  ".pass1.265"
  ".pass1.m2v"
  ".pass1.progress"
  ".pass2.progress"
  "copy_playlist_clips")
  string(FIND "${A}" "${forbidden}" pos)
  if(NOT pos EQUAL -1)
    message(FATAL_ERROR "storage/I/O regression: obsolete duplicate temp artifact remains: ${forbidden}")
  endif()
endforeach()

foreach(needle
  "Storage &locations…"
  "storage/cacheDirectory"
  "storage/temporaryDirectory"
  "Cache location"
  "Temporary files location"
  "AuthorEngine e(engine_tools,limits,storage)")
  string(FIND "${G}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "storage settings GUI regression: missing ${needle}")
  endif()
endforeach()

string(FIND "${P}" "{\"workDirectory\"," persisted_work)
if(NOT persisted_work EQUAL -1)
  message(FATAL_ERROR "temporary-files location must not be serialized into project files")
endif()
string(FIND "${P}" "p.work_directory.clear()" ignored_old_work)
if(ignored_old_work EQUAL -1)
  message(FATAL_ERROR "older project workDirectory values must be ignored")
endif()

string(FIND "${H}" "AuthorEngine(ToolPaths tools = {}, AuthoringLimits limits = {}, StoragePaths storage = {})" ctor)
if(ctor EQUAL -1)
  message(FATAL_ERROR "AuthorEngine does not accept runtime storage paths")
endif()

message(STATUS "two-pass null output, canonical temp files, and application storage settings source checks ok")
