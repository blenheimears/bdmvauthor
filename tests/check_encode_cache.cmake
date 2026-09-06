file(READ "${SOURCE_DIR}/src/author.cpp" A)
file(READ "${SOURCE_DIR}/src/gui.cpp" G)
file(READ "${SOURCE_DIR}/src/cli.cpp" C)
file(READ "${SOURCE_DIR}/src/project_file.cpp" P)
file(READ "${SOURCE_DIR}/include/bdmvauthor/model.hpp" M)
foreach(needle
  "bdmvauthor-video-cache-v14"
  "bdmvauthor-audio-cache-v7"
  "video_bitrate_kbps"
  "peakBitrateLimitKbps="
  "peakBitrateKbps"
  "x264_preset"
  "e.two_pass"
  "p.frame_rate"
  "ac3_bitrate_kbps"
  "dca_bitrate_kbps"
  "fingerprint_media_file(source)"
  "default_encode_cache_root()"
  "cache_encode_temp_dir"
  "CacheTempDirCleanup"
  "finalize_encoded_cache_file"
  "cache_file_usable"
  "cache_tmp_dir / \"video\""
  "fs::rename(temporary, cached, ec)"
  "No copy fallback is"
  "video_meta_for(video_cache_file"
  "audio_meta_for(audio_cache_file"
  "trap 'exit 130' INT TERM HUP"
  "fs::path(home) / \".cache\" / \"bdmvauthor\" / \"encoded-clips\"")
  string(FIND "${A}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "encode-cache source regression: missing ${needle}")
  endif()
endforeach()

# Cached title encodes must not be produced in the /tmp authoring tree and then
# copied/published afterward.  The old materialize/publish path is forbidden.
foreach(forbidden
  "materialize_cache_file(video_cache_file"
  "materialize_cache_file(audio_cache_file"
  "publish_cache_file(video_output"
  "publish_cache_file(audio_output")
  string(FIND "${A}" "${forbidden}" pos)
  if(NOT pos EQUAL -1)
    message(FATAL_ERROR "encode-cache regression: old post-encode copy path remains: ${forbidden}")
  endif()
endforeach()

# The title encoder calls must precede final-name publication and target private
# cache-side temporary directories. Scope these searches to the title loop so
# menu cache publication (which occurs earlier in author.cpp) cannot satisfy or
# confuse the ordering checks.
string(FIND "${A}" "title_label << \"title \"" TITLE_CACHE_START)
if(TITLE_CACHE_START EQUAL -1)
  message(FATAL_ERROR "could not locate title encode-cache section")
endif()
string(SUBSTRING "${A}" ${TITLE_CACHE_START} -1 TITLE_CACHE_SECTION)
string(FIND "${TITLE_CACHE_SECTION}" "(void)encode_video(tools_, title.source, cache_tmp_base" VIDEO_ENCODE)
string(FIND "${TITLE_CACHE_SECTION}" "finalize_encoded_cache_file(cache_tmp_output, video_cache_file" VIDEO_FINAL)
if(VIDEO_ENCODE EQUAL -1 OR VIDEO_FINAL EQUAL -1 OR VIDEO_FINAL LESS VIDEO_ENCODE)
  message(FATAL_ERROR "video cache final name must be published only after successful cache-directory encoding")
endif()
string(FIND "${TITLE_CACHE_SECTION}" "encoded = encode_audio(tools_, title.source, cache_tmp_dir" AUDIO_ENCODE)
string(FIND "${TITLE_CACHE_SECTION}" "finalize_encoded_cache_file(encoded.path, audio_cache_file" AUDIO_FINAL)
if(AUDIO_ENCODE EQUAL -1 OR AUDIO_FINAL EQUAL -1 OR AUDIO_FINAL LESS AUDIO_ENCODE)
  message(FATAL_ERROR "audio cache final name must be published only after successful cache-directory encoding")
endif()

# Menus use the same transactional cache model. Cache identity is based on the
# rendered video inputs plus encoding/timing/duration, while source-backed menu
# audio is cached separately. DVD and Blu-ray/UHD must both use these paths.
foreach(needle
  "bdmvauthor-menu-video-cache-v5"
  "bdmvauthor-menu-audio-cache-v2"
  "bdmvauthor-dvd-menu-audio-cache-v2"
  "encode_cache_root / \"menu-video\""
  "encode_cache_root / \"menu-audio\""
  "menu_video_cache_key(background, combined_overlay"
  "menu_video_cache_key(menu_background, menu_overlay"
  "Using cached encoded video for DVD menu"
  "Using cached encoded video for menu"
  "Using cached encoded audio for DVD menu"
  "Using cached encoded audio for menu")
  string(FIND "${A}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "menu encode-cache regression: missing ${needle}")
  endif()
endforeach()

foreach(needle "Use encoded-media cache" "Use cached compliance analysis" "Refresh encoded-media cache this build" "Re-run compliance analysis this build" "Cache manager…" "Force re-encode" "Qt::NoBrush")
  string(FIND "${G}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "GUI cache/selection regression: missing ${needle}")
  endif()
endforeach()
string(FIND "${G}" "p->setBrush(Qt::black);p->drawRect(rect_)" bad_fill)
if(NOT bad_fill EQUAL -1)
  message(FATAL_ERROR "selected label still fills its entire rectangle black")
endif()
foreach(needle "--no-cache" "--encode-cache" "--no-encode-cache" "--compliance-cache" "--no-compliance-cache" "--refresh-encode-cache" "--refresh-compliance-cache" "--force-reencode" "--allow-passthrough")
  string(FIND "${C}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "CLI cache regression: missing ${needle}")
  endif()
endforeach()
foreach(needle "useEncodeCache" "useComplianceCache")
  string(FIND "${P}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "project cache preference is not persisted: ${needle}")
  endif()
endforeach()
foreach(needle "bool use_encode_cache = true" "bool use_compliance_cache = true" "bool refresh_encode_cache = false" "bool refresh_compliance_cache = false" "bool force_reencode = false")
  string(FIND "${M}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "project cache model regression: missing ${needle}")
  endif()
endforeach()
message(STATUS "direct transactional encode cache and label selection source checks ok")
