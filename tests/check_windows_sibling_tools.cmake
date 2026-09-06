if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "SOURCE_DIR not supplied")
endif()
file(READ "${SOURCE_DIR}/src/gui.cpp" GUI)
file(READ "${SOURCE_DIR}/src/author.cpp" AUTHOR)
file(READ "${SOURCE_DIR}/windows/build-msys2-ucrt64.sh" WINDOWS_BUILD)

foreach(needle
  "resolve_tool(config.ffmpeg,\"ffmpeg\",sibling_external_tools)"
  "resolve_tool(config.ffprobe,\"ffprobe\",sibling_external_tools)"
  "resolve_tool(config.x264,\"x264\",sibling_external_tools)"
  "resolve_tool(config.x265,\"x265\",sibling_external_tools)"
  "resolve_tool(config.dvdauthor,\"dvdauthor\",true)"
  "resolve_tool(config.spumux,\"spumux\",true)"
  "resolve_tool(config.mplex,\"mplex\",true)"
  "resolve_tool(config.mkisofs,\"mkisofs\",true)")
  string(FIND "${GUI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "Windows sibling-tool GUI regression: missing ${needle}")
  endif()
endforeach()

foreach(needle
  "#if defined(_WIN32)"
  "resolve_sibling_default_tool"
  "tools_.ffmpeg=resolve_sibling_default_tool"
  "tools_.ffprobe=resolve_sibling_default_tool"
  "tools_.x264=resolve_sibling_default_tool"
  "tools_.x265=resolve_sibling_default_tool"
  "tools_.dvdauthor=resolve_sibling_default_tool"
  "tools_.spumux=resolve_sibling_default_tool"
  "tools_.mplex=resolve_sibling_default_tool"
  "tools_.mkisofs=resolve_sibling_default_tool")
  string(FIND "${AUTHOR}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "Windows sibling-tool CLI/core regression: missing ${needle}")
  endif()
endforeach()

foreach(needle
  "BDMV Author $version - Windows UCRT64 build"
  "Executables placed beside bdmvauthor.exe are detected")
  string(FIND "${WINDOWS_BUILD}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "Windows sibling-tool packaging regression: missing ${needle}")
  endif()
endforeach()
message(STATUS "sibling external-tool discovery checks ok")
