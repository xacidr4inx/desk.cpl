#pragma once
#include <windows.h>
#include <vector>
#include <limits>
#include <algorithm>

// Once monitors share a side, snap nearby parallel edges exactly rather than
// preserving tiny desktop-pixel offsets hidden by the preview's scale.
inline RECT SnapAttachedMonitorAlignment(RECT rect, const RECT& neighbor,
    bool sideAttachment, int scale, LONG tolerance = 6)
{
    if (scale <= 0) return rect;
    const LONG first = sideAttachment ? neighbor.top - rect.top : neighbor.left - rect.left;
    const LONG second = sideAttachment ? neighbor.bottom - rect.bottom : neighbor.right - rect.right;
    const auto magnitude = [](LONG value) { return value < 0 ? -static_cast<LONGLONG>(value) : static_cast<LONGLONG>(value); };
    const LONG correction = magnitude(first) <= magnitude(second) ? first : second;
    if (magnitude(::MulDiv(correction, scale, 1000)) <= tolerance)
        ::OffsetRect(&rect, sideAttachment ? 0 : correction, sideAttachment ? correction : 0);
    return rect;
}

// Desktop pixels only. The preview's scale, border and decorative gutter must
// never reach DEVMODE.dmPosition. Preserve touching edges when a mode resizes.
inline std::vector<RECT> ResizeMonitorArrangement(const std::vector<RECT>& before,
    std::vector<RECT> after, size_t primary)
{
    if (before.empty() || before.size() != after.size()) return after;
    if (primary >= before.size()) primary = 0;
    const LONG unset = (std::numeric_limits<LONG>::min)();
    for (int axis = 0; axis < 2; ++axis)
    {
        std::vector<LONG> shift(before.size(), unset), delta(before.size());
        for (size_t i = 0; i < before.size(); ++i)
            delta[i] = axis == 0 ? (after[i].right - after[i].left) - (before[i].right - before[i].left)
                : (after[i].bottom - after[i].top) - (before[i].bottom - before[i].top);
        shift[primary] = 0;
        for (size_t pass = 0; pass < before.size(); ++pass)
        {
            bool changed = false;
            for (size_t i = 0; i < before.size(); ++i)
            {
                if (shift[i] == unset) continue;
                const RECT& a = before[i];
                for (size_t j = 0; j < before.size(); ++j)
                {
                    if (shift[j] != unset) continue;
                    const RECT& b = before[j];
                    const LONG a0 = axis == 0 ? a.left : a.top;
                    const LONG a1 = axis == 0 ? a.right : a.bottom;
                    const LONG b0 = axis == 0 ? b.left : b.top;
                    const LONG b1 = axis == 0 ? b.right : b.bottom;
                    const LONG crossA0 = axis == 0 ? a.top : a.left;
                    const LONG crossA1 = axis == 0 ? a.bottom : a.right;
                    const LONG crossB0 = axis == 0 ? b.top : b.left;
                    const LONG crossB1 = axis == 0 ? b.bottom : b.right;
                    const LONG overlapStart = (std::max)(crossA0, crossB0);
                    const LONG overlapEnd = (std::min)(crossA1, crossB1);
                    if (overlapStart > overlapEnd) continue;
                    LONG candidate = unset;
                    if (overlapStart < overlapEnd && a1 == b0) candidate = shift[i] + delta[i];
                    else if (overlapStart < overlapEnd && a0 == b1) candidate = shift[i] - delta[j];
                    else if (a0 == b0) candidate = shift[i];
                    else if (a1 == b1) candidate = shift[i] + delta[i] - delta[j];
                    if (candidate != unset)
                    {
                        shift[j] = candidate;
                        changed = true;
                    }
                }
            }
            if (!changed) break;
        }
        for (size_t i = 0; i < after.size(); ++i)
            if (shift[i] != unset)
                ::OffsetRect(&after[i], axis == 0 ? shift[i] : 0, axis == 1 ? shift[i] : 0);
    }
    return after;
}

inline std::vector<RECT> NormalizeMonitorArrangement(std::vector<RECT> rects, size_t primary)
{
    if (primary >= rects.size()) return rects;
    const POINT origin = { rects[primary].left, rects[primary].top };
    for (RECT& rect : rects) ::OffsetRect(&rect, -origin.x, -origin.y);
    return rects;
}
