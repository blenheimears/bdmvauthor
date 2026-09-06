file(READ "${SOURCE_DIR}/include/bdmvauthor/model.hpp" MODEL)
file(READ "${SOURCE_DIR}/src/author.cpp" AUTHOR)
file(READ "${SOURCE_DIR}/src/compliance.cpp" COMPLIANCE)
file(READ "${SOURCE_DIR}/src/project_file.cpp" PROJECT)
file(READ "${SOURCE_DIR}/src/gui.cpp" GUI)
file(READ "${SOURCE_DIR}/src/cli.cpp" CLI)
file(READ "${SOURCE_DIR}/src/hdmv.cpp" HDMV)

foreach(needle
  "kProjectDesignWidth = 3840"
  "kProjectDesignHeight = 2160"
  "DiscTarget { BluRay1080, UltraHdBluRay2160, DvdVideo480p }"
  "UltraHdBluRay2160: return {3840,2160,1920,1080}"
  "DvdVideo480p: return {720,480,720,480}"
  "VideoCodec { X264, Mpeg2, Hevc }"
  "EncodingProfile uhd_encoding = default_uhd_encoding_profile()"
  "EncodingProfile dvd_encoding = default_dvd_encoding_profile()"
  "encoding_for_target"
)
  string(FIND "${MODEL}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing output-target model marker: ${needle}")
  endif()
endforeach()

foreach(needle
  "--blu-ray-v3"
  "-c:v libx265"
  "uhd-bd=1"
  "V_MPEGH/ISO/HEVC"
  "scale_menu_from_design"
  "geometry.graphics_width"
  "encoding_for_target(menu, p.target)"
  "encoding_for_target(title, p.target)"
  "target_is_uhd(p.target)"
  "colorprim=bt709:transfer=bt709:colormatrix=bt709:range=limited"
  "colorprim=bt2020:transfer=bt2020-10:colormatrix=bt2020nc:range=limited"
  "colorprim=bt2020:transfer=smpte2084:colormatrix=bt2020nc:range=limited"
  "BT.2020/ST 2084 HDR tone-mapped and converted to BT.709 SDR"
  "zscale=pin=bt2020:tin=bt2020-10:min=bt2020nc"
  "source_color_bt709_or_unspecified"
  "partially tagged BT.709 source completed as BT.709 SDR"
  "ffmpeg_setparams_color_input(source) + \"colorspace=all=bt709:range=tv,\""
  "BT.709 full-range source converted to BT.709 limited-range SDR"
)
  string(FIND "${AUTHOR}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing UHD authoring marker: ${needle}")
  endif()
endforeach()

foreach(needle
  "Ultra HD Blu-ray primary video must use HEVC/H.265"
  "Main 10 profile"
  "High Tier"
  "100 Mb/s"
  "V_MPEGH/ISO/HEVC"
  "BT.2020/HDR source requires color conversion to BT.709 for standard Blu-ray output"
  "BT.2020/ST 2084 HDR"
)
  string(FIND "${COMPLIANCE}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing UHD compliance marker: ${needle}")
  endif()
endforeach()

foreach(needle
  "kProjectFormatVersion = 24"
  "blurayEncoding"
  "dvdEncoding"
  "uhdEncoding"
  "target_name"
  "migrate_menu_design_to_4k"
)
  string(FIND "${PROJECT}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing target-aware project-file marker: ${needle}")
  endif()
endforeach()

foreach(needle
  "Current menu — 3840×2160 design space; 4:3 menus use the centered 2880×2160 aperture"
  "Ultra HD Blu-ray 2160p"
  "DVD-Video"
  "RoleHdProfile"
  "RoleUhdProfile"
  "RoleDvdProfile"
  "Build %1 image"
  "%1 image created."
  "return QStringLiteral(\"DVD\")"
)
  string(FIND "${GUI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing target-aware GUI marker: ${needle}")
  endif()
endforeach()

foreach(needle
  "--target MODE"
  "uhd-bluray-2160"
  "dvd-video-480p"
  "--x265-preset PRESET"
)
  string(FIND "${CLI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing target-aware CLI marker: ${needle}")
  endif()
endforeach()

foreach(needle
  "make_index(std::uint16_t title_count,std::uint16_t menu_count,bool version3,std::uint16_t first_play_object)"
  "{'I','N','D','X','0','3','0','0'}"
  "make_movie_object(std::uint16_t title_count,std::uint16_t menu_count,bool version3,"
  "{'M','O','B','J','0','3','0','0'}"
)
  string(FIND "${HDMV}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing HDMV v3 control-file marker: ${needle}")
  endif()
endforeach()

message(STATUS "UHD/1080p/DVD target profiles and DVD target geometry source checks ok")

# Loading a project blocks target_ signals, so populate_project() itself must
# refresh the Build <target> image label after assigning displayed_target_.
string(FIND "${GUI}" "void populate_project(const Project& p)" POPULATE_START)
string(FIND "${GUI}" "bool save_project(bool save_as)" POPULATE_END)
if(POPULATE_START EQUAL -1 OR POPULATE_END EQUAL -1 OR POPULATE_END LESS POPULATE_START)
  message(FATAL_ERROR "could not locate populate_project GUI section")
endif()
math(EXPR POPULATE_LEN "${POPULATE_END} - ${POPULATE_START}")
string(SUBSTRING "${GUI}" ${POPULATE_START} ${POPULATE_LEN} POPULATE_PROJECT)
string(FIND "${POPULATE_PROJECT}" "update_build_button_text();" POPULATE_BUILD_LABEL)
if(POPULATE_BUILD_LABEL EQUAL -1)
  message(FATAL_ERROR "opening a project does not refresh the Build <target> image label")
endif()
