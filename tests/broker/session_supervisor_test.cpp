#include "broker/session_supervisor.h"

#include <array>
#include <deque>
#include <map>
#include <memory>
#include <stop_token>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

namespace zhixu::broker {
namespace {

class FakeProcess final : public ChildProcess {
 public:
  explicit FakeProcess(std::deque<ChildExit> exits, std::shared_ptr<bool> terminated,
                       std::stop_source* stop_source = nullptr)
      : exits_(std::move(exits)), terminated_(std::move(terminated)), stop_source_(stop_source) {}

  std::optional<ChildExit> TryGetExit() override {
    if (stop_source_ != nullptr) {
      stop_source_->request_stop();
      stop_source_ = nullptr;
      return std::nullopt;
    }
    if (exits_.empty()) {
      return std::nullopt;
    }
    const auto exit = exits_.front();
    exits_.pop_front();
    return exit;
  }

  void Terminate() noexcept override { *terminated_ = true; }

 private:
  std::deque<ChildExit> exits_;
  std::shared_ptr<bool> terminated_;
  std::stop_source* stop_source_;
};

class FakeLauncher final : public ChildLauncher {
 public:
  std::unique_ptr<ChildProcess> Start(const ChildSpec& spec) override {
    ++start_count[spec.role];
    auto& scripts = scripts_by_role[spec.role];
    if (scripts.empty()) {
      return nullptr;
    }
    auto terminated = std::make_shared<bool>(false);
    auto process =
        std::make_unique<FakeProcess>(std::move(scripts.front()), terminated, stop_source);
    scripts.pop_front();
    terminated_by_role[spec.role].push_back(std::move(terminated));
    return process;
  }

  std::map<ChildRole, std::deque<std::deque<ChildExit>>> scripts_by_role;
  std::map<ChildRole, std::size_t> start_count;
  std::map<ChildRole, std::vector<std::shared_ptr<bool>>> terminated_by_role;
  std::stop_source* stop_source = nullptr;
};

ChildSpec Spec(ChildRole role) {
  return {
      .role = role,
      .executable = std::filesystem::path(L"C:\\Program Files\\ZhixuIME\\child.exe"),
  };
}

const ChildRuntimeStatus& StatusFor(const SupervisorResult& result, ChildRole role) {
  for (const auto& status : result.children) {
    if (status.role == role) {
      return status;
    }
  }
  throw std::logic_error("missing child status");
}

TEST(SessionSupervisor, DegradesAfterFiveAbnormalRestartsInWindow) {
  FakeLauncher launcher;
  for (int index = 0; index < 6; ++index) {
    launcher.scripts_by_role[ChildRole::kInputHost].push_back(
        {{.kind = ChildExitKind::kAbnormal}});
  }

  SessionSupervisor supervisor(launcher);
  const auto spec = Spec(ChildRole::kInputHost);
  const auto result = supervisor.Run({&spec, 1}, {});

  const auto& status = StatusFor(result, ChildRole::kInputHost);
  EXPECT_EQ(status.state, ChildRuntimeState::kDegraded);
  EXPECT_EQ(status.abnormal_restarts, 5U);
  EXPECT_EQ(launcher.start_count[ChildRole::kInputHost], 6U);
}

TEST(SessionSupervisor, KeepsInputHostIsolatedFromPredictServiceFailures) {
  FakeLauncher launcher;
  launcher.scripts_by_role[ChildRole::kInputHost].push_back(
      {{.kind = ChildExitKind::kNormal}});
  for (int index = 0; index < 6; ++index) {
    launcher.scripts_by_role[ChildRole::kPredictService].push_back(
        {{.kind = ChildExitKind::kAbnormal}});
  }
  const std::array children = {Spec(ChildRole::kInputHost), Spec(ChildRole::kPredictService)};

  SessionSupervisor supervisor(launcher);
  const auto result = supervisor.Run(children, {});

  EXPECT_EQ(StatusFor(result, ChildRole::kInputHost).state, ChildRuntimeState::kStopped);
  EXPECT_EQ(StatusFor(result, ChildRole::kPredictService).state,
            ChildRuntimeState::kDegraded);
  EXPECT_EQ(launcher.start_count[ChildRole::kInputHost], 1U);
}

TEST(SessionSupervisor, DoesNotRestartDuringRequestedShutdown) {
  FakeLauncher launcher;
  launcher.scripts_by_role[ChildRole::kInputHost].push_back({{}});
  std::stop_source stop_source;
  launcher.stop_source = &stop_source;

  SessionSupervisor supervisor(launcher);
  const auto spec = Spec(ChildRole::kInputHost);
  const auto result = supervisor.Run({&spec, 1}, stop_source.get_token());

  EXPECT_EQ(StatusFor(result, ChildRole::kInputHost).state, ChildRuntimeState::kStopped);
  EXPECT_EQ(launcher.start_count[ChildRole::kInputHost], 1U);
  ASSERT_FALSE(launcher.terminated_by_role[ChildRole::kInputHost].empty());
  EXPECT_TRUE(*launcher.terminated_by_role[ChildRole::kInputHost].front());
}

TEST(SessionSupervisor, RejectsRelativeChildPathWithoutLaunching) {
  FakeLauncher launcher;
  auto spec = Spec(ChildRole::kInputHost);
  spec.executable = L"InputHost.exe";

  SessionSupervisor supervisor(launcher);
  const auto result = supervisor.Run({&spec, 1}, {});

  EXPECT_EQ(StatusFor(result, ChildRole::kInputHost).state, ChildRuntimeState::kDegraded);
  EXPECT_TRUE(launcher.start_count.empty());
}

}  // namespace
}  // namespace zhixu::broker
