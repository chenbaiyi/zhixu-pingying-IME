#include "common/protocol/envelope_codec.h"

#include <array>
#include <cstdint>

namespace zhixu::protocol {
tl::expected<std::vector<std::byte>, ProtocolError> EnvelopeCodec::Encode(const Envelope& envelope) {
  const auto payload_size = envelope.ByteSizeLong();
  if (payload_size > kMaxPayloadSize || payload_size > UINT32_MAX) {
    return tl::unexpected(ProtocolError::FrameTooLarge);
  }

  std::vector<std::byte> frame(sizeof(uint32_t) + payload_size);
  const auto length = static_cast<uint32_t>(payload_size);
  for (size_t i = 0; i < sizeof(length); ++i) {
    frame[i] = static_cast<std::byte>((length >> (i * 8)) & 0xff);
  }
  if (!envelope.SerializeToArray(frame.data() + sizeof(length), static_cast<int>(payload_size))) {
    return tl::unexpected(ProtocolError::SerializationFailed);
  }
  return frame;
}

tl::expected<Envelope, ProtocolError> EnvelopeCodec::Decode(std::span<const std::byte> frame) {
  if (frame.size() > sizeof(uint32_t) + kMaxPayloadSize) {
    return tl::unexpected(ProtocolError::FrameTooLarge);
  }
  if (frame.size() < sizeof(uint32_t)) {
    return tl::unexpected(ProtocolError::TruncatedFrame);
  }

  uint32_t payload_size = 0;
  for (size_t i = 0; i < sizeof(payload_size); ++i) {
    payload_size |= static_cast<uint32_t>(std::to_integer<uint8_t>(frame[i])) << (i * 8);
  }
  if (payload_size > kMaxPayloadSize) {
    return tl::unexpected(ProtocolError::FrameTooLarge);
  }
  if (frame.size() < sizeof(uint32_t) + payload_size) {
    return tl::unexpected(ProtocolError::TruncatedFrame);
  }
  if (frame.size() != sizeof(uint32_t) + payload_size) {
    return tl::unexpected(ProtocolError::TrailingBytes);
  }

  Envelope envelope;
  if (!envelope.ParseFromArray(frame.data() + sizeof(uint32_t), static_cast<int>(payload_size))) {
    return tl::unexpected(ProtocolError::InvalidPayload);
  }
  if (envelope.protocol_version() != kProtocolVersion) {
    return tl::unexpected(ProtocolError::UnsupportedVersion);
  }
  return envelope;
}
}
