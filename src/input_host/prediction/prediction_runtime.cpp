#include "input_host/prediction/prediction_runtime.h"

namespace zhixu::input_host {

PredictionRuntime::PredictionRuntime(Predictor predictor, ResultHandler result_handler,
                                     const std::chrono::milliseconds debounce)
    : predictor_(std::move(predictor)), result_handler_(std::move(result_handler)),
      dispatcher_([this](ConfirmedCommit commit) { Dispatch(std::move(commit)); }, debounce) {}

PredictionRuntime::~PredictionRuntime() = default;

void PredictionRuntime::Submit(ConfirmedCommit commit) {
  if (!commit.allow_prediction || commit.text.empty()) return;
  {
    std::lock_guard lock(mutex_);
    active_ = {.token = next_token_++, .text = commit.text, .context_version = commit.context_version};
  }
  dispatcher_.Submit(std::move(commit));
}

void PredictionRuntime::Invalidate() noexcept {
  {
    std::lock_guard lock(mutex_);
    active_.reset();
  }
  dispatcher_.Invalidate();
}

void PredictionRuntime::Dispatch(ConfirmedCommit commit) {
  ActivePrediction active;
  {
    std::lock_guard lock(mutex_);
    if (!active_.has_value() || active_->text != commit.text ||
        active_->context_version != commit.context_version) {
      return;
    }
    active = *active_;
  }

  auto result = predictor_(commit);
  // A first read can exhaust the disk/IPC budget while warming the local indexes.
  // Retry once on the background worker, only while this exact commit is current.
  if (!result || (result->final_result() && result->candidates_size() == 0)) {
    {
      std::lock_guard lock(mutex_);
      if (!active_ || active_->token != active.token) return;
    }
    result = predictor_(commit);
  }
  if (!result.has_value()) return;

  // Keep the validity check and publication in one critical section.  That
  // gives Invalidate a clear linearization point: once it returns, no older
  // prediction result can begin publication.
  std::lock_guard lock(mutex_);
  if (!active_.has_value() || active_->token != active.token) return;
  result_handler_(std::move(*result), active.context_version);
}

}  // namespace zhixu::input_host
