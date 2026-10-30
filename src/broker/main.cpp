#include "broker/session_supervisor.h"
#include "broker/settings_control.h"
#include "broker/win32_child_launcher.h"
#include "common/ipc/pipe_security.h"
#include "common/paths/runtime_paths.h"
#include "common/settings/settings_store.h"

#include <Windows.h>

#include <array>
#include <filesystem>
#include <stop_token>
#include <string>
#include <thread>

namespace {

std::filesystem::path ExecutableDirectory() {
  std::array<wchar_t, 32768> path{};
  const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  if (length == 0 || length >= path.size()) {
    return {};
  }
  return std::filesystem::path(std::wstring(path.data(), length)).parent_path();
}

}  // namespace

int main() {
  const auto sid = zhixu::ipc::CurrentUserSid();
  if (!sid) {
    return 1;
  }
  const std::wstring mutex_name = L"Local\\ZhixuIME.Broker." + *sid;
  HANDLE mutex = CreateMutexW(nullptr, TRUE, mutex_name.c_str());
  if (mutex == nullptr) {
    return 1;
  }
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    CloseHandle(mutex);
    return 0;
  }

  const auto directory = ExecutableDirectory();
  if (directory.empty()) {
    CloseHandle(mutex);
    return 1;
  }
  const auto runtime_paths = zhixu::paths::RuntimePaths::ForCurrentUser();
  if (!runtime_paths) {
    CloseHandle(mutex);
    return 1;
  }
  zhixu::settings::SettingsStore settings_store(runtime_paths->settings);
  zhixu::broker::SettingsControl settings_control(settings_store, runtime_paths->root);
  std::jthread control_server([&settings_control](const std::stop_token stop_token) {
    settings_control.Serve(L"broker-control", stop_token);
  });

  const std::array children = {
      zhixu::broker::ChildSpec{
          .role = zhixu::broker::ChildRole::kInputHost,
          .executable = directory / L"InputHost.exe",
      },
      zhixu::broker::ChildSpec{
          .role = zhixu::broker::ChildRole::kPredictService,
          .executable = directory / L"PredictService.exe",
      },
  };

  zhixu::broker::Win32ChildLauncher launcher;
  zhixu::broker::SessionSupervisor supervisor(launcher);
  const auto result = supervisor.Run(children, {});
  CloseHandle(mutex);
  for (const auto& child : result.children) {
    if (child.state == zhixu::broker::ChildRuntimeState::kDegraded) {
      return 1;
    }
  }
  return 0;
}
