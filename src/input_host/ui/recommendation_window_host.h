#pragma once

#include "input_host/ui/recommendation_presentation.h"

#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

#include <Windows.h>

namespace zhixu::input_host::ui {

class RecommendationWindowHost final : public RecommendationPresenter {
 public:
  RecommendationWindowHost() = default;
  ~RecommendationWindowHost();
  RecommendationWindowHost(const RecommendationWindowHost&) = delete;
  RecommendationWindowHost& operator=(const RecommendationWindowHost&) = delete;

  void SetTheme(int theme) noexcept;
  void Start();
  void Stop() noexcept;
  void Present(RecommendationPresentation presentation) override;
  void Hide() noexcept override;

 private:
  void Run(std::stop_token stop_token);
  void Wake() noexcept;

  std::mutex mutex_;
  std::condition_variable ready_condition_;
  std::optional<RecommendationPresentation> pending_;
  std::jthread worker_;
  DWORD thread_id_ = 0;
  bool ready_ = false;
  int theme_ = 0;
};

}  // namespace zhixu::input_host::ui
