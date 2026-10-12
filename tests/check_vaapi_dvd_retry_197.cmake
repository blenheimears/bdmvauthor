if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "SOURCE_DIR required")
endif()
file(READ "${SOURCE_DIR}/src/author.cpp" AUTHOR)
file(READ "${SOURCE_DIR}/src/gui.cpp" GUI)
foreach(marker
  "target_is_dvd(p.target) ? \"DVD\""
  "VAAPI tonemap_vaapi attempt (not zscale/Hable)"
  "internal HDR10 filter selection error: VAAPI requested but software tone mapping constructed"
  "the same GPU filters with software decoding"
  "decoding_fallback.hardware_decoder.clear()"
  "const auto output = attempt(decoding_fallback)"
  "const auto output = attempt(fallback)"
  "This is a fallback, NOT the selected filter"
  "Original hardware failure:"
  "hardware_failures += without_hardware_decode.what()"
  "if (tools.filter_fallback_warning) tools.filter_fallback_warning(warning)"
  "std::ofstream marker(fallback_filter_marker(output_base))")
  string(FIND "${AUTHOR}" "${marker}" found)
  if(found EQUAL -1)
    message(FATAL_ERROR "Missing VAAPI/DVD retry and diagnostic safeguard: ${marker}")
  endif()
endforeach()
foreach(marker
  "QMetaObject::invokeMethod(this,[this,diagnostic]"
  "warning_shown->exchange(true)"
  "result.filter_warning_shown_during_build=warning_shown->load()")
  string(FIND "${GUI}" "${marker}" found)
  if(found EQUAL -1)
    message(FATAL_ERROR "Missing immediate GUI fallback notification: ${marker}")
  endif()
endforeach()
message(STATUS "DVD HDR10 VAAPI attempt, decode retry, CPU fallback and immediate warnings guarded")
