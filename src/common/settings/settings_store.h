#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <tl/expected.hpp>

namespace zhixu::settings {

enum class Theme { System, Light, Dark };

struct Settings {
  unsigned int version = 1;
  bool privacy_mode = false;
  bool learning_enabled = true;
  bool prediction_enabled = true;
  bool tab_accept_enabled = false;
  unsigned int candidate_count = 8;
  Theme theme = Theme::System;
  bool fuzzy_pinyin = false;
  std::vector<std::string> excluded_app_hashes;
  bool recovered_from_invalid = false;

  [[nodiscard]] static Settings Defaults() noexcept;
  [[nodiscard]] static Settings SafeFallback() noexcept;
};

enum class SettingsError { Io };

[[nodiscard]] std::wstring SettingsChangedEventName();

class SettingsStore {
 public:
  explicit SettingsStore(std::filesystem::path path);

  [[nodiscard]] tl::expected<Settings, SettingsError> Load() const;
  [[nodiscard]] tl::expected<void, SettingsError> Save(const Settings& settings) const;

 private:
  std::filesystem::path path_;
};

}  // namespace zhixu::settings
