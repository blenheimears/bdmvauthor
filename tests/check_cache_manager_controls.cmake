file(READ "${SOURCE_DIR}/include/bdmvauthor/model.hpp" MODEL)
file(READ "${SOURCE_DIR}/src/author.cpp" AUTHOR)
file(READ "${SOURCE_DIR}/src/gui.cpp" GUI)
file(READ "${SOURCE_DIR}/src/cli.cpp" CLI)
file(READ "${SOURCE_DIR}/src/project_file.cpp" PROJECT)

foreach(needle
  "bool use_encode_cache = true"
  "bool use_compliance_cache = true"
  "bool refresh_encode_cache = false"
  "bool refresh_compliance_cache = false")
  string(FIND "${MODEL}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing independent cache model marker: ${needle}")
  endif()
endforeach()

foreach(needle
  "const fs::path encode_cache_root = p.use_encode_cache"
  "const fs::path compliance_cache_root = p.use_compliance_cache"
  "!p.refresh_encode_cache && cache_file_usable"
  "if (!p.refresh_compliance_cache)"
  "force_reencode = p.force_reencode || title.force_reencode"
  "BDMVAUTHOR_CACHE_META 1"
  "sourceName "
  "target "
  "codec "
  "resolution "
  "frameRate "
  "bitrateKbps "
  "settings "
  "createdUnix "
  "lastUsedUnix "
  "touch_cache_metadata"
  "cache_file_mtime_unix"
  ".tmp-" + std::to_string(nonce))
  string(FIND "${AUTHOR}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing cache engine/metadata marker: ${needle}")
  endif()
endforeach()

# Refreshing one cache must not be used as the condition for the other cache.
string(FIND "${AUTHOR}" "!p.refresh_encode_cache && cache_file_usable" encoded_refresh_pos)
string(FIND "${AUTHOR}" "if (!p.refresh_compliance_cache)" compliance_refresh_pos)
if(encoded_refresh_pos EQUAL -1 OR compliance_refresh_pos EQUAL -1)
  message(FATAL_ERROR "independent cache refresh conditions are missing")
endif()

foreach(needle
  "Cache settings"
  "Use encoded-media cache"
  "Use cached compliance analysis"
  "Refresh encoded-media cache this build"
  "Re-run compliance analysis this build"
  "Cache manager…"
  "class CacheManagerDialog"
  "Original file"
  "Important settings"
  "Last used"
  "Delete selected"
  "Delete all"
  "Automatically delete entries unused for"
  "cache/automaticCleanupEnabled"
  "cache/cleanupUnusedDays"
  "cleanup_cache_unused_for_days"
  "maybe_run_scheduled_cache_cleanup"
  "cache_enabled_state_"
  "compliance_cache_enabled_state_"
  "refresh_encode_cache_state_"
  "refresh_compliance_cache_state_")
  string(FIND "${GUI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing GUI cache manager/control marker: ${needle}")
  endif()
endforeach()

foreach(needle
  "--encode-cache"
  "--no-encode-cache"
  "--compliance-cache"
  "--no-compliance-cache"
  "--refresh-encode-cache"
  "--refresh-compliance-cache")
  string(FIND "${CLI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing CLI independent-cache marker: ${needle}")
  endif()
endforeach()

foreach(needle "kProjectFormatVersion = 24" "useEncodeCache" "useComplianceCache")
  string(FIND "${PROJECT}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing project cache persistence marker: ${needle}")
  endif()
endforeach()

message(STATUS "independent cache controls, metadata manager, and 30-day cleanup policy checks ok")
