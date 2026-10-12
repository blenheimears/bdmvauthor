# Regression for a filter that appears in ffmpeg -filters, but was incorrectly
# rejected by an independent availability preflight in 0.1.195.
if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "SOURCE_DIR required")
endif()
file(READ "${SOURCE_DIR}/src/author.cpp" AUTHOR)
file(READ "${SOURCE_DIR}/src/gui.cpp" GUI)
if(AUTHOR MATCHES "preflight_local_hardware_filters\\(tools,color_plan,still\\)")
  message(FATAL_ERROR "The stale preflight still vetoes filters before FFmpeg can run them")
endif()
foreach(marker
  "hardware_processing_filters(tools,color_plan"
  "selected_filter_device_options(tools,color_plan)"
  "Original hardware failure:"
  "fallback.tonemap_filter_backend = \"software\""
  "tonemap_vaapi=format=nv12:p=bt709:t=bt709:m=bt709")
  string(FIND "${AUTHOR}" "${marker}" found)
  if(found EQUAL -1)
    message(FATAL_ERROR "Required GPU-to-software fallback path missing: ${marker}")
  endif()
endforeach()
foreach(marker
  "ffmpeg_filter_help_confirms"
  "supplement_local_ffmpeg_filters"
  "supplement_remote_ffmpeg_filters"
  "-h"
  "filter="
  "Could not verify tonemap_vaapi")
  string(FIND "${GUI}" "${marker}" found)
  if(found EQUAL -1)
    message(FATAL_ERROR "Targeted filter probe missing: ${marker}")
  endif()
endforeach()
# Check an actual FFmpeg binary when available. A build may legitimately lack
# tonemap_vaapi; the behavior under test is that help and list agree if present.
find_program(FFMPEG_BIN ffmpeg)
if(FFMPEG_BIN)
  execute_process(COMMAND "${FFMPEG_BIN}" -hide_banner -filters
    RESULT_VARIABLE listing_rc OUTPUT_VARIABLE listing ERROR_VARIABLE listing_err TIMEOUT 15)
  if(NOT listing_rc EQUAL 0)
    message(FATAL_ERROR "FFmpeg -filters failed: ${listing_err}")
  endif()
  if(listing MATCHES "[ \\t]tonemap_vaapi[ \\t]")
    execute_process(COMMAND "${FFMPEG_BIN}" -hide_banner -h filter=tonemap_vaapi
      RESULT_VARIABLE help_rc OUTPUT_VARIABLE help_output ERROR_VARIABLE help_err TIMEOUT 15)
    string(FIND "${help_output}" "Filter tonemap_vaapi" found_help)
    if(NOT help_rc EQUAL 0 OR found_help EQUAL -1)
      message(FATAL_ERROR "FFmpeg lists tonemap_vaapi but per-filter help did not confirm it: ${help_err}")
    endif()
  endif()
endif()
message(STATUS "Filter-list false negative no longer blocks runtime FFmpeg and software fallback")
