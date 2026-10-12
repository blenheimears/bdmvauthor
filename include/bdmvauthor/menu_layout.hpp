// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "bdmvauthor/model.hpp"
#include <algorithm>
#include <cstdint>
#include <vector>

namespace bdmvauthor {

// Geometry-only layout operations: they never resize buttons/labels or change
// navigation actions, visual styles, or their order in the project model.
enum class MenuAlign { Left, HCenter, Right, Top, VCenter, Bottom };
enum class MenuAxis { Horizontal, Vertical };

inline bool align_menu_rects(const std::vector<Rect*>& rects, MenuAlign alignment,
                             const Rect& aperture) {
    if (rects.size() < 2 || aperture.width <= 0 || aperture.height <= 0) return false;
    if (std::any_of(rects.begin(), rects.end(), [](const Rect* r){ return !r || r->width <= 0 || r->height <= 0; })) return false;
    int left = rects.front()->x, top = rects.front()->y;
    int right = left + rects.front()->width, bottom = top + rects.front()->height;
    int widest = 0, tallest = 0;
    for (const auto* r : rects) {
        left = std::min(left, r->x); top = std::min(top, r->y);
        right = std::max(right, r->x + r->width);
        bottom = std::max(bottom, r->y + r->height);
        widest = std::max(widest, r->width); tallest = std::max(tallest, r->height);
    }
    const bool horizontal = alignment == MenuAlign::Left || alignment == MenuAlign::HCenter || alignment == MenuAlign::Right;
    if ((horizontal ? widest > aperture.width : tallest > aperture.height)) return false;
    const int extent = horizontal ? aperture.width : aperture.height;
    const int start = horizontal ? aperture.x : aperture.y;
    const int largest = horizontal ? widest : tallest;
    const int candidate = alignment == MenuAlign::Left || alignment == MenuAlign::Top ? (horizontal ? left : top)
        : alignment == MenuAlign::Right || alignment == MenuAlign::Bottom ? (horizontal ? right : bottom)
        : ((horizontal ? left + right : top + bottom) / 2);
    const int anchor = alignment == MenuAlign::Left || alignment == MenuAlign::Top
        ? std::clamp(candidate, start, start + extent - largest)
        : alignment == MenuAlign::Right || alignment == MenuAlign::Bottom
            ? std::clamp(candidate, start + largest, start + extent)
            : std::clamp(candidate, start + (largest + 1) / 2, start + extent - largest / 2);
    bool changed = false;
    for (auto* r : rects) {
        const int size = horizontal ? r->width : r->height;
        const int value = alignment == MenuAlign::Left || alignment == MenuAlign::Top ? anchor
            : alignment == MenuAlign::Right || alignment == MenuAlign::Bottom ? anchor - size
            : anchor - size / 2;
        int& coordinate = horizontal ? r->x : r->y;
        const int clamped = std::clamp(value, start, start + extent - size);
        changed |= coordinate != clamped; coordinate = clamped;
    }
    return changed;
}

// An explicit edge-to-edge gap in project design pixels. If the requested
// row/column cannot fit inside the aperture, leave every rectangle unchanged.
inline bool space_menu_rects(const std::vector<Rect*>& rects, MenuAxis axis,
                             int gap, const Rect& aperture) {
    if (rects.size() < 2 || gap < 0 || aperture.width <= 0 || aperture.height <= 0) return false;
    auto sorted = rects;
    if (std::any_of(sorted.begin(), sorted.end(), [](const Rect* r){ return !r || r->width <= 0 || r->height <= 0; })) return false;
    const bool horizontal = axis == MenuAxis::Horizontal;
    std::stable_sort(sorted.begin(), sorted.end(), [horizontal](const Rect* a, const Rect* b){
        return (horizontal ? a->x : a->y) < (horizontal ? b->x : b->y);
    });
    const std::int64_t extent = horizontal ? aperture.width : aperture.height;
    const std::int64_t origin = horizontal ? aperture.x : aperture.y;
    std::int64_t span = static_cast<std::int64_t>(gap) * (sorted.size() - 1);
    for (const auto* r : sorted) span += horizontal ? r->width : r->height;
    if (span > extent) return false;
    std::int64_t pos = std::clamp<std::int64_t>(horizontal ? sorted.front()->x : sorted.front()->y,
                                                 origin, origin + extent - span);
    bool changed = false;
    for (auto* r : sorted) {
        int& coordinate = horizontal ? r->x : r->y;
        changed |= coordinate != pos; coordinate = static_cast<int>(pos);
        pos += (horizontal ? r->width : r->height) + gap;
    }
    return changed;
}
} // namespace bdmvauthor
