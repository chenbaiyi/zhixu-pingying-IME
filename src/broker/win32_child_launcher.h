#pragma once

#include "broker/session_supervisor.h"

#include <Windows.h>

namespace zhixu::broker {

class Win32ChildLauncher final : public ChildLauncher {
 public:
  Win32ChildLauncher();
  ~Win32ChildLauncher() override;
  Win32ChildLauncher(const Win32ChildLauncher&) = delete;
  Win32ChildLauncher& operator=(const Win32ChildLauncher&) = delete;

  [[nodiscard]] std::unique_ptr<ChildProcess> Start(const ChildSpec& spec) override;

 private:
  HANDLE job_ = nullptr;
};

}  // namespace zhixu::broker
