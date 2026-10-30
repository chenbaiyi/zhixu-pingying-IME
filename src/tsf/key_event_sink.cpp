#include "tsf/key_event_sink.h"

namespace zhixu::tsf {

KeyEventSink::KeyEventSink(EngineActionTransport* const transport,
                           const std::chrono::milliseconds deadline) noexcept
    : transport_(transport), deadline_(deadline) {}

std::optional<protocol::v1::EngineAction> KeyEventSink::Handle(const KeyInput& event) const noexcept {
  if (transport_ == nullptr) {
    return std::nullopt;
  }
  try {
    return transport_->Dispatch(event, deadline_);
  } catch (...) {
    // A transport failure must never delay or consume an application key.
    return std::nullopt;
  }
}

}  // namespace zhixu::tsf
