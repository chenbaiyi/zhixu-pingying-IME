#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <thread>

#include "common/ipc/named_pipe.h"

using namespace std::chrono_literals;
using namespace zhixu::ipc;

TEST(NamedPipe, EndpointUsesCurrentUserSidAndRejectsInvalidRole) {
  auto first = PipeEndpoint::ForCurrentUser(L"test");
  auto second = PipeEndpoint::ForCurrentUser(L"test");
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  EXPECT_EQ(first->name, second->name);
  EXPECT_NE(first->name.find(L"\\\\.\\pipe\\ZhixuIME.test."), std::wstring::npos);
  EXPECT_NE(first->sid, L"");
  EXPECT_FALSE(PipeEndpoint::ForCurrentUser(L"../untrusted"));
}

TEST(NamedPipe, RoundTripsEnvelopeWithinDeadline) {
  auto endpoint = PipeEndpoint::ForCurrentUser(L"roundtrip");
  ASSERT_TRUE(endpoint);
  NamedPipeServer server;
  std::jthread loop([&] {
    const auto result = server.Listen(*endpoint, [](const zhixu::protocol::Envelope& request) {
      return request;
    });
    EXPECT_TRUE(result) << static_cast<int>(result.error());
  });
  ASSERT_TRUE(server.WaitUntilListening(1s));

  NamedPipeClient client;
  ASSERT_TRUE(client.Connect(*endpoint, 1s));
  zhixu::protocol::Envelope request;
  request.set_protocol_version(zhixu::protocol::kProtocolVersion);
  request.set_message_type(zhixu::protocol::v1::KEY_EVENT);
  request.set_session_id("pipe-session");
  request.set_request_id(9);
  const auto response = client.Call(request, 1s);
  ASSERT_TRUE(response);
  EXPECT_EQ(response->request_id(), 9);
  server.Close();
}

TEST(NamedPipe, ConnectDeadlineIsBounded) {
  auto endpoint = PipeEndpoint::ForCurrentUser(L"missing");
  ASSERT_TRUE(endpoint);
  NamedPipeClient client;
  const auto start = std::chrono::steady_clock::now();
  const auto result = client.Connect(*endpoint, 40ms);
  EXPECT_FALSE(result);
  EXPECT_EQ(result.error(), PipeError::Timeout);
  EXPECT_LT(std::chrono::steady_clock::now() - start, 500ms);
}

TEST(NamedPipe, InactiveClientLeaseDoesNotBlockNextConnection) {
  const auto endpoint = PipeEndpoint::ForCurrentUser(
      L"inactive-lease-" + std::to_wstring(GetCurrentProcessId()));
  ASSERT_TRUE(endpoint.has_value());
  NamedPipeServer server;
  std::atomic<bool> first_active{true};
  std::atomic<bool> server_failed{false};
  std::jthread listening([&](std::stop_token stop) {
    for (int connection = 0; connection < 2 && !stop.stop_requested(); ++connection) {
      const auto result = server.Listen(*endpoint,
          [](const zhixu::protocol::Envelope& request) { return request; }, stop,
          [&, connection](DWORD client_pid) {
            return client_pid != 0 && (connection != 0 || first_active.load());
          });
      if (!result) { server_failed = true; break; }
    }
  });
  NamedPipeClient first;
  NamedPipeClient second;
  const bool ready = server.WaitUntilListening(1s);
  const bool first_connected = ready && first.Connect(*endpoint, 1s).has_value();
  zhixu::protocol::Envelope request;
  request.set_protocol_version(zhixu::protocol::kProtocolVersion);
  request.set_message_type(zhixu::protocol::v1::HELLO);
  request.set_session_id("lease-test");
  request.set_request_id(41);
  bool first_replied = false;
  if (first_connected) first_replied = first.Call(request, 1s).has_value();
  // Lose foreground ownership, but deliberately keep the first handle open.
  first_active = false;
  const bool second_connected = first_replied && second.Connect(*endpoint, 600ms).has_value();
  bool second_replied = false;
  if (second_connected) {
    request.set_request_id(42);
    const auto response = second.Call(request, 1s);
    second_replied = response.has_value() && response->request_id() == 42;
  }
  listening.request_stop();
  server.Close();
  first.Close();
  second.Close();
  listening.join();
  EXPECT_TRUE(ready);
  EXPECT_TRUE(first_connected);
  EXPECT_TRUE(first_replied);
  EXPECT_FALSE(server_failed.load());
  EXPECT_TRUE(second_connected);
  EXPECT_TRUE(second_replied);
}

TEST(NamedPipe, ActiveClientLeaseRetainsIdleConnection) {
  const auto endpoint = PipeEndpoint::ForCurrentUser(
      L"active-lease-" + std::to_wstring(GetCurrentProcessId()));
  ASSERT_TRUE(endpoint.has_value());
  NamedPipeServer server;
  std::jthread listening([&](std::stop_token stop) {
    server.Listen(*endpoint, [](const zhixu::protocol::Envelope& request) { return request; },
        stop, [](DWORD client_pid) { return client_pid == GetCurrentProcessId(); });
  });
  NamedPipeClient client;
  const bool ready = server.WaitUntilListening(1s);
  const bool connected = ready && client.Connect(*endpoint, 1s).has_value();
  zhixu::protocol::Envelope request;
  request.set_protocol_version(zhixu::protocol::kProtocolVersion);
  request.set_message_type(zhixu::protocol::v1::HELLO);
  request.set_session_id("active-lease-test");
  request.set_request_id(1);
  bool first_replied = false;
  if (connected) first_replied = client.Call(request, 1s).has_value();
  std::this_thread::sleep_for(50ms);  // Multiple lease polls; no new key needed.
  bool second_replied = false;
  if (first_replied) {
    request.set_request_id(2);
    const auto response = client.Call(request, 1s);
    second_replied = response.has_value() && response->request_id() == 2;
  }
  listening.request_stop();
  server.Close();
  client.Close();
  listening.join();
  EXPECT_TRUE(connected);
  EXPECT_TRUE(first_replied);
  EXPECT_TRUE(second_replied);
}

TEST(NamedPipe, ConnectWaitsForServerCreatedWithinDeadline) {
  const auto endpoint = PipeEndpoint::ForCurrentUser(L"delayed-connect");
  ASSERT_TRUE(endpoint);

  NamedPipeClient client;
  std::promise<tl::expected<void, PipeError>> connected;
  auto pending = connected.get_future();
  std::jthread connecting([&] { connected.set_value(client.Connect(*endpoint, 1s)); });

  std::this_thread::sleep_for(50ms);
  NamedPipeServer server;
  std::jthread listening([&] {
    const auto result = server.Listen(*endpoint, [](const zhixu::protocol::Envelope& request) {
      return request;
    });
    EXPECT_TRUE(result) << static_cast<int>(result.error());
  });

  ASSERT_TRUE(server.WaitUntilListening(1s));
  ASSERT_EQ(pending.wait_for(1s), std::future_status::ready);
  EXPECT_TRUE(pending.get());
  server.Close();
}

TEST(NamedPipe, ClosingServerUnblocksPendingCall) {
  auto endpoint = PipeEndpoint::ForCurrentUser(L"disconnect");
  ASSERT_TRUE(endpoint);
  NamedPipeServer server;
  std::jthread loop([&] {
    const auto result = server.Listen(*endpoint, [](const zhixu::protocol::Envelope&) {
      std::this_thread::sleep_for(5s);
      return zhixu::protocol::Envelope{};
    });
    EXPECT_TRUE(result) << static_cast<int>(result.error());
  });
  ASSERT_TRUE(server.WaitUntilListening(1s));
  NamedPipeClient client;
  ASSERT_TRUE(client.Connect(*endpoint, 1s));
  zhixu::protocol::Envelope request;
  request.set_protocol_version(zhixu::protocol::kProtocolVersion);
  auto pending = std::async(std::launch::async, [&] { return client.Call(request, 5s); });
  std::this_thread::sleep_for(40ms);
  server.Close();
  const auto result = pending.get();
  EXPECT_FALSE(result);
  EXPECT_EQ(result.error(), PipeError::Disconnected);
}
