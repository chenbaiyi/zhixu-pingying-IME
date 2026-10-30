#include "input_host/prediction/prediction_dispatcher.h"
#include <windows.h>

namespace zhixu::input_host {

PredictionDispatcher::PredictionDispatcher(Dispatch dispatch, const std::chrono::milliseconds debounce)
    : dispatch_(std::move(dispatch)), debounce_(debounce),
      worker_([this](const std::stop_token stop_token) { Run(stop_token); }) {}

PredictionDispatcher::~PredictionDispatcher() {
  worker_.request_stop();
  condition_.notify_all();
}

void PredictionDispatcher::Submit(ConfirmedCommit commit) {
  if (!commit.allow_prediction) return;
  {
    std::lock_guard lock(mutex_);
    pending_ = std::move(commit);
    ++generation_;
  }
  condition_.notify_all();
}

void PredictionDispatcher::Invalidate() noexcept {
  {
    std::lock_guard lock(mutex_);
    pending_.reset();
    ++generation_;
  }
  condition_.notify_all();
}

void PredictionDispatcher::Run(const std::stop_token stop_token) {
  std::unique_lock lock(mutex_);
  while (!stop_token.stop_requested()) {
    condition_.wait(lock, [&] { return stop_token.stop_requested() || pending_.has_value(); });
    if (stop_token.stop_requested()) return;

    const auto generation = generation_;
    const auto deadline = std::chrono::steady_clock::now() + debounce_;
    if (condition_.wait_until(lock, deadline,
                              [&] { return stop_token.stop_requested() || generation_ != generation; })) {
      continue;
    }
    if (!pending_.has_value() || generation_ != generation) continue;

    auto commit = std::move(*pending_);
    pending_.reset();
    lock.unlock();
    dispatch_(std::move(commit));
    lock.lock();
  }
}

LearningDispatcher::LearningDispatcher(Observer observer) : observer_(std::move(observer)),
 worker_([this](std::stop_token stop) {
  std::unique_lock lock(mutex_);
  while(!stop.stop_requested()) {
   condition_.wait(lock,[&]{return stop.stop_requested() || !pending_.empty();});
   if(stop.stop_requested())return;
   const auto commit=pending_.front();
   lock.unlock();const bool delivered=observer_(commit);lock.lock();
   if(delivered)pending_.pop_front();
   else condition_.wait_for(lock,std::chrono::milliseconds(100),[&]{return stop.stop_requested();});
  }
 }) {}
LearningDispatcher::~LearningDispatcher(){worker_.request_stop();condition_.notify_all();}
void LearningDispatcher::Submit(ConfirmedCommit commit) {
 if(!commit.allow_learning || commit.text.empty())return;
 {std::lock_guard lock(mutex_);if(pending_.size()>=1024){OutputDebugStringW(L"ZhixuIME: learning queue capacity reached; newest event dropped.\n");return;}pending_.push_back(std::move(commit));}
 condition_.notify_all();
}

}  // namespace zhixu::input_host
