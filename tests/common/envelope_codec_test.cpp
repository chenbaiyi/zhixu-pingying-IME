#include <gtest/gtest.h>
#include <array>
#include "common/protocol/envelope_codec.h"
using namespace zhixu::protocol;
TEST(EnvelopeCodec, RoundTripsVersionAndContext) {
  Envelope in;
  in.set_protocol_version(1); in.set_message_type(v1::KEY_EVENT);
  in.set_session_id("session-1"); in.set_request_id(41); in.set_context_version(9);
  in.set_payload("\0binary", 7);
  auto bytes = EnvelopeCodec::Encode(in);
  ASSERT_TRUE(bytes);
  auto out = EnvelopeCodec::Decode(*bytes);
  ASSERT_TRUE(out);
  EXPECT_EQ(out->protocol_version(), 1); EXPECT_EQ(out->message_type(), v1::KEY_EVENT);
  EXPECT_EQ(out->session_id(), "session-1"); EXPECT_EQ(out->request_id(), 41);
  EXPECT_EQ(out->context_version(), 9); EXPECT_EQ(out->payload(), std::string("\0binary", 7));
}
TEST(EnvelopeCodec, EncodesLittleEndianLength) {
  Envelope in; in.set_protocol_version(1);
  auto encoded = EnvelopeCodec::Encode(in);
  ASSERT_TRUE(encoded);
  EXPECT_EQ(*encoded, (std::vector<std::byte>{std::byte{2},std::byte{0},std::byte{0},std::byte{0},std::byte{8},std::byte{1}}));
}
TEST(EnvelopeCodec, RejectsFrameOverOneMiB) {
  std::vector<std::byte> bytes((1u << 20) + 5u);
  auto result = EnvelopeCodec::Decode(bytes);
  ASSERT_FALSE(result); EXPECT_EQ(result.error(), ProtocolError::FrameTooLarge);
}
TEST(EnvelopeCodec, RejectsOversizePrefixWithoutAllocating) {
  std::array bytes{std::byte{255},std::byte{255},std::byte{255},std::byte{255}};
  auto result = EnvelopeCodec::Decode(bytes);
  ASSERT_FALSE(result); EXPECT_EQ(result.error(), ProtocolError::FrameTooLarge);
}
TEST(EnvelopeCodec, RejectsTruncatedAndTrailingFrames) {
  for (size_t length = 0; length < 6; ++length) {
    std::vector<std::byte> bytes{std::byte{2},std::byte{0},std::byte{0},std::byte{0},std::byte{8},std::byte{1}};
    bytes.resize(length);
    auto result = EnvelopeCodec::Decode(bytes);
    ASSERT_FALSE(result); EXPECT_EQ(result.error(), ProtocolError::TruncatedFrame);
  }
  std::vector<std::byte> bytes{std::byte{2},std::byte{0},std::byte{0},std::byte{0},std::byte{8},std::byte{1},std::byte{0}};
  auto result = EnvelopeCodec::Decode(bytes);
  ASSERT_FALSE(result); EXPECT_EQ(result.error(), ProtocolError::TrailingBytes);
}
TEST(EnvelopeCodec, RejectsMalformedAndUnsupportedPayload) {
  std::array bytes{std::byte{2},std::byte{0},std::byte{0},std::byte{0},std::byte{8},std::byte{2}};
  auto result = EnvelopeCodec::Decode(bytes);
  ASSERT_FALSE(result); EXPECT_EQ(result.error(), ProtocolError::UnsupportedVersion);
  bytes[5] = std::byte{128}; result = EnvelopeCodec::Decode(bytes);
  ASSERT_FALSE(result); EXPECT_EQ(result.error(), ProtocolError::InvalidPayload);
}
TEST(EnvelopeCodec, RejectsOversizeEncode) {
  Envelope in; in.set_protocol_version(1); in.set_payload(std::string(1u << 20, 'a'));
  auto result = EnvelopeCodec::Encode(in);
  ASSERT_FALSE(result); EXPECT_EQ(result.error(), ProtocolError::FrameTooLarge);
}
TEST(EnvelopeCodec, AllowsMaximumPayloadAndUnknownProtoFields) {
  // Version (2 bytes), payload tag and three-byte length (4 bytes).
  Envelope in; in.set_protocol_version(1); in.set_payload(std::string((1u << 20) - 6, 'a'));
  auto encoded = EnvelopeCodec::Encode(in);
  ASSERT_TRUE(encoded); EXPECT_EQ(encoded->size(), (1u << 20) + 4u);
  ASSERT_TRUE(EnvelopeCodec::Decode(*encoded));
  std::array bytes{std::byte{4},std::byte{0},std::byte{0},std::byte{0},std::byte{8},std::byte{1},std::byte{120},std::byte{42}};
  ASSERT_TRUE(EnvelopeCodec::Decode(bytes));
}
