#include "../third_party/tsmuxer/tsMuxer/preloadedIgsWindow.h"
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <iostream>
int main() {
    // Reproduction from 00001.clpi: singleton IGS PTS 601 s (90 kHz clock).
    constexpr std::int64_t first = 601 * 90000LL;
    if(preloadedIGEnd45k(first,first) != 602 * 45000U) return 1;
    if(preloadedIGEnd45k(first,first+2*90000LL) != 603 * 45000U) return 2;
    if(preloadedIGEnd45k(first,first+45000) != 602 * 45000U) return 3;
    try { (void)preloadedIGEnd45k(first,first-1); return 4; }
    catch(const std::runtime_error&) {}
    try { (void)preloadedIGEnd45k((static_cast<std::int64_t>(UINT32_MAX)-44000)*2, (static_cast<std::int64_t>(UINT32_MAX)-44000)*2); return 5; }
    catch(const std::runtime_error&) {}
    std::cout << "preloaded singleton IGS interval is nonzero and overflow-checked\n";
    return 0;
}
