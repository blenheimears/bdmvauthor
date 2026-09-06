file(READ "${SOURCE_DIR}/src/author.cpp" AUTHOR)
file(READ "${SOURCE_DIR}/src/cli.cpp" CLI)
file(READ "${SOURCE_DIR}/src/gui.cpp" GUI)

foreach(NEEDED
    "bdmvauthor-compliance-cache-v11"
    "rules=bd-uhd-primary-av-v11-h264-cpb-delay"
    "sampling=first-middle-last-60s-v1"
    "ComplianceCacheSchema = 11"
    "compliance_cache_key"
    "load_compliance_cache"
    "publish_compliance_cache"
    "compliance_cache_root / \"compliance\""
    "Using cached Blu-ray compliance analysis"
    "fingerprint_media_file(source)"
    "NONCOMPLIANT: video"
    "NONCOMPLIANT: audio track"
    "[cached analysis]"
)
  string(FIND "${AUTHOR}" "${NEEDED}" POS)
  if(POS EQUAL -1)
    message(FATAL_ERROR "missing compliance-cache marker: ${NEEDED}")
  endif()
endforeach()

# A cache hit must precede a fresh analysis call in the title path.
string(FIND "${AUTHOR}" "load_compliance_cache(compliance_cache_file" LOAD_POS)
string(FIND "${AUTHOR}" "analysis = analyze_title_streams" ANALYZE_POS)
if(LOAD_POS EQUAL -1 OR ANALYZE_POS EQUAL -1 OR NOT LOAD_POS LESS ANALYZE_POS)
  message(FATAL_ERROR "compliance cache must be consulted before probing")
endif()

foreach(NEEDED
    "m.rfind(\"NONCOMPLIANT:\", 0) == 0"
    "std::string(160, ' ')"
)
  string(FIND "${CLI}" "${NEEDED}" POS)
  if(POS EQUAL -1)
    message(FATAL_ERROR "noncompliance terminal logging regression: missing ${NEEDED}")
  endif()
endforeach()

foreach(NEEDED
    "Use encoded-media cache"
    "Use cached compliance analysis"
    "Refresh encoded-media cache this build"
    "Re-run compliance analysis this build"
    "Cache settings"
    "cache_enabled_state_"
    "compliance_cache_enabled_state_"
    "m.rfind(\"NONCOMPLIANT:\",0)==0"
    "std::cerr"
)
  string(FIND "${GUI}" "${NEEDED}" POS)
  if(POS EQUAL -1)
    message(FATAL_ERROR "GUI compliance-cache description regression: missing ${NEEDED}")
  endif()
endforeach()

foreach(NEEDED
    "!p.refresh_compliance_cache"
    "p.refresh_encode_cache"
    "p.use_compliance_cache"
    "const fs::path compliance_cache_root"
)
  string(FIND "${AUTHOR}" "${NEEDED}" POS)
  if(POS EQUAL -1)
    message(FATAL_ERROR "missing independent cache-control marker: ${NEEDED}")
  endif()
endforeach()
message(STATUS "compliance verdict cache and independent refresh controls checks ok")
