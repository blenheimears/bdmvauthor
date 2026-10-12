if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "SOURCE_DIR is required")
endif()

file(READ "${SOURCE_DIR}/CMakeLists.txt" cmake_src)
file(READ "${SOURCE_DIR}/src/author.cpp" author_src)
file(READ "${SOURCE_DIR}/src/gui.cpp" gui_src)
file(READ "${SOURCE_DIR}/src/cli.cpp" cli_src)
file(READ "${SOURCE_DIR}/include/bdmvauthor/author.hpp" author_hpp)
file(READ "${SOURCE_DIR}/third_party/README-eac371enc.md" helper_readme)

foreach(needle
    "third_party/eac371enc/CMakeLists.txt"
    "EAC371_ENABLE_SNDFILE_INPUT OFF"
    "BDMVAUTHOR_HAVE_EAC371ENC=1")
  string(FIND "${cmake_src}" "${needle}" at)
  if(at EQUAL -1)
    message(FATAL_ERROR "optional eac371enc CMake integration missing: ${needle}")
  endif()
endforeach()

foreach(needle
    "audio_worker_codec=\"eac371\""
    "--raw-pcm --sample-rate 48000 --channels 8 --sample-format f32le --channel-layout 7.1 --bitrate "
    "resolved.audio_codec == AudioCodec::Ac3 && channels == 8"
    "resolved.audio_codec == AudioCodec::Ac3 && channels <= 6"
    "-c:a ac3 -b:a "
    "target_max_ac3_71_bitrate_kbps"
    "target_supports_ac3_71_extension(target)"
    "eac371enc raw-PCM 7.1 AC-3/E-AC-3 frontend")
  string(FIND "${author_src}" "${needle}" at)
  if(at EQUAL -1)
    message(FATAL_ERROR "7.1 AC-3 routing regression: ${needle}")
  endif()
endforeach()

foreach(needle
    "std::string eac371enc = \"eac371enc\""
    "TOTAL AC-3 core + E-AC-3 extension rate"
    "total bitrate of the 5.1 AC-3 compatibility core plus the dependent E-AC-3 surround extension")
  set(found FALSE)
  foreach(text IN ITEMS "${author_hpp}" "${cli_src}" "${gui_src}")
    string(FIND "${text}" "${needle}" at)
    if(NOT at EQUAL -1)
      set(found TRUE)
    endif()
  endforeach()
  if(NOT found)
    message(FATAL_ERROR "7.1 AC-3 user-facing integration missing: ${needle}")
  endif()
endforeach()

string(FIND "${helper_readme}" "does **not** bundle eac371enc" not_bundled)
string(FIND "${helper_readme}" "libsndfile is not a BDMV Author dependency" no_sndfile)
if(not_bundled EQUAL -1 OR no_sndfile EQUAL -1)
  message(FATAL_ERROR "third_party eac371enc policy documentation regressed")
endif()

# Do not reject a source tree merely because third_party/eac371enc exists here:
# builders are explicitly allowed to place the optional helper there before
# configuring. Release packaging separately guarantees that BDMV Author's own
# source archives do not contain the helper.
message(STATUS "eac371enc integration source checks passed")
