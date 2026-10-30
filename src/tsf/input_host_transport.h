#pragma once

#include "common/ipc/named_pipe.h"
#include "tsf/key_event_sink.h"

#include <atomic>
#include <string>
#include <string_view>

namespace zhixu::tsf {

class InputHostTransport final : public EngineActionTransport {
 public:
  explicit InputHostTransport(std::wstring_view role = L"tsf-input");

  bool Connect(std::chrono::milliseconds deadline) override;
  void Disconnect() noexcept override;
  std::optional<protocol::v1::EngineAction> PollRecommendation(
      std::uint64_t context_version, const protocol::v1::PrivacyDecision& privacy,
      std::chrono::milliseconds deadline) override;
  std::optional<protocol::v1::EngineAction> Dispatch(
      const KeyInput& event, std::chrono::milliseconds deadline) override;
  void CommitApplied(std::uint64_t request_id, std::uint64_t context_version) noexcept override;
  void CommitRejected(std::uint64_t request_id, HRESULT error) noexcept override;
  void UpdateCaret(const protocol::v1::CaretUpdate& caret, std::uint64_t context_version) noexcept override;
  void InvalidateSession(protocol::v1::InvalidationReason reason, std::uint64_t context_version) noexcept override;

 private:
  [[nodiscard]] protocol::Envelope MakeRequest(protocol::v1::MessageType type,
                                                std::uint64_t context_version = 0);
  [[nodiscard]] bool IsMatchingResponse(const protocol::Envelope& request,
                                        const protocol::Envelope& response,
                                        protocol::v1::MessageType type) const noexcept;
  void Notify(protocol::v1::MessageType type, const google::protobuf::MessageLite& message,
              std::uint64_t context_version) noexcept;

  std::wstring role_;
  std::string session_id_;
  ipc::NamedPipeClient client_;
  std::atomic_uint64_t next_request_id_{1};
  std::atomic_bool handshaken_{false};
};

}  // namespace zhixu::tsf
