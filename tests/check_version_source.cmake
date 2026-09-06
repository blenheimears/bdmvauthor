if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "SOURCE_DIR not supplied")
endif()

file(READ "${SOURCE_DIR}/VERSION" VERSION_TEXT)
string(STRIP "${VERSION_TEXT}" VERSION_TEXT)
if(NOT VERSION_TEXT MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
  message(FATAL_ERROR "root VERSION is not semantic x.y.z: '${VERSION_TEXT}'")
endif()

file(READ "${SOURCE_DIR}/CMakeLists.txt" TOP)
file(READ "${SOURCE_DIR}/src/cli.cpp" CLI)
file(READ "${SOURCE_DIR}/src/gui.cpp" GUI)
file(READ "${SOURCE_DIR}/flake.nix" FLAKE)
file(READ "${SOURCE_DIR}/windows/build-msys2-ucrt64.sh" WIN)
file(READ "${SOURCE_DIR}/windows/installer.nsi" INSTALLER)
file(READ "${SOURCE_DIR}/nix/build-portable-linux.sh" PORTABLE)

set(CMAKE_MARKERS
  [=[file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" BDMVAUTHOR_VERSION)]=]
  [=[project(bdmvauthor VERSION "${BDMVAUTHOR_VERSION}" LANGUAGES C CXX)]=]
  [=[BDMVAUTHOR_VERSION=\"${PROJECT_VERSION}\"]=])
foreach(needle IN LISTS CMAKE_MARKERS)
  string(FIND "${TOP}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "CMake VERSION single-source marker missing: ${needle}")
  endif()
endforeach()

set(CONSUMER_MARKERS
  "CLI|BDMVAUTHOR_VERSION"
  "GUI|QString::fromLatin1(BDMVAUTHOR_VERSION)"
  "FLAKE|builtins.readFile ./VERSION"
  [=[FLAKE|tr -d '\r\n' < VERSION]=]
  [=[FLAKE|-DBDMVAUTHOR_VERSION=\"$version\"]=]
  [=[WIN|< "$root/VERSION"]=]
  [=[PORTABLE|< "$root/VERSION"]=])
foreach(pair IN LISTS CONSUMER_MARKERS)
  string(REPLACE "|" ";" fields "${pair}")
  list(GET fields 0 var)
  list(GET fields 1 needle)
  string(FIND "${${var}}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "VERSION consumer marker missing in ${var}: ${needle}")
  endif()
endforeach()

# Runtime/build metadata must not carry an independent BDMV Author release literal.
# Historical release notes and vendored upstream version strings are intentionally
# outside this list.
set(AUDIT_FILES
  CMakeLists.txt
  src/cli.cpp
  src/gui.cpp
  flake.nix
  windows/build-msys2-ucrt64.sh
  windows/installer.nsi
  nix/build-portable-linux.sh
  tests/check_bundled_mkisofs.cmake
  tests/check_bundled_dvdauthor_windows.cmake
  tests/check_windows_sibling_tools.cmake)
foreach(rel IN LISTS AUDIT_FILES)
  file(READ "${SOURCE_DIR}/${rel}" text)
  string(REGEX MATCHALL "0\\.1\\.[0-9]+" matches "${text}")
  foreach(match IN LISTS matches)
    message(FATAL_ERROR "stale/hard-coded BDMV Author release literal '${match}' in ${rel}; derive current version from VERSION")
  endforeach()
endforeach()

message(STATUS "VERSION single-source checks ok: ${VERSION_TEXT}")
