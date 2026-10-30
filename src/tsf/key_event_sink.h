#pragma once

// Wincon defines KEY_EVENT as a macro.  The wire protocol uses the same name.
#ifdef KEY_EVENT
#undef KEY_EVENT
#endif

#include "common/protocol/envelope_codec.h"

#include <Windows.h>

#include <chrono>
#include <cstdint>
#include <optional>

namespace zhixu::tsf {

struct KeyInput {
  WPARAM virtual_key = 0;
  LPARAM key_data = 0;
  std::uint64_t context_version = 0;
  std::uint32_t modifiers = 0;
  bool repeated = false;
  bool allow_composition = true;
  bool allow_learning = true;
  bool allow_prediction = true;
  bool key_down = true;
  std::uint32_t key_symbol = 0;
};

class EngineActionTransport {
 public:
  virtual ~EngineActionTransport() = default;
  virtual bool Connect(std::chrono::milliseconds) { return true; }
  virtual void Disconnect() noexcept {}
  virtual std::optional<protocol::v1::EngineAction> Dispatch(
      const KeyInput& event, std::chrono::milliseconds deadline) = 0;
  virtual void CommitApplied(std::uint64_t, std::uint64_t) noexcept {}
  virtual void CommitRejected(std::uint64_t, HRESULT) noexcept {}
  virtual void UpdateCaret(const protocol::v1::CaretUpdate&, std::uint64_t) noexcept {}
  virtual void InvalidateSession(protocol::v1::InvalidationReason, std::uint64_t) noexcept {}
  virtual std::optional<protocol::v1::EngineAction> PollRecommendation(
      std::uint64_t, const protocol::v1::PrivacyDecision&, std::chrono::milliseconds) { return std::nullopt; }
};

// The 30ms candidate target is not a safe transport failure threshold.
// Invalid/fuzzy input can exceed 100ms; allow a bounded 500ms response before
// disconnecting, which resets Rime composition on the next handshake.
class KeyEventSink final {
 public:
  explicit KeyEventSink(EngineActionTransport* transport,
                        std::chrono::milliseconds deadline = std::chrono::milliseconds(500)) noexcept;

  [[nodiscard]] std::optional<protocol::v1::EngineAction> Handle(
      const KeyInput& event) const noexcept;

 private:
  EngineActionTransport* transport_;
  std::chrono::milliseconds deadline_;
};

}  // namespace zhixu::tsf
