if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "SOURCE_DIR not supplied")
endif()
file(READ "${SOURCE_DIR}/CMakeLists.txt" CMAKE)
file(READ "${SOURCE_DIR}/include/bdmvauthor/model.hpp" MODEL)
file(READ "${SOURCE_DIR}/include/bdmvauthor/author.hpp" AUTHOR_HPP)
file(READ "${SOURCE_DIR}/src/author.cpp" AUTHOR)
file(READ "${SOURCE_DIR}/src/gui.cpp" GUI)
file(READ "${SOURCE_DIR}/src/cli.cpp" CLI)
file(READ "${SOURCE_DIR}/src/project_file.cpp" PROJECT)
file(READ "${SOURCE_DIR}/flake.nix" FLAKE)
file(READ "${SOURCE_DIR}/nix/build-portable-linux.sh" PORTABLE)
file(READ "${SOURCE_DIR}/windows/build-msys2-ucrt64.sh" WINDOWS)
file(READ "${SOURCE_DIR}/windows/installer.nsi" NSIS)

foreach(needle
  "BDMVAUTHOR_BUILD_BUNDLED_LIBVC1"
  "third_party/libvc1/CMakeLists.txt"
  [=[VERSION_LESS "0.2.35"]=]
  "LIBVC1_BUILD_FRONTEND"
  "LIBVC1_FRONTEND_LINK_STATIC"
  "LIBVC1_BUILD_TESTS"
  "add_subdirectory(third_party/libvc1"
  "target_compile_definitions(bdmvauthor_core PUBLIC BDMVAUTHOR_HAVE_VC1ENC=1)"
  "install(TARGETS vc1enc RUNTIME DESTINATION bin)")
  string(FIND "${CMAKE}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "VC-1 build integration regression: missing ${needle}")
  endif()
endforeach()

foreach(needle
  "enum class VideoCodec { X264=0, Mpeg2=1, Hevc=2, Vc1=3, H264Hardware=4, HevcHardware=5 }"
  "std::string vc1_preset = \"normal\""
  "std::string vc1_advanced_options"
  "codec == VideoCodec::Vc1")
  string(FIND "${MODEL}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "VC-1 model regression: missing ${needle}")
  endif()
endforeach()

foreach(needle
  "std::string vc1enc = \"vc1enc\""
  "vc1enc_options"
  "vc1Preset="
  "--bluray-compat"
  "--format raw"
  [=[output_base.string() + ".vc1"]=]
  [=[if(e.video_codec==VideoCodec::Vc1)return ".vc1"]=]
  "-f yuv4mpegpipe -"
  "--pass 1 --pass-stats"
  "--pass 2 --pass-stats"
  "video_meta_for"
  "V_MS/VFW/WVC1"
  [=[if(e.video_codec==VideoCodec::Vc1) return "V_MS/VFW/WVC1, "+shq(path)+", fps="+timing.tsmuxer_fps+ar+"\n";]=]
  [=[e.video_codec == VideoCodec::Vc1 ? "vc1enc" : "ffmpeg"]=]
  [=[e.video_codec == VideoCodec::Vc1 ? e.vc1_preset : e.x264_preset]=]
  "resolve_sibling_default_tool(std::move(tools_.vc1enc),\"vc1enc\")")
  set(text "${AUTHOR}")
  if(needle STREQUAL "std::string vc1enc = \"vc1enc\"")
    set(text "${AUTHOR_HPP}")
  endif()
  string(FIND "${text}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "VC-1 frontend/process regression: missing ${needle}")
  endif()
endforeach()

# BDMV Author must request only raw primary VC-1 output. It must neither
# request an M2TS primary container nor a duplicate --es-out sidecar.
string(FIND "${AUTHOR}" "--es-out" raw_sidecar_option)
if(NOT raw_sidecar_option EQUAL -1)
  message(FATAL_ERROR "VC-1 raw-output regression: authoring code contains --es-out")
endif()
string(FIND "${AUTHOR}" "--format m2ts" vc1_m2ts_option)
if(NOT vc1_m2ts_option EQUAL -1)
  message(FATAL_ERROR "VC-1 raw-output regression: authoring code still requests --format m2ts")
endif()
foreach(reserved [=["es-out"]=] [=["format"]=] [=["container"]=])
  string(FIND "${MODEL}" "${reserved}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "VC-1 raw-output regression: advanced options no longer reserve ${reserved}")
  endif()
endforeach()

foreach(needle
  "libvc1 VC-1 (Experimental)"
  "vc1enc (libvc1 VC-1, Experimental)"
  "normal\",\"fast"
  "vc1enc does not expose a separate minimum bitrate control"
  "vc1enc two-pass: statistics-only analysis")
  string(FIND "${GUI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "VC-1 GUI regression: missing ${needle}")
  endif()
endforeach()
# Experimental VC-1 must be a normal codec choice. Because vc1enc is an
# external process frontend, discovery/format legality must not depend on
# whether libvc1 happened to be bundled at build time.
string(FIND "${GUI}" "c->addItem(\"libvc1 VC-1 (Experimental)\"" vc1_choice)
string(FIND "${GUI}" [=[resolve_tool(config.vc1enc,"vc1enc",true)]=] vc1_discovery)
string(FIND "${MODEL}" "BDMVAUTHOR_HAVE_VC1ENC" stale_model_gate)
if(vc1_choice EQUAL -1 OR vc1_discovery EQUAL -1 OR NOT stale_model_gate EQUAL -1)
  message(FATAL_ERROR "VC-1 external-process UI/model availability regression")
endif()

foreach(needle
  "--vc1-preset PRESET"
  "--menu-vc1-preset PRESET"
  "vc1" "vc-1" "wvc1"
  "vc1 (libvc1 VC-1, experimental when built)")
  string(FIND "${CLI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "VC-1 CLI regression: missing ${needle}")
  endif()
endforeach()

foreach(needle "vc1Preset" "vc1AdvancedOptions" "\"vc1\"")
  string(FIND "${PROJECT}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "VC-1 project persistence regression: missing ${needle}")
  endif()
endforeach()


foreach(needle
  "libvc1-portable-build"
  "vc1enc-portable"
  "bundled-vc1enc=libvc1-%s"
  "third_party/libvc1/LICENSE"
  "builder-supplied libvc1 was present but vc1enc was not installed")
  string(FIND "${FLAKE}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "VC-1 portable derivation regression: missing ${needle}")
  endif()
endforeach()
foreach(needle
  [=[test -x "$out_path/bin/vc1enc"]=]
  [=[cp -L "$out_path/bin/vc1enc" "$bundle_dir/vc1enc"]=]
  "compiled experimental private sibling")
  string(FIND "${PORTABLE}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "VC-1 portable packaging regression: missing ${needle}")
  endif()
endforeach()
foreach(needle
  "-DBDMVAUTHOR_BUILD_BUNDLED_LIBVC1=ON"
  "builder-supplied libvc1 is present but experimental vc1enc.exe was not built"
  [=[cp -f "$build_dir/vc1enc.exe" "$dist_dir/"]=]
  "libvc1-LICENSE.txt"
  "does not require Debug mode")
  string(FIND "${WINDOWS}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "VC-1 Windows packaging regression: missing ${needle}")
  endif()
endforeach()
string(FIND "${NSIS}" [=[File /r "${STAGE_DIR}\\*"]=] nsis_stage_all)
if(nsis_stage_all EQUAL -1)
  message(FATAL_ERROR "VC-1 Windows installer regression: NSIS no longer installs the complete staged runtime containing vc1enc.exe")
endif()

message(STATUS "experimental optional VC-1/vc1enc integration source checks ok")
