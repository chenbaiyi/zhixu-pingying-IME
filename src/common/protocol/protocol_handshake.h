#pragma once
#include "common/protocol/envelope_codec.h"
namespace zhixu::protocol {
class ProtocolHandshake {
 public:
  tl::expected<HelloAck, ProtocolError> Accept(const Hello&);
  tl::expected<void, ProtocolError> Validate(MessageType) const;
  void Reset() noexcept { accepted_ = false; }
 private:
  bool accepted_ = false;
};
}
