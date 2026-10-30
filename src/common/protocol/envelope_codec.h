#pragma once
#include <cstddef>
#include <span>
#include <vector>
#include <tl/expected.hpp>
#include "proto/ime.pb.h"
namespace zhixu::protocol {
using v1::Envelope; using v1::Hello; using v1::HelloAck; using v1::MessageType;
inline constexpr uint32_t kProtocolVersion = 1;
inline constexpr size_t kMaxPayloadSize = 1u << 20;
enum class ProtocolError { FrameTooLarge, TruncatedFrame, TrailingBytes, InvalidPayload, UnsupportedVersion, HandshakeRequired, UnexpectedMessage, SerializationFailed };
class EnvelopeCodec {
 public:
  static tl::expected<std::vector<std::byte>, ProtocolError> Encode(const Envelope&);
  static tl::expected<Envelope, ProtocolError> Decode(std::span<const std::byte>);
};
}
