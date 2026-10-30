#include "broker/win32_child_launcher.h"

#include <filesystem>
#include <string>
#include <utility>

namespace zhixu::broker {
namespace {

class Win32ChildProcess final : public ChildProcess {
 public:
  explicit Win32ChildProcess(HANDLE process) : process_(process) {}
  ~Win32ChildProcess() override {
    if (process_ != nullptr) {
      CloseHandle(process_);
    }
  }

  std::optional<ChildExit> TryGetExit() override {
    const DWORD wait = WaitForSingleObject(process_, 0);
    if (wait == WAIT_TIMEOUT) {
      return std::nullopt;
    }
    if (wait != WAIT_OBJECT_0) {
      return ChildExit{.kind = ChildExitKind::kAbnormal, .exit_code = GetLastError()};
    }
    DWORD exit_code = 0;
    if (!GetExitCodeProcess(process_, &exit_code)) {
      return ChildExit{.kind = ChildExitKind::kAbnormal, .exit_code = GetLastError()};
    }
    return ChildExit{
        .kind = exit_code == 0 ? ChildExitKind::kNormal : ChildExitKind::kAbnormal,
        .exit_code = exit_code,
    };
  }

  void Terminate() noexcept override {
    if (process_ != nullptr) {
      TerminateProcess(process_, ERROR_CANCELLED);
    }
  }

 private:
  HANDLE process_ = nullptr;
};

}  // namespace

Win32ChildLauncher::Win32ChildLauncher() : job_(CreateJobObjectW(nullptr, nullptr)) {
  if (job_ == nullptr) {
    return;
  }
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits,
                               sizeof(limits))) {
    CloseHandle(job_);
    job_ = nullptr;
  }
}

Win32ChildLauncher::~Win32ChildLauncher() {
  if (job_ != nullptr) {
    CloseHandle(job_);
  }
}

std::unique_ptr<ChildProcess> Win32ChildLauncher::Start(const ChildSpec& spec) {
  if (job_ == nullptr || !spec.executable.is_absolute() ||
      !std::filesystem::is_regular_file(spec.executable)) {
    return nullptr;
  }

  const auto executable = std::filesystem::canonical(spec.executable).wstring();
  std::wstring command_line = L"\"" + executable + L"\"";
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(executable.c_str(), command_line.data(), nullptr, nullptr, FALSE,
                      CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr, &startup, &process)) {
    return nullptr;
  }
  CloseHandle(process.hThread);
  if (!AssignProcessToJobObject(job_, process.hProcess)) {
    TerminateProcess(process.hProcess, ERROR_ACCESS_DENIED);
    CloseHandle(process.hProcess);
    return nullptr;
  }
  return std::make_unique<Win32ChildProcess>(process.hProcess);
}

}  // namespace zhixu::broker
