if(NOT DEFINED SOURCE_DIR OR NOT DEFINED WORK_DIR)
  message(FATAL_ERROR "SOURCE_DIR/WORK_DIR not supplied")
endif()
file(READ "${SOURCE_DIR}/src/author.cpp" AUTHOR)
file(READ "${SOURCE_DIR}/src/gui.cpp" GUI)
file(READ "${SOURCE_DIR}/src/cli.cpp" CLI)

# Bitmap subtitle events must share a bounded DVD palette; a fresh arbitrary
# palette for each event can exhaust dvdauthor's sixteen PGC palette entries.
# The palette reduction must not run palettegen before preserving foreground:
# a small white glyph can otherwise vanish behind a large dark backdrop.
foreach(needle
  "dvd_subpicture_rgba_palette(rgba)"
  "dvd_subpicture_png_dimensions(original)"
  "-pixel_format rgba -video_size "
  "spumux_dvd_bitmap_subtitle")
  string(FIND "${AUTHOR}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "DVD subtitle foreground-preserving palette safeguard missing: ${needle}")
  endif()
endforeach()

# Work directories are removed by RAII on normal completion, error or cancel;
# only an explicit keep-work/work-dir selection disables this behavior.
foreach(needle
  "bool keep_work_directory = false;"
  "if (!keep_work_directory && !work_dir.empty())"
  "RenderTempCleanup render_cleanup{work, temporary_output_image, p.keep_work_directory};")
  string(FIND "${AUTHOR}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "temporary-file failure cleanup missing: ${needle}")
  endif()
endforeach()

foreach(needle
  [=[c->addItem("DCA",]=]
  [=[c->addItem("THD + AC-3 core (experimental)",]=]
  [=["FFmpeg DCA"]=]
  [=["FFmpeg THD"]=])
  string(FIND "${GUI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "codec display label not updated: ${needle}")
  endif()
endforeach()
foreach(needle "--audio-codec MODE" "thd-ac3" "truehd-ac3" "--keep-work" "--work DIR")
  string(FIND "${CLI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "stable CLI/cleanup interface changed: ${needle}")
  endif()
endforeach()

# Pixel-by-pixel palette correctness, including white foreground, black
# outline, transparency and many differently colored caption events, is tested
# by bdmvauthor-dvd-subpicture-palette-runtime (compiled C++).
message(STATUS "DVD palette, cleanup and codec labels verified")
