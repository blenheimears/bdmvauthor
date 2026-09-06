file(READ "${SOURCE_DIR}/src/author.cpp" AUTHOR)

foreach(needle
    "-minrate \" << e.video_min_bitrate_kbps << \"k -maxrate \" << peak_bitrate_limit_kbps"
    "constexpr int practical_peak_kbps = 9000"
    "target_max_combined_av_bitrate_kbps(DiscTarget::DvdVideo480p)"
    "dvd_mpeg2_vbr_peak_kbps"
    "menu_audio_rate_kbps, \"DVD menu \""
    "configured_audio_total_kbps, \"DVD title \""
    "const fs::path stderr_file = progress_file.string() + \".stderr\""
    "FFmpeg: \" + diagnostic"
)
  string(FIND "${AUTHOR}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing DVD FFmpeg regression marker: ${needle}")
  endif()
endforeach()

# DVD must remain constrained VBR. Hard CBR was a temporary 0.1.120 workaround
# and must not return.
string(FIND "${AUTHOR}" "k -minrate \" << e.video_bitrate_kbps\n             << \"k -maxrate \" << e.video_bitrate_kbps" HARD_CBR_POS)
if(NOT HARD_CBR_POS EQUAL -1)
  message(FATAL_ERROR "DVD MPEG-2 must not force minrate/maxrate equal to the configured average bitrate")
endif()

string(FIND "${AUTHOR}" "k -minrate \" << e.video_min_bitrate_kbps << \"k -maxrate \" << peak_bitrate_limit_kbps << \"k -bufsize 1835008" DVD_RATE_BLOCK_POS)
if(DVD_RATE_BLOCK_POS EQUAL -1)
  message(FATAL_ERROR "DVD MPEG-2 must emit constrained VBR with the structured minrate, a separate peak maxrate, and the DVD VBV buffer")
endif()
