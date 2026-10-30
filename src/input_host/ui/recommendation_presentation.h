#pragma once

#include "common/protocol/envelope_codec.h"
#include "input_host/ui/window_placement.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace zhixu::input_host::ui {

struct RecommendationPresentation {
  Rect caret;
  Rect work_area;
  std::uint32_t dpi = 96;
  std::vector<std::wstring> recommendations;
  std::vector<std::wstring> provenance;
  std::function<void(unsigned int)> on_accept;
  std::uint32_t page = 0, page_count = 1;
  std::function<void(int)> on_page;
};

class RecommendationPresenter {
 public:
  virtual ~RecommendationPresenter() = default;
  virtual void Present(RecommendationPresentation presentation) = 0;
  virtual void Hide() noexcept = 0;
};

class RecommendationPresentationController {
 public:
  explicit RecommendationPresentationController(RecommendationPresenter* presenter) noexcept;
  void OnCaretUpdate(const protocol::v1::CaretUpdate& caret, std::uint64_t context_version);
  [[nodiscard]] bool OnPredictionResult(const protocol::v1::PredictionResult& result,
                                        std::uint64_t context_version,
                                        std::function<void(unsigned int)> on_accept = {}, std::uint32_t page = 0, std::function<void(int)> on_page = {});
  void Invalidate() noexcept;

 private:
  struct CaretState {
    protocol::v1::CaretUpdate update;
    std::uint64_t context_version = 0;
  };

  RecommendationPresenter* presenter_;
  std::optional<CaretState> caret_;
  bool visible_ = false;
};

}  // namespace zhixu::input_host::ui
