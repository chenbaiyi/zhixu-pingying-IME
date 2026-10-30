#pragma once

#include <Windows.h>

#include <string>
#include <tl/expected.hpp>

namespace zhixu::ipc {

enum class PipeError {
  AccessDenied,
  Disconnected,
  InvalidEndpoint,
  IoFailed,
  Protocol,
  Timeout,
};

PipeError PipeErrorFromWin32(DWORD error);
tl::expected<std::wstring, PipeError> CurrentUserSid();

struct PipeSecurity {
  PipeSecurity();
  ~PipeSecurity();
  PipeSecurity(PipeSecurity&& other) noexcept;
  PipeSecurity& operator=(PipeSecurity&& other) noexcept;
  PipeSecurity(const PipeSecurity&) = delete;
  PipeSecurity& operator=(const PipeSecurity&) = delete;

  SECURITY_ATTRIBUTES attributes{};
  PSECURITY_DESCRIPTOR descriptor = nullptr;
};

tl::expected<PipeSecurity, PipeError> CreateCurrentUserPipeSecurity();
tl::expected<void, PipeError> VerifyPipeOwner(HANDLE pipe, const std::wstring& expected_sid);

}  // namespace zhixu::ipc
