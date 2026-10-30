#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <vector>

#include "broker/child_spec.h"

namespace zhixu::broker {

enum class ChildExitKind {
  kNormal,
  kAbnormal,
};

struct ChildExit {
  ChildExitKind kind = ChildExitKind::kAbnormal;
  unsigned long exit_code = 0;
};

class ChildProcess {
 public:
  virtual ~ChildProcess() = default;

  virtual std::optional<ChildExit> TryGetExit() = 0;
  virtual void Terminate() noexcept = 0;
};

class ChildLauncher {
 public:
  virtual ~ChildLauncher() = default;

  virtual std::unique_ptr<ChildProcess> Start(const ChildSpec& spec) = 0;
};

enum class ChildRuntimeState {
  kStopped,
  kRunning,
  kDegraded,
};

struct ChildRuntimeStatus {
  ChildRole role;
  ChildRuntimeState state;
  std::size_t abnormal_restarts = 0;
};

struct SupervisorResult {
  std::vector<ChildRuntimeStatus> children;
};

class SessionSupervisor {
 public:
  using Clock = std::function<std::chrono::steady_clock::time_point()>;

  explicit SessionSupervisor(ChildLauncher& launcher, Clock clock = {});

  [[nodiscard]] SupervisorResult Run(std::span<const ChildSpec> children,
                                     std::stop_token stop_token);

 private:
  struct ManagedChild;

  [[nodiscard]] bool Start(ManagedChild& child);
  void RecordAbnormalExit(ManagedChild& child);

  ChildLauncher& launcher_;
  Clock clock_;
};

}  // namespace zhixu::broker
