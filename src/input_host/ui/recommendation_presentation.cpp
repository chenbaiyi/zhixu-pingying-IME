#include "input_host/ui/recommendation_presentation.h"

#include <Windows.h>
#include <algorithm>

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
  return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                             value.data(), length) == length ? value : std::wstring{};
}

}  // namespace

RecommendationPresentationController::RecommendationPresentationController(
    RecommendationPresenter* const presenter) noexcept : presenter_(presenter) {}

void RecommendationPresentationController::OnCaretUpdate(const protocol::v1::CaretUpdate& caret,
                                                          const std::uint64_t context_version) {
  // Commit acknowledgment and OnEndEdit may report the same insertion caret.
  // A duplicate notification must not hide or invalidate the visible batch.
  if (caret_ && caret_->context_version == context_version &&
      caret_->update.SerializeAsString() == caret.SerializeAsString()) return;
  if (visible_ && presenter_ != nullptr) presenter_->Hide();
  visible_ = false;
  if (caret.caret().right() < caret.caret().left() || caret.caret().bottom() <= caret.caret().top() ||
      caret.work_area().right() <= caret.work_area().left() ||
      caret.work_area().bottom() <= caret.work_area().top()) {
    caret_.reset();
    return;
  }
  caret_ = {.update = caret, .context_version = context_version};
}

bool RecommendationPresentationController::OnPredictionResult(const protocol::v1::PredictionResult& result,
                                                               const std::uint64_t context_version,
                                                               std::function<void(unsigned int)> on_accept, std::uint32_t page, std::function<void(int)> on_page) {
  if (presenter_ == nullptr) return false;
  if (!result.final_result() || !caret_.has_value() || caret_->context_version != context_version ||
      result.candidates_size() == 0) {
    if (visible_) presenter_->Hide();
    visible_ = false;
    return false;
  }
  const auto count = std::min(30, result.candidates_size());
  const auto pages = static_cast<std::uint32_t>((count + 5) / 6);
  if (page >= pages) { Invalidate(); return false; }
  RecommendationPresentation presentation{.caret = ToRect(caret_->update.caret()),
                                            .work_area = ToRect(caret_->update.work_area()),
                                            .dpi = caret_->update.dpi()};
  presentation.page = page; presentation.page_count = pages; presentation.on_page = std::move(on_page);
  for (int index = static_cast<int>(page * 6); index < std::min(count, static_cast<int>(page * 6 + 6)); ++index) {
    const auto& candidate = result.candidates(index);
    if (presentation.recommendations.size() == 6U) break;
    auto text = ToWide(candidate.text());
    if (text.empty()) {
      if (visible_) presenter_->Hide();
      visible_ = false;
      return false;
    }
    std::wstring citation;
    if (candidate.kind() == protocol::v1::QUOTATION ||
        candidate.kind() == protocol::v1::EXACT_CONTINUATION) {
      for (const auto* field : {&candidate.source(), &candidate.author(), &candidate.chapter()}) {
        if (field->empty()) continue;
        auto part = ToWide(*field);
        if (part.empty()) { Invalidate(); return false; }
        if (!citation.empty()) citation += L" · ";
        citation += part;
      }
    }
    presentation.recommendations.push_back(std::move(text));
    presentation.provenance.push_back(std::move(citation));
  }
  if (presentation.recommendations.empty()) {
    if (visible_) presenter_->Hide();
    visible_ = false;
    return false;
  }
  presentation.on_accept = std::move(on_accept);
  presenter_->Present(std::move(presentation));
  visible_ = true;
  return true;
}

void RecommendationPresentationController::Invalidate() noexcept {
  caret_.reset();
  if (visible_ && presenter_ != nullptr) presenter_->Hide();
  visible_ = false;
}

}  // namespace zhixu::input_host::ui
