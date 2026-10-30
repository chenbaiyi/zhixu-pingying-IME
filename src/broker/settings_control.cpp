#include "broker/settings_control.h"
#include "predict/imepack/package_store.h"
#include "predict/imepack/package_validator.h"
#include "predict/personalization/personalization_store.h"
#include <fstream>
#include <algorithm>
#include <tuple>
#include <cstdio>

#include <array>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace zhixu::broker {
namespace {

using Json = nlohmann::json;

template <typename Message>
bool ParsePayload(const protocol::Envelope& envelope, Message* message) {
  return message->ParseFromString(envelope.payload());
}

Json SerializeSettings(const settings::Settings& settings) {
  const char* theme = "system";
  if (settings.theme == settings::Theme::Light) {
    theme = "light";
  } else if (settings.theme == settings::Theme::Dark) {
    theme = "dark";
  }
  return Json{{"version", settings.version},
              {"privacy_mode", settings.privacy_mode},
              {"learning_enabled", settings.learning_enabled},
              {"prediction_enabled", settings.prediction_enabled},
              {"tab_accept_enabled", settings.tab_accept_enabled},
              {"candidate_count", settings.candidate_count},
              {"theme", theme},
              {"fuzzy_pinyin", settings.fuzzy_pinyin},
              {"excluded_app_hashes", settings.excluded_app_hashes}};
}

bool IsWhitelistedKey(const std::string_view key) {
  constexpr std::array keys = {"privacy_mode", "learning_enabled", "prediction_enabled",
                               "tab_accept_enabled", "candidate_count", "theme", "fuzzy_pinyin",
                               "excluded_app_hashes"};
  for (const auto allowed : keys) {
    if (key == allowed) {
      return true;
    }
  }
  return false;
}

bool ApplyPatch(const std::string_view arguments, settings::Settings* target) {
  try {
    const Json patch = Json::parse(arguments);
    if (!patch.is_object()) {
      return false;
    }
    for (const auto& [key, value] : patch.items()) {
      if (!IsWhitelistedKey(key)) {
        return false;
      }
      if (key == "privacy_mode") {
        if (!value.is_boolean()) return false;
        target->privacy_mode = value.get<bool>();
      } else if (key == "learning_enabled") {
        if (!value.is_boolean()) return false;
        target->learning_enabled = value.get<bool>();
      } else if (key == "prediction_enabled") {
        if (!value.is_boolean()) return false;
        target->prediction_enabled = value.get<bool>();
      } else if (key == "tab_accept_enabled") {
        if (!value.is_boolean()) return false;
        target->tab_accept_enabled = value.get<bool>();
      } else if (key == "candidate_count") {
        if (!value.is_number_unsigned()) return false;
        const auto count = value.get<unsigned int>();
        if (count == 0 || count > 9) return false;
        target->candidate_count = count;
      } else if (key == "theme") {
        if (!value.is_string()) return false;
        const auto theme = value.get<std::string>();
        if (theme == "system") target->theme = settings::Theme::System;
        else if (theme == "light") target->theme = settings::Theme::Light;
        else if (theme == "dark") target->theme = settings::Theme::Dark;
        else return false;
      } else if (key == "fuzzy_pinyin") {
        if (!value.is_boolean()) return false;
        target->fuzzy_pinyin = value.get<bool>();
      } else if (key == "excluded_app_hashes") {
        if (!value.is_array()) return false;
        for (const auto& hash : value) {
          if (!hash.is_string()) return false;
        }
        target->excluded_app_hashes = value.get<std::vector<std::string>>();
      }
    }
    return true;
  } catch (const nlohmann::json::exception&) {
    return false;
  }
}

}  // namespace

SettingsControl::SettingsControl(settings::SettingsStore& store, std::filesystem::path data_root) : data_root_(std::move(data_root)), store_(store) {}

tl::expected<void, ipc::PipeError> SettingsControl::Serve(const std::wstring_view role,
                                                           const std::stop_token stop_token) {
  const auto endpoint = ipc::PipeEndpoint::ForCurrentUser(role);
  if (!endpoint) {
    return tl::unexpected(endpoint.error());
  }

  while (!stop_token.stop_requested()) {
    ipc::NamedPipeServer server;
    const auto served = server.Listen(
        *endpoint, [this](const protocol::Envelope& request) { return Handle(request); }, stop_token);
    if (!served) {
      return tl::unexpected(served.error());
    }
  }
  return {};
}

protocol::Envelope SettingsControl::Handle(const protocol::Envelope& request) {
  if (request.protocol_version() != protocol::kProtocolVersion) {
    return ErrorResponse(request);
  }

  if (request.message_type() == protocol::v1::HELLO) {
    protocol::Hello hello;
    if (!ParsePayload(request, &hello)) {
      return ErrorResponse(request);
    }
    handshake_.Reset();
    const auto acknowledgment = handshake_.Accept(hello);
    if (!acknowledgment) {
      return ErrorResponse(request);
    }
    protocol::Envelope response;
    response.set_protocol_version(protocol::kProtocolVersion);
    response.set_message_type(protocol::v1::HELLO_ACK);
    response.set_session_id(request.session_id());
    response.set_request_id(request.request_id());
    acknowledgment->SerializeToString(response.mutable_payload());
    return response;
  }

  if (!handshake_.Validate(request.message_type()) ||
      request.message_type() != protocol::v1::CONTROL_COMMAND) {
    return ErrorResponse(request);
  }

  protocol::v1::ControlCommand command;
  if (!ParsePayload(request, &command)) {
    return ErrorResponse(request);
  }

  switch (command.operation()) {
    case protocol::v1::GET_SETTINGS: {
      if (!command.arguments().empty()) {
        return ResultResponse(request, SettingsControlError::kMalformedArguments);
      }
      const auto settings = store_.Load();
      if (!settings) {
        return ResultResponse(request, SettingsControlError::kSettingsIo);
      }
      return ResultResponse(request, SettingsControlError::kOk, SerializeSettings(*settings).dump(2));
    }
    case protocol::v1::PATCH_SETTINGS: {
      const auto current = store_.Load();
      if (!current) {
        return ResultResponse(request, SettingsControlError::kSettingsIo);
      }
      auto patched = *current;
      if (!ApplyPatch(command.arguments(), &patched)) {
        return ResultResponse(request, SettingsControlError::kMalformedArguments);
      }
      if (!store_.Save(patched)) {
        return ResultResponse(request, SettingsControlError::kSettingsIo);
      }
      return ResultResponse(request, SettingsControlError::kOk, SerializeSettings(patched).dump(2));
    }
    case protocol::v1::CLEAR_LEARNING:
    case protocol::v1::FORGET_TEXT: {
      if(data_root_.empty())return ResultResponse(request,SettingsControlError::kUnsupportedCommand);
      predict::PersonalizationStore learning(data_root_/L"user-data"/L"learning.db",true);
      const bool ok=command.operation()==protocol::v1::CLEAR_LEARNING ? (command.arguments().empty() && learning.Clear()) : learning.Forget(command.arguments());
      return ResultResponse(request,ok?SettingsControlError::kOk:SettingsControlError::kSettingsIo);
    }
    case protocol::v1::INSTALL_PACKAGE: {
      if(data_root_.empty())return ResultResponse(request,SettingsControlError::kUnsupportedCommand);
      const auto validated=predict::imepack::ValidatePackage(std::filesystem::u8path(command.arguments()));
      if(!validated)return ResultResponse(request,SettingsControlError::kPackageIo);
      predict::imepack::PackageStore packages(data_root_/L"packages");const auto installed=packages.InstallPackage(*validated);
      if(!installed && installed.error()!=predict::imepack::PackageError::kAlreadyInstalled)return ResultResponse(request,SettingsControlError::kPackageIo);
      if(!installed && !packages.ActivateVersion(validated->package_id,validated->version))return ResultResponse(request,SettingsControlError::kPackageIo);
      return ResultResponse(request,SettingsControlError::kOk,Json{{"id",validated->package_id},{"version",validated->version}}.dump());
    }
    case protocol::v1::ENABLE_PACKAGE:
    case protocol::v1::DISABLE_PACKAGE:
    case protocol::v1::REMOVE_PACKAGE: {
      if(data_root_.empty())return ResultResponse(request,SettingsControlError::kUnsupportedCommand);
      const auto id=command.arguments();
      if(id.empty() || id=="." || id==".." || id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-")!=std::string::npos)
       return ResultResponse(request,SettingsControlError::kMalformedArguments);
      predict::imepack::PackageStore packages(data_root_/L"packages");
      if(command.operation()==protocol::v1::DISABLE_PACKAGE)return ResultResponse(request,packages.Disable(id)?SettingsControlError::kOk:SettingsControlError::kPackageIo);
      std::vector<std::string> versions;std::error_code error;const auto package_root=data_root_/L"packages"/id;
      if(std::filesystem::is_symlink(package_root,error))return ResultResponse(request,SettingsControlError::kPackageIo);
      for(std::filesystem::directory_iterator it(package_root,error),end; !error && it!=end;it.increment(error))
       if(it->is_directory() && !it->is_symlink()) versions.push_back(it->path().filename().string());
      if(error || versions.empty())return ResultResponse(request,SettingsControlError::kPackageIo);
      auto version_key=[](const std::string& v){unsigned a=0,b=0,c=0;sscanf_s(v.c_str(),"%u.%u.%u",&a,&b,&c);return std::tuple{a,b,c,v};};
      std::sort(versions.begin(),versions.end(),[&](const auto& a,const auto& b){return version_key(a)<version_key(b);});
      if(command.operation()==protocol::v1::ENABLE_PACKAGE)return ResultResponse(request,packages.ActivateVersion(id,versions.back())?SettingsControlError::kOk:SettingsControlError::kPackageIo);
      if(!packages.Disable(id))return ResultResponse(request,SettingsControlError::kPackageIo);
      for(const auto& version:versions)if(!packages.Uninstall(id,version))return ResultResponse(request,SettingsControlError::kPackageIo);
      return ResultResponse(request,SettingsControlError::kOk);
    }
    case protocol::v1::GET_RUNTIME_STATUS: {
      Json list=Json::array();std::error_code error;const auto root=data_root_/L"packages";
      if(!data_root_.empty() && std::filesystem::exists(root,error))for(std::filesystem::directory_iterator it(root,error),end;!error && it!=end;it.increment(error)){
       if(!it->is_directory() || it->is_symlink() || it->path().filename()==L".staging")continue;
       Json active;std::ifstream active_file(it->path()/L"active.json");if(active_file)active=Json::parse(active_file,nullptr,false);
       for(std::filesystem::directory_iterator v(it->path(),error),vend;!error && v!=vend;v.increment(error)){
        if(!v->is_directory() || v->is_symlink())continue;
        std::ifstream input(v->path()/L"manifest.json");if(!input)continue;const auto manifest=Json::parse(input,nullptr,false);if(manifest.is_discarded())continue;
        list.push_back(Json{{"id",manifest.value("id",std::string{})},{"name",manifest.value("name",std::string{})},{"version",manifest.value("version",std::string{})},{"enabled",active.is_object() && active.value("version",std::string{})==manifest.value("version",std::string{})}});
       }
      }
      return ResultResponse(request,error?SettingsControlError::kPackageIo:SettingsControlError::kOk,Json{{"packages",list}}.dump());
    }
    default:
      return ResultResponse(request, SettingsControlError::kUnsupportedCommand);
  }
}

protocol::Envelope SettingsControl::ErrorResponse(const protocol::Envelope& request) const {
  protocol::Envelope response;
  response.set_protocol_version(protocol::kProtocolVersion);
  response.set_message_type(protocol::v1::MESSAGE_TYPE_UNSPECIFIED);
  response.set_session_id(request.session_id());
  response.set_request_id(request.request_id());
  return response;
}

protocol::Envelope SettingsControl::ResultResponse(const protocol::Envelope& request,
                                                   const SettingsControlError error,
                                                   const std::string_view data) const {
  protocol::v1::ControlResult result;
  result.set_error_code(static_cast<std::uint32_t>(error));
  result.set_data(data.data(), data.size());

  protocol::Envelope response;
  response.set_protocol_version(protocol::kProtocolVersion);
  response.set_message_type(protocol::v1::CONTROL_RESULT);
  response.set_session_id(request.session_id());
  response.set_request_id(request.request_id());
  result.SerializeToString(response.mutable_payload());
  return response;
}

}  // namespace zhixu::broker
