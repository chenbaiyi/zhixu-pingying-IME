#pragma once

#include <chrono>
#include <filesystem>

namespace zhixu::broker {

enum class ChildRole {
  kInputHost,
  kPredictService,
};

struct RestartPolicy {
  std::size_t max_abnormal_restarts = 5;
  std::chrono::minutes restart_window = std::chrono::minutes(5);
};

struct ChildSpec {
  ChildRole role;
  std::filesystem::path executable;
  std::chrono::milliseconds healthy_after = std::chrono::seconds(30);
  RestartPolicy restart;
};

}  // namespace zhixu::broker
