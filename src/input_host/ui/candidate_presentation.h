#pragma once

#include "common/protocol/envelope_codec.h"
#include "input_host/ui/candidate_window.h"

#include <cstdint>
#include <optional>

namespace zhixu::input_host::ui {

struct CandidatePresentation {
  Rect caret;
  Rect work_area;
  std::uint32_t dpi = 96;
  std::vector<CandidateLine> candidates;
  std::uint32_t page = 0;
  bool has_next_page = false;
  std::function<void(unsigned int)> on_accept;
  std::function<void(bool)> on_page;
};

class CandidatePresenter {
 public:
  virtual ~CandidatePresenter() = default;
  virtual void Present(CandidatePresentation presentation) = 0;
  virtual void Hide() noexcept = 0;
};

class CandidatePresentationController {
 public:
  explicit CandidatePresentationController(CandidatePresenter* presenter) noexcept;
  void OnCaretUpdate(const protocol::v1::CaretUpdate& caret, std::uint64_t context_version);
  void OnEngineAction(const protocol::v1::EngineAction& action,
                      const protocol::v1::PrivacyDecision& privacy,
                      std::uint64_t context_version,
                      std::function<void(unsigned int)> on_accept = {},
                      std::function<void(bool)> on_page = {});
  void Invalidate() noexcept;

 private:
  struct CaretState {
    protocol::v1::CaretUpdate update;
    std::uint64_t context_version = 0;
  };

  CandidatePresenter* presenter_;
  std::optional<CaretState> caret_;
};

}  // namespace zhixu::input_host::ui
