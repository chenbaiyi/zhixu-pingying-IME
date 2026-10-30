#include "input_host/ui/candidate_presentation.h"

#include <Windows.h>

namespace zhixu::input_host::ui {
namespace {

Rect ToRect(const protocol::v1::Rect& rect) {
  return {.left = rect.left(), .top = rect.top(), .right = rect.right(), .bottom = rect.bottom()};
}

std::wstring ToWide(const std::string& text) {
  if (text.empty()) return {};
  const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
  if (length <= 0) return {};
  std::wstring value(static_cast<std::size_t>(length), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                          static_cast<int>(text.size()), value.data(), length) != length) return {};
  return value;
}

}  // namespace

CandidatePresentationController::CandidatePresentationController(CandidatePresenter* const presenter) noexcept
    : presenter_(presenter) {}

void CandidatePresentationController::OnCaretUpdate(const protocol::v1::CaretUpdate& caret,
                                                     const std::uint64_t context_version) {
  if (caret_ && presenter_ != nullptr &&
      (caret_->context_version != context_version ||
       caret_->update.SerializeAsString() != caret.SerializeAsString())) presenter_->Hide();
  // A collapsed TSF insertion selection can have zero width but a valid height.
  if (caret.caret().right() < caret.caret().left() || caret.caret().bottom() <= caret.caret().top() ||
      caret.work_area().right() <= caret.work_area().left() ||
      caret.work_area().bottom() <= caret.work_area().top()) {
    Invalidate();
    return;
  }
  caret_ = {.update = caret, .context_version = context_version};
}

void CandidatePresentationController::OnEngineAction(const protocol::v1::EngineAction& action,
                                                      const protocol::v1::PrivacyDecision& privacy,
                                                      const std::uint64_t context_version,
                                                      std::function<void(unsigned int)> on_accept,
                                                      std::function<void(bool)> on_page) {
  if (presenter_ == nullptr) return;
  if ((!privacy.allow_composition() && !privacy.allow_prediction()) || !action.consumed() || action.composition().candidates_size() == 0 ||
      !caret_.has_value() || caret_->context_version != context_version) {
    presenter_->Hide();
    return;
  }
  CandidatePresentation presentation{.caret = ToRect(caret_->update.caret()),
                                      .work_area = ToRect(caret_->update.work_area()),
                                      .dpi = caret_->update.dpi()};
  presentation.on_accept = std::move(on_accept);
  presentation.on_page = std::move(on_page);
  presentation.page = action.composition().page();
  presentation.has_next_page = action.composition().has_next_page();
  const auto highlighted = action.composition().has_highlighted_candidate()
      ? action.composition().highlighted_candidate() : action.composition().candidates(0).index();
  for (const auto& candidate : action.composition().candidates()) {
    auto text = ToWide(candidate.text());
    if (text.empty()) {
      presenter_->Hide();
      return;
    }
    presentation.candidates.push_back({.text = std::move(text),
                                       .comment = ToWide(candidate.comment()),
                                       .index = candidate.index(),
                                       .selected = candidate.index() == highlighted});
  }
  presenter_->Present(std::move(presentation));
}

void CandidatePresentationController::Invalidate() noexcept {
  caret_.reset();
  if (presenter_ != nullptr) presenter_->Hide();
}

}  // namespace zhixu::input_host::ui
