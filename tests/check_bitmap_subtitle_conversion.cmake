if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "SOURCE_DIR not supplied")
endif()
file(READ "${SOURCE_DIR}/src/author.cpp" AUTHOR)
file(READ "${SOURCE_DIR}/src/gui.cpp" GUI)
file(READ "${SOURCE_DIR}/src/cli.cpp" CLI)
file(READ "${SOURCE_DIR}/include/bdmvauthor/author.hpp" HEADER)
foreach(needle
  "external_subtitle_codec"
  "magic[0] == 'P' && magic[1] == 'G'"
  "magic[0] == 'S' && magic[1] == 'P'"
  "dvd_subtitle"
  "hddvd_subtitle"
  "normalized_bitmap_input"
  "convert_bitmap_with_bdsup2sub"
  "S_HDMV/PGS"
  "spumux_dvd_bitmap_subtitle"
  "dvd_quantize_bitmap"
  "dvd_subpicture_rgba_palette(rgba)"
  "bdn_spumux_timestamp"
  "xoffset"
  "yoffset"
  "forced"
)
  string(FIND "${AUTHOR}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing bitmap subtitle conversion marker: ${needle}")
  endif()
endforeach()
foreach(needle "*.sup *.pgs *.idx *.sub" "tools/bdsup2sub" "bdsup2sub (native bitmap subtitles)")
  string(FIND "${GUI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing bitmap subtitle GUI marker: ${needle}")
  endif()
endforeach()
foreach(needle "--bdsup2sub")
  string(FIND "${CLI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing bitmap subtitle CLI marker: ${needle}")
  endif()
endforeach()
foreach(needle "std::string bdsup2sub =")
  string(FIND "${HEADER}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing bitmap subtitle configuration: ${needle}")
  endif()
endforeach()
message(STATUS "bitmap subtitle conversion code path present")
