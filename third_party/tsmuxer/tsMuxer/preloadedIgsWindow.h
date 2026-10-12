#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>

// Preloaded (application_type=5) IGS may have a single display set with the
// same PTS on all segments. The elementary data is persistent; its metadata
// must nevertheless describe a positive interval or mastering systems reject
// the Clip/SubPlayItem. All times in this interface are in 90 kHz ticks.
// Other clip application types retain their unmodified timing.
inline std::uint32_t preloadedIGEnd45k(std::int64_t firstPTS90k, std::int64_t lastPTS90k)
{
    constexpr std::int64_t oneSecond90k = 90000;
    constexpr std::int64_t max45k = std::numeric_limits<std::uint32_t>::max();
    if (firstPTS90k < 0 || lastPTS90k < firstPTS90k ||
        firstPTS90k > (max45k - 45000) * 2 || lastPTS90k / 2 > max45k)
        throw std::runtime_error("invalid preloaded IGS presentation timestamps");
    return static_cast<std::uint32_t>(std::max(lastPTS90k, firstPTS90k + oneSecond90k) / 2);
}
