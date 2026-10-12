# SPDX-License-Identifier: Apache-2.0
# Source-level regression for the auto-sizing two-stage pipeline and all-stage progress.
file(READ "${SOURCE_DIR}/src/author.cpp" AUTHOR)
file(READ "${SOURCE_DIR}/third_party/udf25mkiso/udf_writer.cpp" UDF)
file(READ "${SOURCE_DIR}/src/gui.cpp" GUI)

string(FIND "${AUTHOR}" "std::vector<std::optional<PreparedAudioAsset>> prepared_menu_audio" preflight_start)
string(FIND "${AUTHOR}" "if (p.automatic_video_bitrate) {" preflight_guard)
string(FIND "${AUTHOR}" "struct CapacityPlan {" capacity_start)
string(FIND "${AUTHOR}" "// Author one ordinary Blu-ray playlist/clip per menu node" bluray_video_start)
if(preflight_start LESS 0 OR preflight_guard LESS 0 OR capacity_start LESS 0 OR bluray_video_start LESS 0)
  message(FATAL_ERROR "missing audio preflight, capacity model or authoring stages")
endif()
if(NOT preflight_start LESS capacity_start OR NOT capacity_start LESS bluray_video_start)
  message(FATAL_ERROR "all-audio preflight must precede bitrate planning and video encoding")
endif()
foreach(needle
    "prepared_menu_audio[mi] = asset("
    "prepared_title_audio[ti][ai] = asset("
    "capacity_plan.audio_kbit_seconds += static_cast<double>(prepared_menu_audio[mi]->bytes)"
    "capacity_plan.audio_kbit_seconds += static_cast<double>(prepared_title_audio[ti][ai]->bytes)"
    "prepared_menu_audio[mi]"
    "prepared_title_audio[i][ai]"
    "Audio encoding: "
    "audio_percent(before + units"
    "run_with_ffmpeg_progress(command, progress_file, expected_duration"
    "Muxing HDMV menu "
    "Muxing DVD menu "
    "Muxing DVD title "
    "run_with_output_progress"
    "o.progress = [&](double fraction)"
    "Building DVD-Video UDF image"
    "percent = std::clamp(percent, last_percent, 100.0)"
)
  string(FIND "${AUTHOR}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing multi-stage authoring/progress marker: ${needle}")
  endif()
endforeach()
foreach(needle "emit_image_progress(0.0)" "emit_image_progress(0.98" "emit_image_progress(0.99)" "emit_image_progress(1.0)")
  string(FIND "${UDF}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing streamed UDF image progress marker: ${needle}")
  endif()
endforeach()
foreach(needle "progress_->setValue(static_cast<int>(std::lround(v*100.0)))" "statusBar()->showMessage(QString::fromStdString(m))")
  string(FIND "${GUI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "GUI must connect both progress bar and status bar to progress reporter: ${needle}")
  endif()
endforeach()
