// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <string>

namespace bdmvauthor {
// FFmpeg -vf expression for fitting a still menu background to the target's
// *display* area, then padding it to the target coded-pixel raster.
// Labels and buttons are already laid out in that raster; do not scale them
// again when fitting a menu background.
std::string menu_still_background_fit_filter(int coded_width, int coded_height,
                                              const std::string& display_aspect,
                                              const std::string& sample_aspect);
} // namespace bdmvauthor
