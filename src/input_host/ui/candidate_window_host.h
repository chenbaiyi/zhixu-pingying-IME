#pragma once

#include "input_host/ui/candidate_presentation.h"

#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

namespace zhixu::input_host::ui {

class CandidateWindowHost final : public CandidatePresenter {
 public:
  CandidateWindowHost() = default;
  ~CandidateWindowHost();
  CandidateWindowHost(const CandidateWindowHost&) = delete;
  CandidateWindowHost& operator=(const CandidateWindowHost&) = delete;

  void SetTheme(int theme) noexcept;
  void Start();
  void Stop() noexcept;
  void Present(CandidatePresentation presentation) override;
  void Hide() noexcept override;

 private:
  void Run(std::stop_token stop_token);
  void Wake() noexcept;

  std::mutex mutex_;
  std::condition_variable ready_condition_;
  std::optional<CandidatePresentation> pending_;
  std::jthread worker_;
  DWORD thread_id_ = 0;
  bool ready_ = false;
  int theme_ = 0;
};

}  // namespace zhixu::input_host::ui
