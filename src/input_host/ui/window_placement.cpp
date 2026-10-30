#include "input_host/ui/window_placement.h"

#include <algorithm>

namespace zhixu::input_host::ui {
namespace {

[[nodiscard]] int Clamp(const int value, const int minimum, const int maximum) noexcept {
  return std::clamp(value, minimum, std::max(minimum, maximum));
}

[[nodiscard]] bool FitsAbove(const Rect caret, const Rect work_area, const Size window) noexcept {
  return caret.top - window.height >= work_area.top;
}

[[nodiscard]] bool FitsBelow(const Rect caret, const Rect work_area, const Size window) noexcept {
  return caret.bottom + window.height <= work_area.bottom;
}

}  // namespace

WindowPlacement PlaceCandidate(const Rect caret, const Rect work_area, const Size window,
                               const PlacementPreference preference) noexcept {
  const int width = std::clamp(window.width, 0, std::max(0, work_area.right - work_area.left));
  const int height = std::clamp(window.height, 0, std::max(0, work_area.bottom - work_area.top));
  const bool prefer_above = preference == PlacementPreference::Above;
  const bool place_above = prefer_above ? FitsAbove(caret, work_area, {width, height})
                                        : !FitsBelow(caret, work_area, {width, height});
  const int top = place_above ? caret.top - height : caret.bottom;
  const int left = Clamp(caret.left, work_area.left, work_area.right - width);
  const int clamped_top = Clamp(top, work_area.top, work_area.bottom - height);
  return {.bounds = {.left = left,
                     .top = clamped_top,
                     .right = left + width,
                     .bottom = clamped_top + height},
          .placed_above = place_above};
}

}  // namespace zhixu::input_host::ui
