#include "input_host/ui/candidate_window_host.h"

#include "input_host/ui/window_placement.h"

#include <algorithm>

namespace zhixu::input_host::ui {
namespace { constexpr UINT kApplyPresentation = WM_APP + 17; }

void CandidateWindowHost::SetTheme(int theme) noexcept {
  { std::lock_guard lock(mutex_); theme_ = theme; }
  Wake();
}

CandidateWindowHost::~CandidateWindowHost() { Stop(); }

void CandidateWindowHost::Stop() noexcept {
  if (!worker_.joinable()) return;
  worker_.request_stop();
  DWORD thread = 0;
  { std::lock_guard lock(mutex_); thread = thread_id_; }
  if (thread != 0) PostThreadMessageW(thread, WM_QUIT, 0, 0);
  worker_.join();
  { std::lock_guard lock(mutex_); pending_.reset(); thread_id_ = 0; ready_ = false; }
}

void CandidateWindowHost::Start() {
  std::unique_lock lock(mutex_);
  if (worker_.joinable()) return;
  worker_ = std::jthread([this](const std::stop_token stop_token) { Run(stop_token); });
  ready_condition_.wait(lock, [this] { return ready_; });
}

void CandidateWindowHost::Present(CandidatePresentation presentation) {
  { std::lock_guard lock(mutex_); pending_ = std::move(presentation); }
  Wake();
}

void CandidateWindowHost::Hide() noexcept {
  { std::lock_guard lock(mutex_); pending_.reset(); }
  Wake();
}

void CandidateWindowHost::Wake() noexcept {
  DWORD thread = 0;
  { std::lock_guard lock(mutex_); thread = thread_id_; }
  if (thread != 0) PostThreadMessageW(thread, kApplyPresentation, 0, 0);
}

void CandidateWindowHost::Run(const std::stop_token stop_token) {
  MSG message{};
  PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);
  { std::lock_guard lock(mutex_); thread_id_ = GetCurrentThreadId(); ready_ = true; }
  ready_condition_.notify_all();
  CandidateWindow window;
  const bool created = window.Create(GetModuleHandleW(nullptr), {420, 34});
  while (!stop_token.stop_requested() && GetMessageW(&message, nullptr, 0, 0) > 0) {
    if (message.message == kApplyPresentation) {
      std::optional<CandidatePresentation> next;
      int theme;
      { std::lock_guard lock(mutex_); next = pending_; theme = theme_; }
      window.ApplyTheme(theme);
      if (!created || !next.has_value()) { window.Dismiss(); continue; }
      window.SetCandidates(std::move(next->candidates), std::move(next->on_accept));
      window.SetPageState(next->page, next->has_next_page, std::move(next->on_page));
      const Size size = window.Measure(next->dpi, next->work_area.right - next->work_area.left,
                                      next->work_area.bottom - next->work_area.top);
      window.ShowAt(PlaceCandidate(next->caret, next->work_area, size, PlacementPreference::Below).bounds);
      continue;
    }
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
}

}  // namespace zhixu::input_host::ui
