#pragma once

#include <cstdint>
#include <stop_token>
#include <string_view>

#include "common/ipc/named_pipe.h"
#include "common/protocol/envelope_codec.h"
#include "common/protocol/protocol_handshake.h"
#include "common/settings/settings_store.h"

namespace zhixu::broker {

enum class SettingsControlError : std::uint32_t {
  kOk = 0,
  kMalformedArguments = 1,
  kSettingsIo = 2,
  kUnsupportedCommand = 3,
  kPackageIo = 4,
};

class SettingsControl {
 public:
  explicit SettingsControl(settings::SettingsStore& store, std::filesystem::path data_root = {});

  [[nodiscard]] tl::expected<void, ipc::PipeError> Serve(std::wstring_view role,
                                                          std::stop_token stop_token);
  [[nodiscard]] protocol::Envelope Handle(const protocol::Envelope& request);

 private:
  [[nodiscard]] protocol::Envelope ErrorResponse(const protocol::Envelope& request) const;
  [[nodiscard]] protocol::Envelope ResultResponse(const protocol::Envelope& request,
                                                   SettingsControlError error,
                                                   std::string_view data = {}) const;

  std::filesystem::path data_root_;
  settings::SettingsStore& store_;
  protocol::ProtocolHandshake handshake_;
};

}  // namespace zhixu::broker
