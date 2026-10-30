#include "input_host/prediction/prediction_service_client.h"

#include <chrono>
#include <string>

namespace zhixu::input_host {
namespace {

constexpr std::string_view kSessionId = "input-host-predict";
constexpr auto kConnectDeadline = std::chrono::milliseconds(50);
constexpr auto kPredictionDeadline = std::chrono::milliseconds(200);

protocol::Envelope MakeEnvelope(const protocol::v1::MessageType type, const std::uint64_t request_id,
                                const std::uint64_t context_version) {
  protocol::Envelope envelope;
  envelope.set_protocol_version(protocol::kProtocolVersion);
  envelope.set_message_type(type);
  envelope.set_session_id(kSessionId);
  envelope.set_request_id(request_id);
  envelope.set_context_version(context_version);
  return envelope;
}

bool IsResponseFor(const protocol::Envelope& request, const protocol::Envelope& response,
                   const protocol::v1::MessageType type) {
  return response.protocol_version() == protocol::kProtocolVersion && response.message_type() == type &&
         response.session_id() == request.session_id() && response.request_id() == request.request_id() &&
         response.context_version() == request.context_version();
}

}  // namespace

PredictionServiceClient::PredictionServiceClient(ipc::PipeEndpoint endpoint) : endpoint_(std::move(endpoint)) {}

std::optional<protocol::v1::PredictionResult> PredictionServiceClient::Predict(
    const ConfirmedCommit& commit) {
  if (!commit.allow_prediction || commit.text.empty()) return std::nullopt;

  ipc::NamedPipeClient client;
  if (!client.Connect(endpoint_, kConnectDeadline)) return std::nullopt;

  const auto hello_id = next_request_id_.fetch_add(1);
  auto hello_envelope = MakeEnvelope(protocol::v1::HELLO, hello_id, commit.context_version);
  protocol::v1::Hello hello;
  hello.set_min_version(protocol::kProtocolVersion);
  hello.set_max_version(protocol::kProtocolVersion);
  if (!hello.SerializeToString(hello_envelope.mutable_payload())) return std::nullopt;
  const auto hello_response = client.Call(hello_envelope, kPredictionDeadline);
  if (!hello_response || !IsResponseFor(hello_envelope, *hello_response, protocol::v1::HELLO_ACK)) {
    return std::nullopt;
  }
  protocol::v1::HelloAck acknowledgment;
  if (!acknowledgment.ParseFromString(hello_response->payload()) ||
      acknowledgment.selected_version() != protocol::kProtocolVersion) {
    return std::nullopt;
  }

  const auto request_id = next_request_id_.fetch_add(1);
  auto request_envelope = MakeEnvelope(protocol::v1::PREDICTION_REQUEST, request_id, commit.context_version);
  protocol::v1::PredictionRequest request;
  request.set_context_text(commit.context_text.empty() ? commit.text : commit.context_text);
  request.set_candidate_limit(30);
  request.set_policy_generation(commit.policy_generation);
  request.set_deadline_tick_ms(static_cast<std::uint64_t>(GetTickCount64()) + kPredictionDeadline.count());
  request.mutable_privacy()->set_allow_prediction(true);
  if (!request.SerializeToString(request_envelope.mutable_payload())) return std::nullopt;
  const auto response = client.Call(request_envelope, kPredictionDeadline);
  if (!response || !IsResponseFor(request_envelope, *response, protocol::v1::PREDICTION_RESULT)) {
    return std::nullopt;
  }

  protocol::v1::PredictionResult result;
  if (!result.ParseFromString(response->payload())) return std::nullopt;
  return result;
}

bool PredictionServiceClient::Observe(const ConfirmedCommit& commit) {
 if(!commit.allow_learning || commit.text.empty() || commit.session_id.empty() || commit.commit_id==0)return false;
 ipc::NamedPipeClient client;if(!client.Connect(endpoint_,kConnectDeadline))return false;
 auto hello_envelope=MakeEnvelope(protocol::v1::HELLO,next_request_id_.fetch_add(1),commit.context_version);
 protocol::v1::Hello hello;hello.set_min_version(protocol::kProtocolVersion);hello.set_max_version(protocol::kProtocolVersion);
 hello.SerializeToString(hello_envelope.mutable_payload());const auto hello_response=client.Call(hello_envelope,kPredictionDeadline);
 if(!hello_response || !IsResponseFor(hello_envelope,*hello_response,protocol::v1::HELLO_ACK))return false;
 auto envelope=MakeEnvelope(protocol::v1::LEARNING_OBSERVATION,next_request_id_.fetch_add(1),commit.context_version);
 protocol::v1::LearningObservation event;event.set_policy_generation(commit.policy_generation);event.set_identity(commit.session_id+":"+std::to_string(commit.commit_id));
 event.set_committed_text(commit.text);event.set_context_before(commit.context_before);event.mutable_privacy()->set_allow_learning(true);
 event.SerializeToString(envelope.mutable_payload());const auto response=client.Call(envelope,kPredictionDeadline);
 if(!response || !IsResponseFor(envelope,*response,protocol::v1::CONTROL_RESULT))return false;
 protocol::v1::ControlResult result;return result.ParseFromString(response->payload()) && result.error_code()==0;
}

}  // namespace zhixu::input_host
