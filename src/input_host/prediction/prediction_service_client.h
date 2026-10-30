#pragma once

#include "common/ipc/named_pipe.h"
#include "input_host/session/session_controller.h"

#include <atomic>
#include <optional>

namespace zhixu::input_host {

class PredictionServiceClient final {
 public:
  explicit PredictionServiceClient(ipc::PipeEndpoint endpoint);

  [[nodiscard]] bool Observe(const ConfirmedCommit& commit);
  [[nodiscard]] std::optional<protocol::v1::PredictionResult> Predict(
      const ConfirmedCommit& commit);

 private:
  ipc::PipeEndpoint endpoint_;
  std::atomic_uint64_t next_request_id_{1};
};

}  // namespace zhixu::input_host
