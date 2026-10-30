#include <gtest/gtest.h>

#include "tsf/key_event_sink.h"

namespace zhixu::tsf {
namespace {

class FakeTransport final : public EngineActionTransport {
 public:
  std::optional<protocol::v1::EngineAction> Dispatch(
      const KeyInput& event, const std::chrono::milliseconds deadline) override {
    last_event = event;
    last_deadline = deadline;
    return response;
  }

  std::optional<protocol::v1::EngineAction> response;
  KeyInput last_event{};
  std::chrono::milliseconds last_deadline{};
};

TEST(KeyEventSink, UsesShortDeadlineAndConsumesOnlyConfirmedAction) {
  FakeTransport transport;
  protocol::v1::EngineAction expected_action;
  expected_action.set_consumed(true);
  transport.response = expected_action;
  KeyEventSink sink(&transport, std::chrono::milliseconds(12));

  const auto received_action = sink.Handle({.virtual_key = 'A', .key_data = 7});
  ASSERT_TRUE(received_action);
  EXPECT_TRUE(received_action->consumed());
  EXPECT_EQ(transport.last_event.virtual_key, static_cast<WPARAM>('A'));
  EXPECT_EQ(transport.last_event.key_data, 7);
  EXPECT_EQ(transport.last_deadline, std::chrono::milliseconds(12));
}

TEST(KeyEventSink, MissingOrUnavailableTransportPassesKeyThrough) {
  KeyEventSink missing(nullptr);
  EXPECT_FALSE(missing.Handle({.virtual_key = 'A'}));

  FakeTransport unavailable;
  KeyEventSink sink(&unavailable);
  EXPECT_FALSE(sink.Handle({.virtual_key = 'A'}));
  protocol::v1::EngineAction pass_through;
  pass_through.set_consumed(false);
  unavailable.response = pass_through;
  const auto action = sink.Handle({.virtual_key = 'A'});
  ASSERT_TRUE(action);
  EXPECT_FALSE(action->consumed());
}

}  // namespace
}  // namespace zhixu::tsf
