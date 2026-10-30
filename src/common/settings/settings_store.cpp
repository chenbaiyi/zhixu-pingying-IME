#include "common/settings/settings_store.h"

#include <Windows.h>
#include <Sddl.h>

#include <cstddef>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace zhixu::settings {
namespace {

using Json = nlohmann::json;

void SignalSettingsChanged() {
  const std::wstring event_name = SettingsChangedEventName();
  const HANDLE event = CreateEventW(nullptr, TRUE, FALSE, event_name.c_str());
  if (event != nullptr) {
    SetEvent(event);
    CloseHandle(event);
  }
}

bool ReadBool(const Json& object, const char* const key, const bool fallback) {
  const auto found = object.find(key);
  if (found == object.end()) {
    return fallback;
  }
  return found->get<bool>();
}

unsigned int ReadCandidateCount(const Json& object) {
  const auto found = object.find("candidate_count");
  if (found == object.end()) {
    return Settings::Defaults().candidate_count;
  }
  const auto count = found->get<unsigned int>();
  if (count == 0 || count > 9) {
    throw std::invalid_argument("candidate_count must be between 1 and 9");
  }
  return count;
}

Theme ReadTheme(const Json& object) {
  const auto found = object.find("theme");
  if (found == object.end()) {
    return Theme::System;
  }
  const auto value = found->get<std::string>();
  if (value == "system") return Theme::System;
  if (value == "light") return Theme::Light;
  if (value == "dark") return Theme::Dark;
  throw std::invalid_argument("theme must be system, light, or dark");
}

const char* ThemeName(const Theme theme) noexcept {
  switch (theme) {
    case Theme::Light:
      return "light";
    case Theme::Dark:
      return "dark";
    case Theme::System:
      return "system";
  }
  return "system";
}

Settings Parse(const Json& object) {
  if (!object.is_object() || object.value("version", 0U) != 1U) {
    throw std::invalid_argument("unsupported settings version");
  }

  Settings settings = Settings::Defaults();
  settings.privacy_mode = ReadBool(object, "privacy_mode", settings.privacy_mode);
  settings.learning_enabled = ReadBool(object, "learning_enabled", settings.learning_enabled);
  settings.prediction_enabled = ReadBool(object, "prediction_enabled", settings.prediction_enabled);
  settings.tab_accept_enabled = ReadBool(object, "tab_accept_enabled", settings.tab_accept_enabled);
  settings.candidate_count = ReadCandidateCount(object);
  settings.theme = ReadTheme(object);
  settings.fuzzy_pinyin = ReadBool(object, "fuzzy_pinyin", settings.fuzzy_pinyin);
  if (const auto hashes = object.find("excluded_app_hashes"); hashes != object.end()) {
    settings.excluded_app_hashes = hashes->get<std::vector<std::string>>();
  }
  return settings;
}

Json Serialize(const Settings& settings) {
  return Json{{"version", 1},
              {"privacy_mode", settings.privacy_mode},
              {"learning_enabled", settings.learning_enabled},
              {"prediction_enabled", settings.prediction_enabled},
              {"tab_accept_enabled", settings.tab_accept_enabled},
              {"candidate_count", settings.candidate_count},
              {"theme", ThemeName(settings.theme)},
              {"fuzzy_pinyin", settings.fuzzy_pinyin},
              {"excluded_app_hashes", settings.excluded_app_hashes}};
}

bool WriteAllAndFlush(const std::filesystem::path& path, const std::string& content) {
  const HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return false;
  }

  DWORD written = 0;
  const bool succeeded = content.size() <= MAXDWORD &&
                         WriteFile(handle, content.data(), static_cast<DWORD>(content.size()), &written,
                                   nullptr) &&
                         written == content.size() && FlushFileBuffers(handle);
  CloseHandle(handle);
  return succeeded;
}

}  // namespace

std::wstring SettingsChangedEventName() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
    return L"Local\\ZhixuIME.SettingsChanged";
  }

  DWORD byte_count = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &byte_count);
  std::vector<std::byte> buffer(byte_count);
  const bool got_user = byte_count != 0 &&
                        GetTokenInformation(token, TokenUser, buffer.data(), byte_count, &byte_count);
  CloseHandle(token);
  if (!got_user) {
    return L"Local\\ZhixuIME.SettingsChanged";
  }

  const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
  LPWSTR sid_text = nullptr;
  if (!ConvertSidToStringSidW(user->User.Sid, &sid_text)) {
    return L"Local\\ZhixuIME.SettingsChanged";
  }

  const std::wstring event_name = L"Local\\ZhixuIME.SettingsChanged." + std::wstring(sid_text);
  LocalFree(sid_text);
  return event_name;
}

Settings Settings::Defaults() noexcept { return {}; }

Settings Settings::SafeFallback() noexcept {
  Settings settings;
  settings.privacy_mode = true;
  settings.learning_enabled = false;
  settings.prediction_enabled = false;
  settings.recovered_from_invalid = true;
  return settings;
}

SettingsStore::SettingsStore(std::filesystem::path path) : path_(std::move(path)) {}

tl::expected<Settings, SettingsError> SettingsStore::Load() const {
  std::error_code error;
  if (!std::filesystem::exists(path_, error)) {
    if (error) {
      return tl::unexpected(SettingsError::Io);
    }
    return Settings::Defaults();
  }

  std::ifstream file(path_, std::ios::binary);
  if (!file) {
    return tl::unexpected(SettingsError::Io);
  }
  try {
    return Parse(Json::parse(file));
  } catch (const std::exception&) {
    return Settings::SafeFallback();
  }
}

tl::expected<void, SettingsError> SettingsStore::Save(const Settings& settings) const {
  std::error_code error;
  std::filesystem::create_directories(path_.parent_path(), error);
  if (error) {
    return tl::unexpected(SettingsError::Io);
  }

  const auto temporary = path_.wstring() + L".tmp-" + std::to_wstring(GetCurrentProcessId());
  const std::filesystem::path temporary_path(temporary);
  std::filesystem::remove(temporary_path, error);
  error.clear();
  if (!WriteAllAndFlush(temporary_path, Serialize(settings).dump(2))) {
    return tl::unexpected(SettingsError::Io);
  }

  const bool target_exists = std::filesystem::exists(path_, error);
  if (error || (target_exists && !ReplaceFileW(path_.c_str(), temporary_path.c_str(), nullptr,
                                                 REPLACEFILE_WRITE_THROUGH, nullptr, nullptr)) ||
      (!target_exists && !MoveFileExW(temporary_path.c_str(), path_.c_str(), MOVEFILE_WRITE_THROUGH))) {
    DeleteFileW(temporary_path.c_str());
    return tl::unexpected(SettingsError::Io);
  }
  SignalSettingsChanged();
  return {};
}

}  // namespace zhixu::settings
