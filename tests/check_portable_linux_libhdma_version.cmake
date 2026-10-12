# Copyright (C) 2026 the contributors to the BDMV Author project
if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "SOURCE_DIR is required")
endif()
file(READ "${SOURCE_DIR}/flake.nix" FLAKE)
foreach(needle
    "portable Linux libhdma source must declare VERSION >= 0.0.41"
    "requires libhdma >= 0.0.41 (GCC11 XLL bounds fix)"
    "hdma_patch < 41"
    "-DLIBHDMA_TESTS=OFF -DLIBHDMA_ENABLE_SNDFILE_INPUT=OFF")
  string(FIND "${FLAKE}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "portable libhdma version guard missing: ${needle}")
  endif()
endforeach()
string(FIND "${FLAKE}" "-Wno-stringop-overflow" suppress_pos)
if(NOT suppress_pos EQUAL -1)
  message(FATAL_ERROR "portable Linux must fix libhdma bounds instead of suppressing -Wstringop-overflow")
endif()
message(STATUS "portable Linux libhdma GCC11 version scope: PASS")
