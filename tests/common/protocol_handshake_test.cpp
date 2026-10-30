#include <gtest/gtest.h>
#include "common/protocol/protocol_handshake.h"
using namespace zhixu::protocol;
TEST(ProtocolHandshake, RejectsBusinessMessageBeforeHello) {
  ProtocolHandshake handshake; auto result = handshake.Validate(v1::KEY_EVENT);
  ASSERT_FALSE(result); EXPECT_EQ(result.error(), ProtocolError::HandshakeRequired);
}
TEST(ProtocolHandshake, RejectsUnsupportedVersionWithoutTextPayload) {
  ProtocolHandshake handshake; Hello hello; hello.set_min_version(2); hello.set_max_version(2);
  auto result = handshake.Accept(hello);
  ASSERT_FALSE(result); EXPECT_EQ(result.error(), ProtocolError::UnsupportedVersion);
  EXPECT_FALSE(handshake.Validate(v1::KEY_EVENT));
}
TEST(ProtocolHandshake, AcceptsOverlappingRangeAndResetsOnReconnect) {
  ProtocolHandshake handshake; Hello hello; hello.set_min_version(1); hello.set_max_version(3);
  auto ack = handshake.Accept(hello);
  ASSERT_TRUE(ack); EXPECT_EQ(ack->selected_version(), 1);
  EXPECT_TRUE(handshake.Validate(v1::KEY_EVENT)); EXPECT_TRUE(handshake.Validate(v1::CONTROL_COMMAND));
  EXPECT_FALSE(handshake.Validate(v1::MESSAGE_TYPE_UNSPECIFIED));
  EXPECT_FALSE(handshake.Validate(static_cast<MessageType>(999))); EXPECT_FALSE(handshake.Accept(hello));
  handshake.Reset(); EXPECT_FALSE(handshake.Validate(v1::KEY_EVENT)); EXPECT_TRUE(handshake.Accept(hello));
}
TEST(ProtocolHandshake, RejectsReversedAndZeroRanges) {
  for (auto range : {std::pair{2u,1u}, std::pair{0u,1u}, std::pair{0u,0u}}) {
    ProtocolHandshake handshake; Hello hello;
    hello.set_min_version(range.first); hello.set_max_version(range.second);
    auto ack = handshake.Accept(hello);
    ASSERT_FALSE(ack); EXPECT_EQ(ack.error(), ProtocolError::UnsupportedVersion);
  }
}
