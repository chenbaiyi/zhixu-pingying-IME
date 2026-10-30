#pragma once

#include "input_host/ui/window_base.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace zhixu::input_host::ui {

struct CandidateLine {
  std::wstring text;
  std::wstring comment;
  unsigned int index = 0;
  bool selected = false;
  std::wstring source;
  std::wstring author;
};

class CandidateWindow final : public WindowBase {
 public:
  ~CandidateWindow() override;
  [[nodiscard]] Size Measure(unsigned int dpi, int maximum_width, int maximum_height = 0);
  [[nodiscard]] const std::vector<Rect>& CandidateBounds() const noexcept { return pixel_bounds_; }
  [[nodiscard]] std::optional<unsigned int> HitTest(int x, int y) const noexcept;
  void SetPageState(unsigned int page, bool has_next, std::function<void(bool)> on_page);
  void SetCandidates(std::vector<CandidateLine> candidates,
                     std::function<void(unsigned int)> on_accept,
                     std::wstring page_hint = {});
  [[nodiscard]] bool AcceptShortcut(unsigned int number, bool alt_pressed) const;
  void Dismiss() noexcept;

 protected:
  void Render(ID2D1RenderTarget* target, IDWriteFactory* text_factory) override;
  LRESULT HandleWindowMessage(UINT message, WPARAM w_param, LPARAM l_param) noexcept override;

 private:
  std::vector<CandidateLine> candidates_;
  std::function<void(unsigned int)> on_accept_;
  std::wstring page_hint_;
  unsigned int dpi_ = 96, page_ = 0;
  bool has_next_ = false;
  std::function<void(bool)> on_page_;
  std::vector<D2D1_RECT_F> bounds_;
  std::vector<Rect> pixel_bounds_;
  std::vector<float> number_widths_;
  D2D1_RECT_F previous_{}, next_{};
  HWND tooltip_ = nullptr;
  void ClearTooltips() noexcept;

};

}  // namespace zhixu::input_host::ui
