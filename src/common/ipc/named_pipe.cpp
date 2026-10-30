#include "common/ipc/named_pipe.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cwctype>
#include <limits>
#include <vector>

namespace zhixu::ipc {
namespace {

class Event final {
 public:
  Event() : handle_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
  ~Event() {
    if (handle_ != nullptr) {
      CloseHandle(handle_);
    }
  }
  Event(const Event&) = delete;
  HANDLE get() const { return handle_; }
  bool valid() const { return handle_ != nullptr; }

 private:
  HANDLE handle_ = nullptr;
};

DWORD RemainingMilliseconds(const std::chrono::steady_clock::time_point deadline) {
  const auto now = std::chrono::steady_clock::now();
  if (now >= deadline) {
    return 0;
  }
  const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
  return static_cast<DWORD>(std::min<int64_t>(std::max<int64_t>(1, remaining), MAXDWORD - 1));
}

void CancelPendingIo(HANDLE pipe, OVERLAPPED& operation) {
  CancelIoEx(pipe, &operation);
  DWORD ignored = 0;
  // The kernel must finish using the stack OVERLAPPED/buffer before they die.
  GetOverlappedResult(pipe, &operation, &ignored, TRUE);
}

tl::expected<DWORD, PipeError> WaitForOverlapped(HANDLE pipe, OVERLAPPED& operation,
    const std::chrono::steady_clock::time_point deadline,
    const std::function<bool()>* lease = nullptr) {
  for (;;) {
    if (lease != nullptr && !(*lease)()) {
      CancelPendingIo(pipe, operation);
      return tl::unexpected(PipeError::Disconnected);
    }
    const DWORD remaining = RemainingMilliseconds(deadline);
    const DWORD wait = WaitForSingleObject(operation.hEvent,
        lease != nullptr ? std::min<DWORD>(remaining, 5) : remaining);
    if (wait == WAIT_OBJECT_0) break;
    if (wait == WAIT_TIMEOUT) {
      if (lease != nullptr && RemainingMilliseconds(deadline) != 0) continue;
      CancelPendingIo(pipe, operation);
      return tl::unexpected(PipeError::Timeout);
    }
    const DWORD error = GetLastError();
    CancelPendingIo(pipe, operation);
    return tl::unexpected(PipeErrorFromWin32(error));
  }
  DWORD transferred = 0;
  if (!GetOverlappedResult(pipe, &operation, &transferred, FALSE)) {
    return tl::unexpected(PipeErrorFromWin32(GetLastError()));
  }
  return transferred;
}

tl::expected<void, PipeError> ReadExact(HANDLE pipe, std::span<std::byte> target,
    const std::chrono::steady_clock::time_point deadline,
    const std::function<bool()>* lease = nullptr) {
  size_t offset = 0;
  while (offset < target.size()) {
    if (lease != nullptr && !(*lease)()) return tl::unexpected(PipeError::Disconnected);
    Event event;
    if (!event.valid()) {
      return tl::unexpected(PipeErrorFromWin32(GetLastError()));
    }
    OVERLAPPED operation{};
    operation.hEvent = event.get();
    DWORD read = 0;
    const DWORD requested = static_cast<DWORD>(std::min<size_t>(target.size() - offset, MAXDWORD));
    if (!ReadFile(pipe, target.data() + offset, requested, &read, &operation)) {
      const DWORD error = GetLastError();
      if (error != ERROR_IO_PENDING) {
        return tl::unexpected(PipeErrorFromWin32(error));
      }
      const auto completed = WaitForOverlapped(pipe, operation, deadline, lease);
      if (!completed) {
        return tl::unexpected(completed.error());
      }
      read = *completed;
    }
    if (read == 0) {
      return tl::unexpected(PipeError::Disconnected);
    }
    offset += read;
  }
  return {};
}

tl::expected<void, PipeError> WriteExact(HANDLE pipe, std::span<const std::byte> source,
    const std::chrono::steady_clock::time_point deadline,
    const std::function<bool()>* lease = nullptr) {
  size_t offset = 0;
  while (offset < source.size()) {
    if (lease != nullptr && !(*lease)()) return tl::unexpected(PipeError::Disconnected);
    Event event;
    if (!event.valid()) {
      return tl::unexpected(PipeErrorFromWin32(GetLastError()));
    }
    OVERLAPPED operation{};
    operation.hEvent = event.get();
    DWORD written = 0;
    const DWORD requested = static_cast<DWORD>(std::min<size_t>(source.size() - offset, MAXDWORD));
    if (!WriteFile(pipe, source.data() + offset, requested, &written, &operation)) {
      const DWORD error = GetLastError();
      if (error != ERROR_IO_PENDING) {
        return tl::unexpected(PipeErrorFromWin32(error));
      }
      const auto completed = WaitForOverlapped(pipe, operation, deadline, lease);
      if (!completed) {
        return tl::unexpected(completed.error());
      }
      written = *completed;
    }
    if (written == 0) {
      return tl::unexpected(PipeError::Disconnected);
    }
    offset += written;
  }
  return {};
}

tl::expected<protocol::Envelope, PipeError> ReadEnvelope(
    HANDLE pipe, const std::chrono::steady_clock::time_point deadline,
    const std::function<bool()>* lease = nullptr) {
  std::array<std::byte, sizeof(uint32_t)> prefix{};
  const auto read_prefix = ReadExact(pipe, prefix, deadline, lease);
  if (!read_prefix) {
    return tl::unexpected(read_prefix.error());
  }
  uint32_t size = 0;
  for (size_t index = 0; index < prefix.size(); ++index) {
    size |= static_cast<uint32_t>(std::to_integer<uint8_t>(prefix[index])) << (index * 8);
  }
  if (size > protocol::kMaxPayloadSize) {
    return tl::unexpected(PipeError::Protocol);
  }
  std::vector<std::byte> frame(sizeof(uint32_t) + size);
  std::copy(prefix.begin(), prefix.end(), frame.begin());
  const auto read_payload = ReadExact(pipe, std::span(frame).subspan(sizeof(uint32_t)), deadline, lease);
  if (!read_payload) {
    return tl::unexpected(read_payload.error());
  }
  const auto envelope = protocol::EnvelopeCodec::Decode(frame);
  if (!envelope) {
    return tl::unexpected(PipeError::Protocol);
  }
  return *envelope;
}

tl::expected<void, PipeError> WriteEnvelope(
    HANDLE pipe, const protocol::Envelope& envelope, const std::chrono::steady_clock::time_point deadline,
    const std::function<bool()>* lease = nullptr) {
  const auto frame = protocol::EnvelopeCodec::Encode(envelope);
  if (!frame) {
    return tl::unexpected(PipeError::Protocol);
  }
  return WriteExact(pipe, *frame, deadline, lease);
}

bool IsSafeRole(const std::wstring_view role) {
  return !role.empty() && role.size() <= 48 && std::all_of(role.begin(), role.end(), [](wchar_t character) {
           return (character >= L'a' && character <= L'z') ||
                  (character >= L'A' && character <= L'Z') ||
                  (character >= L'0' && character <= L'9') || character == L'_' || character == L'-';
         });
}

}  // namespace

tl::expected<PipeEndpoint, PipeError> PipeEndpoint::ForCurrentUser(const std::wstring_view role) {
  if (!IsSafeRole(role)) {
    return tl::unexpected(PipeError::InvalidEndpoint);
  }
  const auto sid = CurrentUserSid();
  if (!sid) {
    return tl::unexpected(sid.error());
  }
  return PipeEndpoint{.name = L"\\\\.\\pipe\\ZhixuIME." + std::wstring(role) + L"." + *sid, .sid = *sid};
}

NamedPipeServer::~NamedPipeServer() {
  Close();
}

tl::expected<void, PipeError> NamedPipeServer::Listen(
    const PipeEndpoint& endpoint, MessageHandler handler, const std::stop_token stop,
    ConnectionLease connection_lease) {
  closed_ = false;
  auto security = CreateCurrentUserPipeSecurity();
  if (!security) {
    return tl::unexpected(security.error());
  }
  HANDLE pipe = CreateNamedPipeW(endpoint.name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                 PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1,
                                 protocol::kMaxPayloadSize + sizeof(uint32_t),
                                 protocol::kMaxPayloadSize + sizeof(uint32_t), 0, &security->attributes);
  if (pipe == INVALID_HANDLE_VALUE) {
    return tl::unexpected(PipeErrorFromWin32(GetLastError()));
  }
  const auto owner = VerifyPipeOwner(pipe, endpoint.sid);
  if (!owner) {
    CloseHandle(pipe);
    return tl::unexpected(owner.error());
  }
  {
    std::lock_guard lock(mutex_);
    pipe_ = pipe;
    listening_ = true;
  }
  listening_condition_.notify_all();

  Event connect_event;
  OVERLAPPED connect{};
  connect.hEvent = connect_event.get();
  BOOL connected = ConnectNamedPipe(pipe, &connect);
  if (!connected) {
    const DWORD error = GetLastError();
    if (error == ERROR_IO_PENDING) {
      while (!closed_ && !stop.stop_requested()) {
        const DWORD result = WaitForSingleObject(connect_event.get(), 100);
        if (result == WAIT_OBJECT_0) {
          connected = TRUE;
          break;
        }
        if (result != WAIT_TIMEOUT) {
          CloseHandle(pipe);
          return tl::unexpected(PipeErrorFromWin32(GetLastError()));
        }
      }
    } else if (error == ERROR_PIPE_CONNECTED) {
      connected = TRUE;
    } else {
      CloseHandle(pipe);
      return tl::unexpected(PipeErrorFromWin32(error));
    }
  }
  if (!connected || closed_ || stop.stop_requested()) {
    CloseHandle(pipe);
    return {};
  }

  DWORD client_pid = 0;
  std::function<bool()> lease_check;
  if (connection_lease) {
    GetNamedPipeClientProcessId(pipe, &client_pid);
    lease_check = [&] {
      return client_pid != 0 && !closed_ && !stop.stop_requested() && connection_lease(client_pid);
    };
  }
  const auto* lease = lease_check ? &lease_check : nullptr;
  while (!closed_ && !stop.stop_requested()) {
    const auto request = ReadEnvelope(pipe, std::chrono::steady_clock::now() + std::chrono::hours(1), lease);
    if (!request) {
      break;
    }
    if (lease != nullptr && !(*lease)()) break;
    const auto response = handler(*request);
    const auto written = WriteEnvelope(pipe, response, std::chrono::steady_clock::now() + std::chrono::hours(1), lease);
    if (!written) {
      break;
    }
  }
  {
    std::lock_guard lock(mutex_);
    if (pipe_ == pipe) {
      pipe_ = INVALID_HANDLE_VALUE;
    }
    listening_ = false;
  }
  CloseHandle(pipe);
  return {};
}

bool NamedPipeServer::WaitUntilListening(const Deadline deadline) {
  std::unique_lock lock(mutex_);
  return listening_condition_.wait_for(lock, deadline, [this] { return listening_.load(); });
}

void NamedPipeServer::Close() {
  closed_ = true;
  std::lock_guard lock(mutex_);
  if (pipe_ != INVALID_HANDLE_VALUE) {
    CancelIoEx(pipe_, nullptr);
    DisconnectNamedPipe(pipe_);
  }
}

NamedPipeClient::~NamedPipeClient() {
  Close();
}

tl::expected<void, PipeError> NamedPipeClient::Connect(const PipeEndpoint& endpoint, const Deadline deadline) {
  Close();
  const auto end = std::chrono::steady_clock::now() + deadline;
  HANDLE pipe = INVALID_HANDLE_VALUE;
  while (std::chrono::steady_clock::now() < end) {
    pipe = CreateFileW(endpoint.name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                       OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (pipe != INVALID_HANDLE_VALUE) {
      break;
    }
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND) {
      const DWORD remaining = RemainingMilliseconds(end);
      if (remaining == 0) {
        break;
      }
      Sleep(std::min<DWORD>(remaining, 10));
      continue;
    }
    if (error != ERROR_PIPE_BUSY) {
      return tl::unexpected(PipeErrorFromWin32(error));
    }
    if (!WaitNamedPipeW(endpoint.name.c_str(), RemainingMilliseconds(end))) {
      const DWORD wait_error = GetLastError();
      if (wait_error != ERROR_FILE_NOT_FOUND) {
        return tl::unexpected(PipeErrorFromWin32(wait_error));
      }
    }
  }
  if (pipe == INVALID_HANDLE_VALUE) {
    return tl::unexpected(PipeError::Timeout);
  }
  const auto owner = VerifyPipeOwner(pipe, endpoint.sid);
  if (!owner) {
    CloseHandle(pipe);
    return tl::unexpected(owner.error());
  }
  std::lock_guard lock(mutex_);
  pipe_ = pipe;
  return {};
}

bool NamedPipeClient::IsConnected() {
  std::lock_guard lock(mutex_);
  return pipe_ != INVALID_HANDLE_VALUE && PeekNamedPipe(pipe_, nullptr, 0, nullptr, nullptr, nullptr) != FALSE;
}

tl::expected<protocol::Envelope, PipeError> NamedPipeClient::Call(
    const protocol::Envelope& request, const Deadline deadline) {
  std::lock_guard lock(mutex_);
  if (pipe_ == INVALID_HANDLE_VALUE) {
    return tl::unexpected(PipeError::Disconnected);
  }
  const auto end = std::chrono::steady_clock::now() + deadline;
  const auto written = WriteEnvelope(pipe_, request, end);
  if (!written) {
    return tl::unexpected(written.error());
  }
  return ReadEnvelope(pipe_, end);
}

void NamedPipeClient::Close() {
  std::lock_guard lock(mutex_);
  if (pipe_ != INVALID_HANDLE_VALUE) {
    CancelIoEx(pipe_, nullptr);
    CloseHandle(pipe_);
    pipe_ = INVALID_HANDLE_VALUE;
  }
}

}  // namespace zhixu::ipc
