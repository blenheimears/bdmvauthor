if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "SOURCE_DIR not supplied")
endif()
file(READ "${SOURCE_DIR}/src/author.cpp" AUTHOR)
file(READ "${SOURCE_DIR}/include/bdmvauthor/author.hpp" HEADER)
file(READ "${SOURCE_DIR}/include/bdmvauthor/model.hpp" MODEL)
file(READ "${SOURCE_DIR}/src/cli.cpp" CLI)
file(READ "${SOURCE_DIR}/src/gui.cpp" GUI)
file(READ "${SOURCE_DIR}/flake.nix" FLAKE)
foreach(needle
  "Authoring DVD-Video MPEG-2/VOB assets"
  "mux_dvd_program_stream"
  "mplex -f 8"
  "dvdLpcm=raw-big-endian-for-mplex-v2"
  "prepare_dvd_menu_button_visuals"
  "spumux_dvd_menu"
  "spumux_dvd_text_subtitle"
  "DvdNavigationCompiler"
  "call vmgm menu"
  "Building DVD-Video VIDEO_TS structure with dvdauthor"
  "-dvd-video -V"
  "Building DVD-Video UDF 1.02/ISO image with mkisofs -dvd-video"
  "DVD-Video output cannot convert PGS subtitles"
  "DVD-Video supports at most 8 audio streams"
  "DVD-Video supports at most 32 subtitle streams"
)
  string(FIND "${AUTHOR}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing DVD backend marker: ${needle}")
  endif()
endforeach()
foreach(needle "std::string dvdauthor = \"dvdauthor\"" "std::string spumux = \"spumux\"" "std::string mplex = \"mplex\"" "std::string mkisofs = \"mkisofs\"")
  string(FIND "${HEADER}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing DVD tool path: ${needle}")
  endif()
endforeach()

string(FIND "${AUTHOR}" "-f dvd -muxrate 10080k -packetsize 2048" old_ffmpeg_dvd_mux_pos)
if(NOT old_ffmpeg_dvd_mux_pos EQUAL -1)
  message(FATAL_ERROR "DVD backend still uses FFmpeg stream-copy DVD muxing instead of mplex NAV-sector muxing")
endif()

string(FIND "${MODEL}" "return true;" enabled_pos)
if(enabled_pos EQUAL -1)
  message(FATAL_ERROR "DVD target is not authorable")
endif()
foreach(needle "--dvdauthor PATH" "--spumux PATH" "--mplex PATH" "--mkisofs PATH")
  string(FIND "${CLI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing DVD CLI tool option: ${needle}")
  endif()
endforeach()
foreach(needle "pkgs.dvdauthor" "pkgs.mjpegtools" "BDMVAUTHOR_BUILD_BUNDLED_MKISOFS=ON" "mkisofsBundled = true" "mkisofsSourceVersion = \"cdrtools-3.02a09\"")
  string(FIND "${FLAKE}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "Nix runtime tools missing: ${needle}")
  endif()
endforeach()

string(FIND "${AUTHOR}" "if (target_is_dvd(p.target))" dvd_branch_pos)
string(FIND "${AUTHOR}" "auto disc = work / \"disc\";" bluray_disc_pos)
if(dvd_branch_pos EQUAL -1 OR bluray_disc_pos EQUAL -1 OR NOT bluray_disc_pos GREATER dvd_branch_pos)
  message(FATAL_ERROR "Blu-ray BDMV staging must be created only after the DVD early-return branch")
endif()
string(FIND "${AUTHOR}" "<pre>g0 = 0; g1 = 0;</pre>" menu_reset_pos)
if(menu_reset_pos EQUAL -1)
  message(FATAL_ERROR "visible DVD menus must clear stale continuation registers")
endif()

# VMGM may jump to a disc-global title, but the VTS-qualified title form is
# illegal there without jumppads and is rejected by dvdauthor 0.7.2.
string(FIND "${AUTHOR}" "jump title \" << action.target_title << \";" global_title_jump_pos)
if(global_title_jump_pos EQUAL -1)
  message(FATAL_ERROR "DVD VMGM PlayTitle must use disc-global 'jump title N'")
endif()
string(FIND "${AUTHOR}" "jump titleset \" << action.target_title << \" title 1;" illegal_vmgm_title_jump_pos)
if(NOT illegal_vmgm_title_jump_pos EQUAL -1)
  message(FATAL_ERROR "DVD VMGM still contains the illegal 'jump titleset N title 1' form")
endif()

# Audio/subtitle SetSTN commands are also VTS-domain operations.  VMGM stores
# selections in g2/g3 and each title applies them in its pre-command.
foreach(needle
  "out << \"g2 = \" << action.target_stream"
  "out << \"g3 = \" << action.target_stream"
  "out << \"g3 = 33; \""
  "dvd_title_stream_pre_commands"
  "audio = \" << i"
  "subtitle = \" << (64U + i)"
  "subtitle = 62"
  "if (g2 == 0) audio = "
  "if (g3 == 0) subtitle = 62"
  "authored_title.default_audio_stream"
  "authored_title.default_subtitle_stream"
)
  string(FIND "${AUTHOR}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing DVD VMGM/VTS stream-selection legality marker: ${needle}")
  endif()
endforeach()

# DVD raster/timing supports the legal multi-resolution 4:3/16:9 matrix, with all three FFmpeg-style
# timing families and live FFmpeg progress callbacks wired into the DVD branch.
foreach(needle
  "film-dvd"
  "ntsc-dvd"
  "pal-dvd"
  "DVD-Video PAL 576i50"
  "sample_aspect_ratio_for_raster"
  "setsar=\" + sar"
  "dvd_progress_for_video"
  "target_geometry_for_timing"
  "dvd_spumux_format"
  "dvdRaster=multi-mode-v3"
)
  string(FIND "${AUTHOR}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing DVD timing/aspect/progress marker: ${needle}")
  endif()
endforeach()
foreach(needle
  "Film DVD — 23.976p, 720×480"
  "NTSC DVD — 29.97i, 720×480"
  "PAL DVD — 25i, 720×576"
  "p.frame_rate=frame_rate_->currentData()"
)
  string(FIND "${GUI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing GUI DVD frame-rate selector marker: ${needle}")
  endif()
endforeach()

message(STATUS "DVD-Video backend source checks ok")
