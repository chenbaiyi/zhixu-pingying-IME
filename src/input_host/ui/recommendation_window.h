#pragma once

#include "input_host/ui/window_base.h"

#include <functional>
#include <string>
#include <vector>

namespace zhixu::input_host::ui {

class RecommendationWindow final : public WindowBase {
 public:
  void SetRecommendations(std::vector<std::wstring> recommendations,
                          std::function<void(unsigned int)> on_accept,
                          std::vector<std::wstring> provenance = {});
  void SetPaging(unsigned int page, unsigned int page_count, std::function<void(int)> on_page);
  [[nodiscard]] Size Measure(unsigned int dpi, int maximum_width);
  [[nodiscard]] bool AcceptShortcut(unsigned int number, bool alt_pressed) ;
  void Dismiss() noexcept;

 protected:
  void Render(ID2D1RenderTarget* target, IDWriteFactory* text_factory) override;
  LRESULT HandleWindowMessage(UINT message, WPARAM w_param, LPARAM l_param) noexcept override;

 private:
  void Accept(unsigned int index);
  void Page(int direction);
  [[nodiscard]] D2D1_RECT_F PageBounds(bool forward, float width) const noexcept;
  unsigned int dpi_ = 96;
  int row_height_ = 26;
  int selected_ = 0;
  unsigned int page_ = 0;
  unsigned int page_count_ = 1;
  std::vector<std::wstring> provenance_;
  std::vector<std::wstring> recommendations_;
  std::function<void(unsigned int)> on_accept_;
  std::function<void(int)> on_page_;
};

}  // namespace zhixu::input_host::ui
