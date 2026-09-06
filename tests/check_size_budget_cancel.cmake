if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "SOURCE_DIR not supplied")
endif()
file(READ "${SOURCE_DIR}/include/bdmvauthor/model.hpp" MODEL)
file(READ "${SOURCE_DIR}/include/bdmvauthor/author.hpp" AUTHOR_H)
file(READ "${SOURCE_DIR}/src/project_file.cpp" PROJECT)
file(READ "${SOURCE_DIR}/src/author.cpp" AUTHOR)
file(READ "${SOURCE_DIR}/src/gui.cpp" GUI)
file(READ "${SOURCE_DIR}/src/cli.cpp" CLI)
file(READ "${SOURCE_DIR}/third_party/udf25mkiso/udf_writer.hpp" UDF_H)
file(READ "${SOURCE_DIR}/third_party/udf25mkiso/udf_writer.cpp" UDF)

foreach(needle
  "disc_capacity_bytes = default_disc_capacity_bytes(DiscTarget::BluRay1080)"
  "bool automatic_video_bitrate = true"
  "4700000000ULL"
  "25000000000ULL"
  "50000000000ULL"
  "disc_uhd_capacity_bytes = default_disc_capacity_bytes(DiscTarget::UltraHdBluRay2160)"
  "disc_dvd_capacity_bytes = default_disc_capacity_bytes(DiscTarget::DvdVideo480p)"
  "new_project_disc_capacity_for_target"
  "disc_capacity_is_unlimited")
  string(FIND "${MODEL}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "size-target model regression: missing ${needle}")
  endif()
endforeach()

foreach(needle
  "kProjectFormatVersion = 24"
  "discCapacityBytes"
  "automaticVideoBitrate"
  "version >= 22"
  "toBool(true) : false"
  "default_disc_capacity_bytes(p.target)")
  string(FIND "${PROJECT}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "size-target project persistence regression: missing ${needle}")
  endif()
endforeach()

foreach(needle
  "struct CapacityPlan"
  "capacity_plan.fixed_overhead_bytes"
  "capacity_plan.mux_factor = 1.04"
  "capacity_plan.safety_bytes = p.disc_capacity_bytes / 50ULL"
  "available_video_kbit_seconds"
  "video_cost_at_waterline"
  "configured manual video bitrate exceeds the selected disc capacity"
  "maximum common video bitrate that fits"
  "Automatic video bitrate target:"
  "!p.automatic_video_bitrate && analysis"
  "completed DVD image exceeds the selected size target"
  "completed Blu-ray image exceeds the selected size target"
  "publish_rendered_image(temporary_output_image, p.output_image)"
  "RenderTempCleanup")
  string(FIND "${AUTHOR}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "capacity-planning/render cleanup regression: missing ${needle}")
  endif()
endforeach()

foreach(needle
  "using CancellationCallback = std::function<bool()>"
  "class AuthorCancelled"
  "CancellationCallback cancelled")
  string(FIND "${AUTHOR_H}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "author cancellation API regression: missing ${needle}")
  endif()
endforeach()

foreach(needle
  "Size target:"
  "Automatic video bitrate"
  "1.46 GB (8 cm single-layer DVD)"
  "2.66 GB (8 cm dual-layer DVD)"
  "7.8 GB (8 cm single-layer BD)"
  "15.6 GB (8 cm dual-layer BD)"
  "25 GB (non-standard UHD target)"
  "7.8 GB (8 cm, non-standard UHD target)"
  "15.6 GB (8 cm dual-layer, non-standard UHD target)"
  "4.7 GB (single-layer DVD)"
  "8.5 GB (dual-layer DVD)"
  "25 GB"
  "50 GB"
  "66 GB"
  "100 GB"
  "Unlimited"
  "defaults/project/blurayDiscCapacityBytes"
  "defaults/project/uhdDiscCapacityBytes"
  "defaults/project/dvdDiscCapacityBytes"
  "Default Blu-ray size"
  "Default UHD size"
  "Default DVD size"
  "size_target_->setMinimumWidth(420)"
  "sizeRow->addWidget(size_target_,1)"
  "progress_=new QProgressBar"
  "Cache settings"
  "Cancel"
  "Render in progress"
  "cancel_render()"
  "close_after_render_"
  "std::make_shared<std::atomic_bool>(false)"
  "catch(const AuthorCancelled&)"
  "p.disc_capacity_bytes=selected_disc_capacity_bytes()"
  "p.automatic_video_bitrate=automatic_bitrate_"
  "automatic_video_bitrate_enabled() const"
  "menu_bitrate_->setEnabled(enc&&!automatic_video_bitrate_enabled())"
  "bitrate->setEnabled(!automatic_video_bitrate_enabled())"
  "connect(automatic_bitrate_,&QCheckBox::toggled")
  string(FIND "${GUI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "size-target/cancel GUI regression: missing ${needle}")
  endif()
endforeach()

foreach(needle
  "--size-target"
  "--automatic-bitrate"
  "--manual-bitrate"
  "parse_size_target"
  "p.automatic_video_bitrate=false")
  string(FIND "${CLI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "size-target CLI regression: missing ${needle}")
  endif()
endforeach()

foreach(needle "std::function<bool()> cancel_requested" "options.cancel_requested && options.cancel_requested()" "UDF image creation cancelled")
  string(FIND "${UDF_H}${UDF}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "UDF cancellation regression: missing ${needle}")
  endif()
endforeach()

message(STATUS "size-target, automatic/manual bitrate budget, cache-settings relocation, and render cancellation checks ok")
