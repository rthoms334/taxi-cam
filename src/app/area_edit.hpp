#pragma once
// A display area (the rectangle of a display texture a side's cameras are
// drawn in) moved and resized over a picture of the texture: the camera
// adjustment window's display area mode and the Displays step's area picker.
// Handles 0-3 are the corners (top-left, top-right, bottom-right,
// bottom-left), 4-7 the edge midpoints (top, right, bottom, left), 8 the
// whole frame.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <algorithm>
#include <array>
#include "../profiles/catalog.hpp"

namespace taxi_camera::standalone::area_edit {
struct Limits {
  int min_width = 16, min_height = 16;
};
// Large enough to keep a camera inside the profile's padding, and a catalog
// profile's whole-display minimum (valid_override).
inline Limits limits(const profiles::AircraftProfile& p) noexcept {
  const auto& pad = p.camera_padding;
  const int least = p.fit_texture ? 16 : 64;
  return {std::max(least, static_cast<int>(pad.left + pad.right) + 16), std::max(least, static_cast<int>(pad.top + pad.bottom) + 16)};
}
// Texture pixels on a picture of the whole texture.
inline POINT to_screen(const RECT& picture, unsigned texture_width, unsigned texture_height, unsigned x, unsigned y) noexcept {
  const int tw = static_cast<int>(std::max(texture_width, 1u)), th = static_cast<int>(std::max(texture_height, 1u));
  return {picture.left + MulDiv(static_cast<int>(x), picture.right - picture.left, tw),
          picture.top + MulDiv(static_cast<int>(y), picture.bottom - picture.top, th)};
}
inline RECT to_screen(const RECT& picture, unsigned texture_width, unsigned texture_height, const profiles::DisplayRect& a) noexcept {
  const auto from = to_screen(picture, texture_width, texture_height, a.left, a.top);
  const auto to = to_screen(picture, texture_width, texture_height, a.right, a.bottom);
  return {from.x, from.y, to.x, to.y};
}
inline std::array<POINT, 8> handles(const RECT& picture, unsigned texture_width, unsigned texture_height, const profiles::DisplayRect& a) {
  const unsigned cx = (a.left + a.right) / 2, cy = (a.top + a.bottom) / 2;
  const auto at = [&](unsigned x, unsigned y) { return to_screen(picture, texture_width, texture_height, x, y); };
  return {at(a.left, a.top), at(a.right, a.top), at(a.right, a.bottom), at(a.left, a.bottom),
          at(cx, a.top),     at(a.right, cy),    at(cx, a.bottom),      at(a.left, cy)};
}
// The handle under a point (within grab pixels), else 8 inside the frame
// (within reach pixels of it), else -1.
inline int
hit(const RECT& picture, unsigned texture_width, unsigned texture_height, const profiles::DisplayRect& a, POINT p, int grab, int reach) {
  const auto points = handles(picture, texture_width, texture_height, a);
  for (int i = 0; i < 8; ++i) {
    const auto& q = points[static_cast<std::size_t>(i)];
    if (std::abs(q.x - p.x) <= grab && std::abs(q.y - p.y) <= grab)
      return i;
  }
  const auto r = to_screen(picture, texture_width, texture_height, a);
  return p.x >= r.left - reach && p.x <= r.right + reach && p.y >= r.top - reach && p.y <= r.bottom + reach ? 8 : -1;
}
// The area after dragging a handle by dx, dy texture pixels from start:
// kept on the texture and no smaller than the limits.
inline profiles::DisplayRect drag(const profiles::DisplayRect& start, int handle, int dx, int dy, int tw, int th, Limits least) {
  int left = static_cast<int>(start.left), top = static_cast<int>(start.top), right = static_cast<int>(start.right),
      bottom = static_cast<int>(start.bottom);
  if (handle == 8) {
    const int w = right - left, h = bottom - top;
    left = std::clamp(left + dx, 0, std::max(tw - w, 0));
    top = std::clamp(top + dy, 0, std::max(th - h, 0));
    right = left + w;
    bottom = top + h;
  } else {
    if (handle == 0 || handle == 3 || handle == 7)
      left = std::clamp(left + dx, 0, std::max(right - least.min_width, 0));
    if (handle == 1 || handle == 2 || handle == 5)
      right = std::clamp(right + dx, std::min(left + least.min_width, tw), tw);
    if (handle == 0 || handle == 1 || handle == 4)
      top = std::clamp(top + dy, 0, std::max(bottom - least.min_height, 0));
    if (handle == 2 || handle == 3 || handle == 6)
      bottom = std::clamp(bottom + dy, std::min(top + least.min_height, th), th);
  }
  return {static_cast<unsigned>(std::max(left, 0)), static_cast<unsigned>(std::max(top, 0)), static_cast<unsigned>(std::max(right, 0)),
          static_cast<unsigned>(std::max(bottom, 0))};
}
// A rectangle (a gauge's) made a usable area: grown about its centre to the
// limits where it is smaller, then kept on the texture.
inline profiles::DisplayRect fit(const profiles::DisplayRect& r, int tw, int th, Limits least) {
  const int w = std::min(std::max(static_cast<int>(r.right) - static_cast<int>(r.left), least.min_width), tw);
  const int h = std::min(std::max(static_cast<int>(r.bottom) - static_cast<int>(r.top), least.min_height), th);
  const int cx = (static_cast<int>(r.left) + static_cast<int>(r.right)) / 2,
            cy = (static_cast<int>(r.top) + static_cast<int>(r.bottom)) / 2;
  const int left = std::clamp(cx - w / 2, 0, std::max(tw - w, 0)), top = std::clamp(cy - h / 2, 0, std::max(th - h, 0));
  return {static_cast<unsigned>(left), static_cast<unsigned>(top), static_cast<unsigned>(left + w), static_cast<unsigned>(top + h)};
}
// The pointer shape over a handle (0-8): the IDC_SIZE* system cursors,
// named by number so the header does not depend on UNICODE.
inline LPCWSTR cursor(int handle) noexcept {
  static const WORD shapes[]{32642, 32643, 32642, 32643, 32645, 32644, 32645, 32644, 32646};
  return MAKEINTRESOURCEW(handle >= 0 && handle < 9 ? shapes[handle] : 32512);
}
inline bool same(const profiles::DisplayRect& a, const profiles::DisplayRect& b) noexcept {
  return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}
}  // namespace taxi_camera::standalone::area_edit
