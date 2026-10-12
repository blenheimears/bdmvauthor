// SPDX-License-Identifier: Apache-2.0
#include "bdmvauthor/menu_image.hpp"
#include <stdexcept>

namespace bdmvauthor {
std::string menu_still_background_fit_filter(int coded_width, int coded_height,
                                              const std::string& display_aspect,
                                              const std::string& sample_aspect) {
    if (coded_width <= 0 || coded_height <= 0)
        throw std::invalid_argument("invalid menu raster dimensions");
    // The only disc menu DARs are 16:9 and 4:3. Do not accept an arbitrary
    // string in FFmpeg filter expressions.
    if (display_aspect != "16:9" && display_aspect != "4:3")
        throw std::invalid_argument("unsupported menu display aspect");
    std::string sar;
    for (char c : sample_aspect) {
        if (c == '/') sar += '/';
        else if (c >= '0' && c <= '9') sar += c;
        else throw std::invalid_argument("invalid menu sample aspect");
    }
    if (sar.empty() || sar.find('/') == std::string::npos)
        throw std::invalid_argument("invalid menu sample aspect");

    const std::string w = std::to_string(coded_width);
    const std::string h = std::to_string(coded_height);
    const std::string target_dar = display_aspect == "4:3" ? "4/3" : "16/9";
    // Compare source DAR (including any source SAR) with the requested DVD
    // display DAR. Transform the *active* image into coded pixels accounting
    // for the destination SAR, then use transparent padding. A 16:9 square-
    // pixel source fills a 720x480 anamorphic 16:9 target, not 720x405.
    return "scale=w='if(gt(dar," + target_dar + ")," + w +
           ",max(2,trunc((" + h + "*dar/(" + sar + "))/2)*2))'"
           ":h='if(gt(dar," + target_dar + "),max(2,trunc((" + w + "*(" + sar + ")/dar)/2)*2)," + h +
           ")':flags=lanczos,format=rgba,pad=" + w + ":" + h +
           ":(ow-iw)/2:(oh-ih)/2:color=black@0.0";
}
} // namespace bdmvauthor
