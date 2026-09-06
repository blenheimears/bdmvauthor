file(READ "${SOURCE_DIR}/src/author.cpp" A)
file(READ "${SOURCE_DIR}/src/compliance.cpp" C)
file(READ "${SOURCE_DIR}/src/cli.cpp" CLI)
file(READ "${SOURCE_DIR}/include/bdmvauthor/model.hpp" M)
file(READ "${SOURCE_DIR}/src/project_file.cpp" P)
file(READ "${SOURCE_DIR}/src/gui.cpp" GUI)
foreach(needle
  "kBluRay1080TimingModes"
  "1080i50 (25 frames/s, 50 fields/s)"
  "1080i59.94 (29.97 frames/s, 59.94 fields/s)"
  "kBluRay1080AvcTimingModes"
  "1080p25 (x264 fake interlaced)"
  "1080p29.97 (x264 fake interlaced)"
  "--fake-interlaced"
  ":fake-interlaced=1"
  "kUhdTimingModes"
  "2160p60"
  "automatic_timing_from_candidates"
  "regular_duplicate"
  "regular_drop"
  "bwdif=mode=send_field"
  "tinterlace=mode=interleave_top"
  "setfield=tff"
  "--tff"
  "timing.frame_rate"
  "timing.temporal_rate"
  "video-cache-v13"
  "raw_elementary"
  "ffprobe cannot derive a reliable average rate"
  "output timing")
  string(FIND "${A}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "frame-rate source regression: missing ${needle}")
  endif()
endforeach()
foreach(needle
  "near(i.frame_rate, 60.0)"
  "near(i.frame_rate, 60000.0/1001.0)")
  string(FIND "${C}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "UHD frame-rate compliance regression: missing ${needle}")
  endif()
endforeach()
# 2160p29.97 must not be in the UHD allowed-mode expression anymore.
string(FIND "${C}" "near(i.frame_rate, 30000.0/1001.0) ||\n              near(i.frame_rate, 50.0)" bad_uhd)
if(NOT bad_uhd EQUAL -1)
  message(FATAL_ERROR "UHD 29.97p accidentally remains in the allowed frame-rate table")
endif()
foreach(needle
  "--frame-rate RATE"
  "p.frame_rate = value()"
  "--title-frame-rate RATE"
  "frame_rate_for_target(t, p.target) = next_title_frame_rate")
  string(FIND "${CLI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "frame-rate CLI regression: missing ${needle}")
  endif()
endforeach()
string(FIND "${M}" "std::string frame_rate = \"auto\"" pos)
if(pos EQUAL -1)
  message(FATAL_ERROR "new projects must default frame rate selection to auto")
endif()
string(FIND "${P}" "if (version <= 4 && p.frame_rate == \"24000/1001\") p.frame_rate = \"auto\"" pos)
if(pos EQUAL -1)
  message(FATAL_ERROR "legacy fixed-23.976 project migration to auto is missing")
endif()


foreach(needle
  "std::string frame_rate = \"inherit\""
  "std::string uhd_frame_rate = \"inherit\""
  "std::string dvd_frame_rate = \"inherit\""
  "frame_rate_for_target(Title& title, DiscTarget target)")
  string(FIND "${M}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "per-title frame-rate model regression: missing ${needle}")
  endif()
endforeach()
foreach(needle
  "blurayFrameRate"
  "uhdFrameRate"
  "dvdFrameRate"
  "kProjectFormatVersion = 24")
  string(FIND "${P}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "per-title frame-rate persistence regression: missing ${needle}")
  endif()
endforeach()
foreach(needle
  "resolve_output_mode"
  "effective_title_frame_rate"
  "DVD-Video cannot mix PAL titles/resolutions with Film/NTSC titles/resolutions on the same disc"
  "dvd_family_from_resolution"
  "NTSC/Film 525/60 family")
  string(FIND "${A}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "mixed-title frame-rate backend regression: missing ${needle}")
  endif()
endforeach()
foreach(needle
  "Menu/default frame rate"
  "Frame rate"
  "Project default"
  "Auto (match title)"
  "Film and NTSC titles may coexist; PAL titles require an all-PAL disc.")
  string(FIND "${GUI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "per-title frame-rate GUI regression: missing ${needle}")
  endif()
endforeach()

# The explicit-rate parser is templated over target tables of different sizes.
# Never index it as though every table were the six-entry UHD table: GCC 15
# diagnoses those otherwise-dead out-of-bounds accesses for the 4-entry BD and
# 2-entry DVD instantiations under optimization.
string(FIND "${A}" "candidates[" fixed_index)
if(NOT fixed_index EQUAL -1)
  message(FATAL_ERROR "frame-rate source regression: explicit candidate parsing must not use fixed array indexes")
endif()
foreach(needle
  "progressive(24000.0/1001.0)"
  "progressive(30000.0/1001.0)"
  "progressive(60.0)"
  "interlaced(50.0)"
  "interlaced(60000.0/1001.0)")
  string(FIND "${A}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "frame-rate alias regression: missing property-based lookup ${needle}")
  endif()
endforeach()

message(STATUS "automatic Blu-ray/UHD frame-rate policy source checks ok")
