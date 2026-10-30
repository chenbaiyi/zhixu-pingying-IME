#include "common/protocol/protocol_handshake.h"
namespace zhixu::protocol {
tl::expected<HelloAck, ProtocolError> ProtocolHandshake::Accept(const Hello& hello) {
  if (accepted_) {
    return tl::unexpected(ProtocolError::UnexpectedMessage);
  }
  if (hello.min_version() == 0 || hello.min_version() > hello.max_version() ||
      hello.min_version() > kProtocolVersion || hello.max_version() < kProtocolVersion) {
    return tl::unexpected(ProtocolError::UnsupportedVersion);
  }

  HelloAck acknowledgment;
  acknowledgment.set_selected_version(kProtocolVersion);
  accepted_ = true;
  return acknowledgment;
}

tl::expected<void, ProtocolError> ProtocolHandshake::Validate(MessageType type) const {
  if (!accepted_) {
    return tl::unexpected(ProtocolError::HandshakeRequired);
  }
  if (!v1::MessageType_IsValid(type) || type == v1::MESSAGE_TYPE_UNSPECIFIED ||
      type == v1::HELLO || type == v1::HELLO_ACK) {
    return tl::unexpected(ProtocolError::UnexpectedMessage);
  }
  return {};
}
}
