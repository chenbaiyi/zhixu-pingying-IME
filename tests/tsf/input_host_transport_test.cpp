#include "tsf/input_host_transport.h"

#include "common/ipc/named_pipe.h"
#include "common/protocol/protocol_handshake.h"

#include <atomic>
#include <chrono>
#include <stop_token>
#include <thread>

#include <gtest/gtest.h>

namespace zhixu::tsf {
namespace {

using namespace std::chrono_literals;

protocol::Envelope Response(const protocol::Envelope& request,
                            const protocol::v1::MessageType type) {
  protocol::Envelope response;
  response.set_protocol_version(protocol::kProtocolVersion);
  response.set_message_type(type);
  response.set_session_id(request.session_id());
  response.set_request_id(request.request_id());
  return response;
}

TEST(InputHostTransport, DispatchReconnectsAfterInitialConnectionFailure) {
  constexpr std::wstring_view role = L"tsf-retry-after-start";
  InputHostTransport transport(role);
  ASSERT_FALSE(transport.Connect(20ms));
  const auto endpoint = ipc::PipeEndpoint::ForCurrentUser(role);
  ASSERT_TRUE(endpoint);
  ipc::NamedPipeServer server;
  std::jthread worker([&] {
    const auto served = server.Listen(*endpoint, [](const protocol::Envelope& request) {
      if (request.message_type() == protocol::v1::HELLO) {
        protocol::v1::HelloAck acknowledgment;
        acknowledgment.set_selected_version(protocol::kProtocolVersion);
        auto response = Response(request, protocol::v1::HELLO_ACK);
        acknowledgment.SerializeToString(response.mutable_payload());
        return response;
      }
      protocol::v1::EngineAction action;
      action.set_consumed(true);
      action.mutable_composition()->set_preedit("n");
      auto response = Response(request, protocol::v1::ENGINE_ACTION);
      action.SerializeToString(response.mutable_payload());
      return response;
    });
    EXPECT_TRUE(served);
  });
  const bool listening = server.WaitUntilListening(1s);
  const auto action = listening ? transport.Dispatch({.virtual_key = 'N'}, 1s) : std::nullopt;
  server.Close();
  worker.join();
  ASSERT_TRUE(listening);
  ASSERT_TRUE(action.has_value());
  EXPECT_TRUE(action->consumed());
  EXPECT_EQ(action->composition().preedit(), "n");
}

TEST(InputHostTransport, ReconnectsBeforeFirstKeyAfterServerReclaimsIdleConnection) {
  const auto role = L"tsf-reclaimed-" + std::to_wstring(GetCurrentProcessId());
  const auto endpoint = ipc::PipeEndpoint::ForCurrentUser(role);
  ASSERT_TRUE(endpoint.has_value());
  const auto reply = [](const protocol::Envelope& request) {
    if (request.message_type() == protocol::v1::HELLO) {
      protocol::HelloAck acknowledgment;
      acknowledgment.set_selected_version(protocol::kProtocolVersion);
      auto response = Response(request, protocol::v1::HELLO_ACK);
      acknowledgment.SerializeToString(response.mutable_payload());
      return response;
    }
    protocol::v1::EngineAction action;
    action.set_consumed(true);
    action.mutable_composition()->set_preedit("n");
    auto response = Response(request, protocol::v1::ENGINE_ACTION);
    action.SerializeToString(response.mutable_payload());
    return response;
  };
  ipc::NamedPipeServer first_server;
  std::jthread first_worker([&] { EXPECT_TRUE(first_server.Listen(*endpoint, reply)); });
  InputHostTransport transport(role);
  const bool initial_ready = first_server.WaitUntilListening(1s);
  const bool initial_connected = initial_ready && transport.Connect(1s);
  // Simulate the foreground lease revoking this still-cached connection.
  first_server.Close();
  first_worker.join();

  ipc::NamedPipeServer second_server;
  std::atomic<unsigned int> new_handshakes{0};
  std::jthread second_worker([&] {
    const auto served = second_server.Listen(*endpoint, [&](const protocol::Envelope& request) {
      if (request.message_type() == protocol::v1::HELLO) ++new_handshakes;
      return reply(request);
    });
    EXPECT_TRUE(served);
  });
  const bool second_ready = second_server.WaitUntilListening(1s);
  const bool reconnected = initial_connected && second_ready && transport.Connect(1s);
  const auto action = reconnected ? transport.Dispatch({.virtual_key = 'N'}, 1s) : std::nullopt;
  transport.Disconnect();
  second_server.Close();
  second_worker.join();
  EXPECT_TRUE(initial_connected);
  EXPECT_TRUE(second_ready);
  EXPECT_TRUE(reconnected);
  EXPECT_EQ(new_handshakes.load(), 1U);
  ASSERT_TRUE(action.has_value());
  EXPECT_TRUE(action->consumed());
  EXPECT_EQ(action->composition().preedit(), "n");
}

TEST(InputHostTransport, DeliversEngineActionAfterHandshake) {
  constexpr std::wstring_view kRole = L"tsf-transport-integration";
  const auto endpoint = ipc::PipeEndpoint::ForCurrentUser(kRole);
  ASSERT_TRUE(endpoint);

  ipc::NamedPipeServer server;
  std::atomic_bool commit_acknowledged = false;
  std::jthread server_thread([&](const std::stop_token) {
    const auto served = server.Listen(*endpoint, [&commit_acknowledged](const protocol::Envelope& request) {
      if (request.message_type() == protocol::v1::HELLO) {
        protocol::v1::HelloAck acknowledgment;
        acknowledgment.set_selected_version(protocol::kProtocolVersion);
        auto response = Response(request, protocol::v1::HELLO_ACK);
        acknowledgment.SerializeToString(response.mutable_payload());
        return response;
      }
      if (request.message_type() == protocol::v1::COMMIT_APPLIED) {
        protocol::v1::CommitApplied applied;
        EXPECT_TRUE(applied.ParseFromString(request.payload()));
        EXPECT_EQ(applied.request_id(), 44U);
        EXPECT_EQ(applied.context_version(), 10U);
        EXPECT_EQ(request.context_version(), 10U);
        commit_acknowledged = true;
        auto response = Response(request, protocol::v1::COMMIT_APPLIED);
        applied.SerializeToString(response.mutable_payload());
        return response;
      }
      if (request.message_type() == protocol::v1::POLL_RECOMMENDATION) {
        protocol::v1::PollRecommendation poll;
        EXPECT_TRUE(poll.ParseFromString(request.payload()));
        EXPECT_EQ(request.context_version(), 10U);
        EXPECT_TRUE(poll.privacy().allow_composition());
        EXPECT_TRUE(poll.privacy().allow_prediction());
        protocol::v1::EngineAction action;
        action.set_consumed(true);
        action.mutable_commit_request()->set_text("clicked");
        action.mutable_commit_request()->set_request_id(45);
        action.mutable_commit_request()->set_expected_context_version(10);
        auto response = Response(request, protocol::v1::ENGINE_ACTION);
        action.SerializeToString(response.mutable_payload());
        return response;
      }
      EXPECT_EQ(request.message_type(), protocol::v1::KEY_EVENT);
      EXPECT_EQ(request.context_version(), 9U);
      protocol::v1::KeyEvent key_event;
      EXPECT_TRUE(key_event.ParseFromString(request.payload()));
      EXPECT_EQ(key_event.key_code(), static_cast<std::uint32_t>('N'));
      EXPECT_EQ(key_event.modifiers(), 1U);
      EXPECT_EQ(key_event.key_symbol(), static_cast<std::uint32_t>('N'));
      EXPECT_FALSE(key_event.key_down());
      EXPECT_TRUE(key_event.repeated());
      protocol::v1::EngineAction action;
      action.set_consumed(true);
      action.mutable_composition()->set_preedit("ni");
      action.mutable_composition()->add_candidates()->set_text("你");
      auto response = Response(request, protocol::v1::ENGINE_ACTION);
      action.SerializeToString(response.mutable_payload());
      return response;
    }, {});
    EXPECT_TRUE(served) << static_cast<int>(served.error());
  });
  ASSERT_TRUE(server.WaitUntilListening(1s));

  InputHostTransport transport(kRole);
  ASSERT_TRUE(transport.Connect(1s));
  const auto action = transport.Dispatch({.virtual_key = 'N', .context_version = 9, .modifiers = 1,
                                          .repeated = true, .key_down = false, .key_symbol = 'N'}, 1s);

  ASSERT_TRUE(action.has_value());
  EXPECT_TRUE(action->consumed());
  EXPECT_EQ(action->composition().preedit(), "ni");
  ASSERT_EQ(action->composition().candidates_size(), 1);
  EXPECT_EQ(action->composition().candidates(0).text(), "你");
  transport.CommitApplied(44, 10);
  EXPECT_TRUE(commit_acknowledged);
  protocol::v1::PrivacyDecision privacy;
  privacy.set_allow_composition(true);
  privacy.set_allow_prediction(true);
  const auto clicked = transport.PollRecommendation(10, privacy, 1s);
  ASSERT_TRUE(clicked);
  ASSERT_TRUE(clicked->has_commit_request());
  EXPECT_EQ(clicked->commit_request().text(), "clicked");
  EXPECT_EQ(clicked->commit_request().expected_context_version(), 10U);
  server_thread.request_stop();
}

}  // namespace
}  // namespace zhixu::tsf

namespace zhixu::tsf { namespace {
TEST(InputHostTransport, DelayedFuzzyKeyPreservesEveryPrefixWithoutReconnect) {
  const auto role = L"tsf-delayed-fuzzy-" + std::to_wstring(GetCurrentProcessId());
  const auto endpoint = ipc::PipeEndpoint::ForCurrentUser(role);
  ASSERT_TRUE(endpoint);
  ipc::NamedPipeServer server;
  std::atomic<unsigned int> handshakes{0};
  std::string preedit;
  std::jthread worker([&](std::stop_token stop) {
    while (!stop.stop_requested()) {
      server.Listen(*endpoint, [&](const protocol::Envelope& request) {
        if (request.message_type() == protocol::v1::HELLO) {
          ++handshakes;
          preedit.clear();
          protocol::HelloAck ack;
          ack.set_selected_version(protocol::kProtocolVersion);
          auto response = Response(request, protocol::v1::HELLO_ACK);
          ack.SerializeToString(response.mutable_payload());
          return response;
        }
        protocol::v1::KeyEvent key;
        key.ParseFromString(request.payload());
        if (key.key_code() == 'O') std::this_thread::sleep_for(150ms);
        preedit.push_back(static_cast<char>(key.key_code() - 'A' + 'a'));
        protocol::v1::EngineAction action;
        action.set_consumed(true);
        action.mutable_composition()->set_preedit(preedit);
        auto response = Response(request, protocol::v1::ENGINE_ACTION);
        action.SerializeToString(response.mutable_payload());
        return response;
      }, stop);
    }
  });
  const bool ready = server.WaitUntilListening(1s);
  InputHostTransport transport(role);
  const bool connected = ready && transport.Connect(1s);
  KeyEventSink sink(&transport);
  std::string expected;
  if (connected) {
    for (char key : std::string("zhngow")) {
      expected.push_back(key);
      const auto action = sink.Handle({.virtual_key = static_cast<WPARAM>(key - 'a' + 'A')});
      EXPECT_TRUE(action) << expected;
      if (action) EXPECT_EQ(action->composition().preedit(), expected);
    }
  }
  transport.Disconnect();
  worker.request_stop();
  server.Close();
  worker.join();
  EXPECT_TRUE(connected);
  EXPECT_EQ(handshakes.load(), 1U);
}
} }