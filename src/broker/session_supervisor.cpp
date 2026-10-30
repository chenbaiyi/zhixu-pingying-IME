#include "broker/session_supervisor.h"

#include <algorithm>
#include <deque>
#include <thread>
#include <utility>

namespace zhixu::broker {

struct SessionSupervisor::ManagedChild {
  const ChildSpec* spec = nullptr;
  std::unique_ptr<ChildProcess> process;
  ChildRuntimeState state = ChildRuntimeState::kStopped;
  std::deque<std::chrono::steady_clock::time_point> restart_times;
  std::chrono::steady_clock::time_point started_at{};
};

SessionSupervisor::SessionSupervisor(ChildLauncher& launcher, Clock clock)
    : launcher_(launcher),
      clock_(clock ? std::move(clock) : [] { return std::chrono::steady_clock::now(); }) {}

SupervisorResult SessionSupervisor::Run(std::span<const ChildSpec> children,
                                        std::stop_token stop_token) {
  std::vector<ManagedChild> managed;
  managed.reserve(children.size());
  for (const auto& spec : children) {
    managed.push_back({.spec = &spec});
    Start(managed.back());
  }

  while (!stop_token.stop_requested()) {
    bool has_running_child = false;
    for (auto& child : managed) {
      if (child.state != ChildRuntimeState::kRunning) {
        continue;
      }

      has_running_child = true;
      const auto exit = child.process->TryGetExit();
      if (!exit.has_value()) {
        continue;
      }

      child.process.reset();
      if (exit->kind == ChildExitKind::kNormal) {
        child.state = ChildRuntimeState::kStopped;
      } else {
        RecordAbnormalExit(child);
      }
    }

    if (stop_token.stop_requested() || !has_running_child) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  if (stop_token.stop_requested()) {
    for (auto& child : managed) {
      if (child.state == ChildRuntimeState::kRunning) {
        child.process->Terminate();
        child.process.reset();
        child.state = ChildRuntimeState::kStopped;
      }
    }
  }

  SupervisorResult result;
  result.children.reserve(managed.size());
  for (const auto& child : managed) {
    result.children.push_back({
        .role = child.spec->role,
        .state = child.state,
        .abnormal_restarts = child.restart_times.size(),
    });
  }
  return result;
}

bool SessionSupervisor::Start(ManagedChild& child) {
  if (!child.spec->executable.is_absolute()) {
    child.state = ChildRuntimeState::kDegraded;
    return false;
  }

  child.process = launcher_.Start(*child.spec);
  if (!child.process) {
    RecordAbnormalExit(child);
    return false;
  }

  child.started_at = clock_();
  child.state = ChildRuntimeState::kRunning;
  return true;
}

void SessionSupervisor::RecordAbnormalExit(ManagedChild& child) {
  const auto now = clock_();
  const auto earliest = now - child.spec->restart.restart_window;
  while (!child.restart_times.empty() && child.restart_times.front() < earliest) {
    child.restart_times.pop_front();
  }

  if (child.restart_times.size() >= child.spec->restart.max_abnormal_restarts) {
    child.state = ChildRuntimeState::kDegraded;
    return;
  }

  child.restart_times.push_back(now);
  Start(child);
}

}  // namespace zhixu::broker
