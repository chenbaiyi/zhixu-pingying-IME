#pragma once

namespace zhixu::input_host::ui {

struct Rect {
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;
};

struct Size {
  int width = 0;
  int height = 0;
};

enum class PlacementPreference {
  Above,
  Below,
};

struct WindowPlacement {
  Rect bounds;
  bool placed_above = false;
};

[[nodiscard]] WindowPlacement PlaceCandidate(Rect caret, Rect work_area, Size window,
                                               PlacementPreference preference) noexcept;

}  // namespace zhixu::input_host::ui
