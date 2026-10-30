#include "tsf/input_host_transport.h"
#include "common/settings/runtime_settings.h"

#include <Windows.h>

#include <string>
#include <utility>

namespace zhixu::tsf {
namespace {

constexpr std::chrono::milliseconds kCommitDeadline{100};

}  // namespace

InputHostTransport::InputHostTransport(const std::wstring_view role)
    : role_(role), session_id_("tsf-" + std::to_string(GetCurrentProcessId())) {}

bool InputHostTransport::Connect(const std::chrono::milliseconds deadline) {
  const auto expires = std::chrono::steady_clock::now() + deadline;
  if (handshaken_ && client_.IsConnected()) return true;
  handshaken_ = false;
  const auto endpoint = ipc::PipeEndpoint::ForCurrentUser(role_);
  if (!endpoint || !client_.Connect(*endpoint, deadline)) {
    return false;
  }

  protocol::Hello hello;
  hello.set_min_version(protocol::kProtocolVersion);
  hello.set_max_version(protocol::kProtocolVersion);
  auto request = MakeRequest(protocol::v1::HELLO);
  if (!hello.SerializeToString(request.mutable_payload())) {
    client_.Close();
    return false;
  }
  const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
      expires - std::chrono::steady_clock::now());
  if (remaining <= std::chrono::milliseconds::zero()) {
    client_.Close();
    return false;
  }
  const auto response = client_.Call(request, remaining);
  if (!response || !IsMatchingResponse(request, *response, protocol::v1::HELLO_ACK)) {
    client_.Close();
    return false;
  }
  protocol::HelloAck acknowledgment;
  if (!acknowledgment.ParseFromString(response->payload()) ||
      acknowledgment.selected_version() != protocol::kProtocolVersion) {
    client_.Close();
    return false;
  }
  handshaken_ = true;
  return true;
}

void InputHostTransport::Disconnect() noexcept {
  handshaken_ = false;
  client_.Close();
}

std::optional<protocol::v1::EngineAction> InputHostTransport::Dispatch(
    const KeyInput& event, const std::chrono::milliseconds deadline) {
  const auto expires = std::chrono::steady_clock::now() + deadline;
  if (!Connect(deadline)) {
    return std::nullopt;
  }
  protocol::v1::KeyEvent key_event;
  key_event.set_key_code(static_cast<std::uint32_t>(event.virtual_key));
  key_event.set_scan_code(static_cast<std::uint32_t>((event.key_data >> 16) & 0xff));
  key_event.set_modifiers(event.modifiers);
  key_event.set_key_down(event.key_down);
  key_event.set_key_symbol(event.key_symbol);
  key_event.set_application_hash(settings::CurrentApplicationHash());
  key_event.set_repeated(event.repeated);
  key_event.mutable_privacy()->set_allow_composition(event.allow_composition);
  key_event.mutable_privacy()->set_allow_learning(event.allow_learning);
  key_event.mutable_privacy()->set_allow_prediction(event.allow_prediction);

  auto request = MakeRequest(protocol::v1::KEY_EVENT, event.context_version);
  if (!key_event.SerializeToString(request.mutable_payload())) {
    return std::nullopt;
  }
  const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
      expires - std::chrono::steady_clock::now());
  if (remaining <= std::chrono::milliseconds::zero()) return std::nullopt;
  const auto response = client_.Call(request, remaining);
  if (!response || !IsMatchingResponse(request, *response, protocol::v1::ENGINE_ACTION)) {
    handshaken_ = false;
    client_.Close();
    return std::nullopt;
  }
  protocol::v1::EngineAction action;
  if (!action.ParseFromString(response->payload())) {
    return std::nullopt;
  }
  return action;
}

std::optional<protocol::v1::EngineAction> InputHostTransport::PollRecommendation(
    const std::uint64_t context_version, const protocol::v1::PrivacyDecision& privacy,
    const std::chrono::milliseconds deadline) {
  if (!handshaken_) return std::nullopt;
  protocol::v1::PollRecommendation poll;
  *poll.mutable_privacy() = privacy;
  poll.set_application_hash(settings::CurrentApplicationHash());
  auto request = MakeRequest(protocol::v1::POLL_RECOMMENDATION, context_version);
  if (!poll.SerializeToString(request.mutable_payload())) return std::nullopt;
  const auto response = client_.Call(request, deadline);
  if (!response || !IsMatchingResponse(request, *response, protocol::v1::ENGINE_ACTION)) {
    handshaken_ = false;
    client_.Close();
    return std::nullopt;
  }
  protocol::v1::EngineAction action;
  if (!action.ParseFromString(response->payload())) return std::nullopt;
  return action;
}

void InputHostTransport::CommitApplied(const std::uint64_t request_id,
                                       const std::uint64_t context_version) noexcept {
  // InputHost advances its session version only after this acknowledgement.
  protocol::v1::CommitApplied applied;
  applied.set_request_id(request_id);
  applied.set_context_version(context_version);
  Notify(protocol::v1::COMMIT_APPLIED, applied, context_version);
}

void InputHostTransport::CommitRejected(const std::uint64_t request_id, const HRESULT error) noexcept {
  protocol::v1::CommitRejected rejected;
  rejected.set_request_id(request_id);
  rejected.set_hresult(static_cast<std::int32_t>(error));
  Notify(protocol::v1::COMMIT_REJECTED, rejected, 0);
}

void InputHostTransport::UpdateCaret(const protocol::v1::CaretUpdate& caret,
                                     const std::uint64_t context_version) noexcept {
  Notify(protocol::v1::CARET_UPDATE, caret, context_version);
}

void InputHostTransport::InvalidateSession(const protocol::v1::InvalidationReason reason,
                                           const std::uint64_t context_version) noexcept {
  protocol::v1::SessionInvalidated invalidation;
  invalidation.set_reason(reason);
  invalidation.set_context_version(context_version);
  Notify(protocol::v1::SESSION_INVALIDATED, invalidation, context_version);
}

protocol::Envelope InputHostTransport::MakeRequest(const protocol::v1::MessageType type,
                                                    const std::uint64_t context_version) {
  protocol::Envelope request;
  request.set_protocol_version(protocol::kProtocolVersion);
  request.set_message_type(type);
  request.set_session_id(session_id_);
  request.set_request_id(next_request_id_++);
  request.set_context_version(context_version);
  return request;
}

bool InputHostTransport::IsMatchingResponse(const protocol::Envelope& request,
                                            const protocol::Envelope& response,
                                            const protocol::v1::MessageType type) const noexcept {
  return response.protocol_version() == protocol::kProtocolVersion &&
         response.message_type() == type && response.session_id() == request.session_id() &&
         response.request_id() == request.request_id();
}

void InputHostTransport::Notify(const protocol::v1::MessageType type,
                                const google::protobuf::MessageLite& message,
                                const std::uint64_t context_version) noexcept {
  if (!handshaken_) {
    return;
  }
  try {
    auto request = MakeRequest(type, context_version);
    if (!message.SerializeToString(request.mutable_payload())) {
      return;
    }
    const auto response = client_.Call(request, kCommitDeadline);
    if (!response || !IsMatchingResponse(request, *response, type)) {
      handshaken_ = false;
      client_.Close();
    }
  } catch (...) {
    handshaken_ = false;
    client_.Close();
  }
}

}  // namespace zhixu::tsf
