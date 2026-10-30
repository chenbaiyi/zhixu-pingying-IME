#pragma once

#include "input_host/session/session_controller.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <deque>

namespace zhixu::input_host {

class PredictionDispatcher final {
 public:
  using Dispatch = std::function<void(ConfirmedCommit)>;

  explicit PredictionDispatcher(Dispatch dispatch,
                                std::chrono::milliseconds debounce = std::chrono::milliseconds(100));
  ~PredictionDispatcher();
  PredictionDispatcher(const PredictionDispatcher&) = delete;
  PredictionDispatcher& operator=(const PredictionDispatcher&) = delete;

  void Submit(ConfirmedCommit commit);
  void Invalidate() noexcept;

 private:
  void Run(std::stop_token stop_token);

  Dispatch dispatch_;
  std::chrono::milliseconds debounce_;
  std::mutex mutex_;
  std::condition_variable condition_;
  std::optional<ConfirmedCommit> pending_;
  std::uint64_t generation_ = 0;
  std::jthread worker_;
};

class LearningDispatcher final {
 public:
  using Observer = std::function<bool(const ConfirmedCommit&)>;
  explicit LearningDispatcher(Observer observer);
  ~LearningDispatcher();
  void Submit(ConfirmedCommit commit);
 private:
  Observer observer_;
  std::mutex mutex_;
  std::condition_variable condition_;
  std::deque<ConfirmedCommit> pending_;
  std::jthread worker_;
};

}  // namespace zhixu::input_host
