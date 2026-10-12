if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "SOURCE_DIR not set")
endif()
file(READ "${SOURCE_DIR}/src/author.cpp" AUTHOR)

# 0.1.153 and earlier did not put worker location into the encoded-media hash.
# 0.1.162 restores that spelling as canonical, while retaining lookup-only
# compatibility with the 0.1.154-0.1.160 local-token hashes.
foreach(needle
  "return video_cache_key_impl(fp,e,p,timing,tools,peak_bitrate_limit_kbps,false);"
  "return video_cache_key_impl(fp,e,p,timing,tools,peak_bitrate_limit_kbps,true);"
  "return menu_video_cache_key_impl(input,overlay,e,p,timing,tools,source_interlaced,still,seconds,background_color,"
  "loop_media,peak_bitrate_limit_kbps,false);"
  "loop_media,peak_bitrate_limit_kbps,true);"
  "return audio_cache_key_impl(fp,e,source_ordinal,output_channels,tools,false);"
  "return audio_cache_key_impl(fp,e,source_ordinal,output_channels,tools,true);"
  "return menu_audio_cache_key_impl(fp,e,target,seconds,dvd_elementary,loop_media,tools,false);"
  "return menu_audio_cache_key_impl(fp,e,target,seconds,dvd_elementary,loop_media,tools,true);"
  "if (remote_location_compat) k << \"\\nencodingLocation=local\";"
  "if (remote_location_compat) k<<\"\\nhdmaLocation=local\";"
  "compatible_encoded_cache_hit"
  "if (cache_file_usable(canonical)) return canonical;"
  "cached_audio_peak_kbps"
  "probe_elementary_audio_peak_kbps(tools, cached, probe_output)"
  "metadata.peak_bitrate_kbps = peak"
  "audio_cache_key_remote_location_compat"
  "menu_audio_cache_key_remote_location_compat"
  "video_cache_key_remote_location_compat"
  "menu_video_cache_key_remote_location_compat")
  string(FIND "${AUTHOR}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "pre-remote cache compatibility regression: missing ${needle}")
  endif()
endforeach()

# All remote-affected cache families must go through the canonical+compat path.
string(REGEX MATCHALL "encoded_cache_paths\\(" CACHE_PATH_CALLS "${AUTHOR}")
list(LENGTH CACHE_PATH_CALLS CACHE_PATH_COUNT)
if(CACHE_PATH_COUNT LESS 8)
  message(FATAL_ERROR "expected at least 8 canonical/compat encoded-cache lookup paths, found ${CACHE_PATH_COUNT}")
endif()

# Never make a remote host or current worker selection part of the canonical key.
foreach(forbidden
  [=[if (!remote_location_compat) k << "\nencodingLocation=]=]
  [=[if (!remote_location_compat) k<<"\nhdmaLocation=]=]
  [=[remote_location_compat ? "local" :]=])
  string(FIND "${AUTHOR}" "${forbidden}" pos)
  if(NOT pos EQUAL -1)
    message(FATAL_ERROR "canonical cache identity still contains worker-location state: ${forbidden}")
  endif()
endforeach()

message(STATUS "pre-remote encoded-cache compatibility source checks ok")
