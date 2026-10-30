#include "input_host/ui/recommendation_window_host.h"

#include "input_host/ui/recommendation_window.h"
#include "input_host/ui/window_placement.h"

#include <algorithm>

namespace zhixu::input_host::ui {
namespace { constexpr UINT kApplyPresentation = WM_APP + 18; }

void RecommendationWindowHost::SetTheme(int theme) noexcept {
  { std::lock_guard lock(mutex_); theme_ = theme; }
  Wake();
}

RecommendationWindowHost::~RecommendationWindowHost() {
  Stop();
}

void RecommendationWindowHost::Stop() noexcept {
  if (!worker_.joinable()) return;
  worker_.request_stop();
  DWORD thread = 0;
  { std::lock_guard lock(mutex_); thread = thread_id_; }
  if (thread != 0) PostThreadMessageW(thread, WM_QUIT, 0, 0);
  worker_.join();
  { std::lock_guard lock(mutex_); pending_.reset(); thread_id_ = 0; ready_ = false; }
}

void RecommendationWindowHost::Start() {
  std::unique_lock lock(mutex_);
  if (worker_.joinable()) return;
  worker_ = std::jthread([this](const std::stop_token stop_token) { Run(stop_token); });
  ready_condition_.wait(lock, [this] { return ready_; });
}

void RecommendationWindowHost::Present(RecommendationPresentation presentation) {
  { std::lock_guard lock(mutex_); pending_ = std::move(presentation); }
  Wake();
}

void RecommendationWindowHost::Hide() noexcept {
  { std::lock_guard lock(mutex_); pending_.reset(); }
  Wake();
}

void RecommendationWindowHost::Wake() noexcept {
  DWORD thread = 0;
  { std::lock_guard lock(mutex_); thread = thread_id_; }
  if (thread != 0) PostThreadMessageW(thread, kApplyPresentation, 0, 0);
}

void RecommendationWindowHost::Run(const std::stop_token stop_token) {
  MSG message{};
  PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);
  { std::lock_guard lock(mutex_); thread_id_ = GetCurrentThreadId(); ready_ = true; }
  ready_condition_.notify_all();
  RecommendationWindow window;
  const bool created = window.Create(GetModuleHandleW(nullptr), {420, 34});
  while (!stop_token.stop_requested() && GetMessageW(&message, nullptr, 0, 0) > 0) {
    if (message.message == kApplyPresentation) {
      std::optional<RecommendationPresentation> next;
      int theme;
      { std::lock_guard lock(mutex_); next = pending_; theme = theme_; }
      window.ApplyTheme(theme);
      if (!created || !next.has_value()) { window.Dismiss(); continue; }
      window.SetRecommendations(std::move(next->recommendations), std::move(next->on_accept),
                                std::move(next->provenance));
      window.SetPaging(next->page, next->page_count, std::move(next->on_page));
      const Size size = window.Measure(next->dpi, next->work_area.right - next->work_area.left);
      window.ShowAt(PlaceCandidate(next->caret, next->work_area, size, PlacementPreference::Above).bounds);
      continue;
    }
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
}

}  // namespace zhixu::input_host::ui
