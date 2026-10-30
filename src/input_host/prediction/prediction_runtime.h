#pragma once

#include "input_host/prediction/prediction_dispatcher.h"
#include "input_host/session/session_controller.h"

#include <chrono>
#include <functional>
#include <mutex>
#include <optional>

namespace zhixu::input_host {

class PredictionRuntime final {
 public:
  using Predictor = std::function<std::optional<protocol::v1::PredictionResult>(const ConfirmedCommit&)>;
  using ResultHandler = std::function<void(protocol::v1::PredictionResult, std::uint64_t)>;

  PredictionRuntime(Predictor predictor, ResultHandler result_handler,
                    std::chrono::milliseconds debounce = std::chrono::milliseconds(100));
  ~PredictionRuntime();
  PredictionRuntime(const PredictionRuntime&) = delete;
  PredictionRuntime& operator=(const PredictionRuntime&) = delete;

  void Submit(ConfirmedCommit commit);
  void Invalidate() noexcept;

 private:
  struct ActivePrediction {
    std::uint64_t token = 0;
    std::string text;
    std::uint64_t context_version = 0;
  };

  void Dispatch(ConfirmedCommit commit);

  Predictor predictor_;
  ResultHandler result_handler_;
  std::mutex mutex_;
  std::optional<ActivePrediction> active_;
  std::uint64_t next_token_ = 1;
  // Declare last so its worker stops before the callback state above is destroyed.
  PredictionDispatcher dispatcher_;
};

}  // namespace zhixu::input_host
