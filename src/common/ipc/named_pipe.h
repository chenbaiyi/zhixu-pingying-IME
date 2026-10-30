#pragma once

#include "common/protocol/envelope_codec.h"

#include <Windows.h>

// Wincon's legacy KEY_EVENT macro collides with the versioned wire enum.
// This transport does not use console input records.
#ifdef KEY_EVENT
#undef KEY_EVENT
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <stop_token>
#include <tl/expected.hpp>

#include "common/ipc/pipe_security.h"

namespace zhixu::ipc {

using Deadline = std::chrono::milliseconds;

struct PipeEndpoint {
  std::wstring name;
  std::wstring sid;

  static tl::expected<PipeEndpoint, PipeError> ForCurrentUser(std::wstring_view role);
};

using MessageHandler = std::function<protocol::Envelope(const protocol::Envelope&)>;
using ConnectionLease = std::function<bool(DWORD client_process_id)>;

class NamedPipeServer {
 public:
  NamedPipeServer() = default;
  ~NamedPipeServer();
  NamedPipeServer(const NamedPipeServer&) = delete;
  NamedPipeServer& operator=(const NamedPipeServer&) = delete;

  tl::expected<void, PipeError> Listen(
      const PipeEndpoint& endpoint, MessageHandler handler, std::stop_token stop = {},
      ConnectionLease connection_lease = {});
  bool WaitUntilListening(Deadline deadline);
  void Close();

 private:
  std::mutex mutex_;
  std::condition_variable listening_condition_;
  HANDLE pipe_ = INVALID_HANDLE_VALUE;
  std::atomic_bool listening_ = false;
  std::atomic_bool closed_ = false;
};

class NamedPipeClient {
 public:
  NamedPipeClient() = default;
  ~NamedPipeClient();
  NamedPipeClient(const NamedPipeClient&) = delete;
  NamedPipeClient& operator=(const NamedPipeClient&) = delete;

  tl::expected<void, PipeError> Connect(const PipeEndpoint& endpoint, Deadline deadline);
  bool IsConnected();
  tl::expected<protocol::Envelope, PipeError> Call(const protocol::Envelope& request, Deadline deadline);
  void Close();

 private:
  std::mutex mutex_;
  HANDLE pipe_ = INVALID_HANDLE_VALUE;
};

}  // namespace zhixu::ipc
