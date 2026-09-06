file(READ "${SOURCE_DIR}/src/project_file.cpp" PROJECT_FILE)
file(READ "${SOURCE_DIR}/src/gui.cpp" GUI)
file(READ "${SOURCE_DIR}/CMakeLists.txt" CMAKE)

foreach(needle
    "fingerprint_media_file"
    "MediaFingerprint::SampleBytes"
    "mode\", \"whole-file"
    "first-middle-last-1MiB"
    "middleOffset"
    "lastOffset"
    "firstSha256"
    "middleSha256"
    "lastSha256"
    "fileSize"
    "Media file checksum mismatch"
    "Referenced media file is missing"
    "Select the correct file…"
    "QSaveFile"
    "BDMV Author Project"
)
  string(FIND "${PROJECT_FILE}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing project-file persistence marker: ${needle}")
  endif()
endforeach()

foreach(needle
    "Open project…"
    "Save project"
    "Save project &as…"
    "project.bdmvproject"
    "save_project_file"
    "load_project_file"
    "snapshot_project()"
    "populate_project"
)
  string(FIND "${GUI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing GUI project persistence marker: ${needle}")
  endif()
endforeach()

string(FIND "${CMAKE}" "src/project_file.cpp" pos)
if(pos EQUAL -1)
  message(FATAL_ERROR "GUI target does not compile project_file.cpp")
endif()
