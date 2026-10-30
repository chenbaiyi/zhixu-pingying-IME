#include "common/ipc/pipe_security.h"

#include <Aclapi.h>
#include <Sddl.h>

#include <vector>

namespace zhixu::ipc {

PipeError PipeErrorFromWin32(const DWORD error) {
  switch (error) {
    case ERROR_ACCESS_DENIED:
    case ERROR_PRIVILEGE_NOT_HELD:
      return PipeError::AccessDenied;
    case ERROR_BROKEN_PIPE:
    case ERROR_NO_DATA:
    case ERROR_OPERATION_ABORTED:
    case ERROR_PIPE_NOT_CONNECTED:
      return PipeError::Disconnected;
    case ERROR_BAD_PATHNAME:
    case ERROR_INVALID_NAME:
      return PipeError::InvalidEndpoint;
    case ERROR_SEM_TIMEOUT:
    case ERROR_FILE_NOT_FOUND:
    case WAIT_TIMEOUT:
      return PipeError::Timeout;
    default:
      return PipeError::IoFailed;
  }
}

tl::expected<std::wstring, PipeError> CurrentUserSid() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
    return tl::unexpected(PipeErrorFromWin32(GetLastError()));
  }

  DWORD bytes = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
    const auto error = PipeErrorFromWin32(GetLastError());
    CloseHandle(token);
    return tl::unexpected(error);
  }
  std::vector<std::byte> buffer(bytes);
  if (!GetTokenInformation(token, TokenUser, buffer.data(), bytes, &bytes)) {
    const auto error = PipeErrorFromWin32(GetLastError());
    CloseHandle(token);
    return tl::unexpected(error);
  }
  CloseHandle(token);

  auto* user = reinterpret_cast<TOKEN_USER*>(buffer.data());
  PWSTR sid_text = nullptr;
  if (!ConvertSidToStringSidW(user->User.Sid, &sid_text)) {
    return tl::unexpected(PipeErrorFromWin32(GetLastError()));
  }
  std::wstring sid(sid_text);
  LocalFree(sid_text);
  return sid;
}

PipeSecurity::PipeSecurity() {
  attributes.nLength = sizeof(attributes);
}

PipeSecurity::~PipeSecurity() {
  if (descriptor != nullptr) {
    LocalFree(descriptor);
  }
}

PipeSecurity::PipeSecurity(PipeSecurity&& other) noexcept : attributes(other.attributes), descriptor(other.descriptor) {
  attributes.lpSecurityDescriptor = descriptor;
  other.attributes.lpSecurityDescriptor = nullptr;
  other.descriptor = nullptr;
}

PipeSecurity& PipeSecurity::operator=(PipeSecurity&& other) noexcept {
  if (this != &other) {
    if (descriptor != nullptr) {
      LocalFree(descriptor);
    }
    attributes = other.attributes;
    descriptor = other.descriptor;
    attributes.lpSecurityDescriptor = descriptor;
    other.attributes.lpSecurityDescriptor = nullptr;
    other.descriptor = nullptr;
  }
  return *this;
}

tl::expected<PipeSecurity, PipeError> CreateCurrentUserPipeSecurity() {
  const auto sid = CurrentUserSid();
  if (!sid) {
    return tl::unexpected(sid.error());
  }

  PipeSecurity security;
  const std::wstring sddl = L"O:" + *sid + L"D:P(A;;GA;;;SY)(A;;GA;;;" + *sid + L")";
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          sddl.c_str(), SDDL_REVISION_1, &security.descriptor, nullptr)) {
    return tl::unexpected(PipeErrorFromWin32(GetLastError()));
  }
  security.attributes.lpSecurityDescriptor = security.descriptor;
  security.attributes.bInheritHandle = FALSE;
  return security;
}

tl::expected<void, PipeError> VerifyPipeOwner(HANDLE pipe, const std::wstring& expected_sid) {
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  PSID owner = nullptr;
  const DWORD status = GetSecurityInfo(
      pipe, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr, &descriptor);
  if (status != ERROR_SUCCESS) {
    return tl::unexpected(PipeErrorFromWin32(status));
  }

  PSID expected = nullptr;
  const BOOL converted = ConvertStringSidToSidW(expected_sid.c_str(), &expected);
  const bool matches = converted && owner != nullptr && EqualSid(owner, expected) != FALSE;
  if (expected != nullptr) {
    LocalFree(expected);
  }
  LocalFree(descriptor);
  if (!matches) {
    return tl::unexpected(converted ? PipeError::AccessDenied : PipeErrorFromWin32(GetLastError()));
  }
  return {};
}

}  // namespace zhixu::ipc
