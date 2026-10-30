#pragma once

#include <initializer_list>
#include <string_view>
#include "common/settings/settings_store.h"

namespace zhixu::tsf {

enum class InputScope {
  Ordinary,
  Password,
  Pin,
};

struct InputScopeContext {
  InputScope input_scope = InputScope::Ordinary;
  bool secure_desktop = false;
  bool privacy_mode = false;
  std::string_view executable_hash;
};

struct PrivacyDecision {
  bool allow_composition = true;
  bool allow_learning = true;
  bool allow_prediction = true;
};

[[nodiscard]] PrivacyDecision EvaluateInputScope(
    const InputScopeContext& context,
    std::initializer_list<std::string_view> excluded_executable_hashes = {});

PrivacyDecision ApplyApplicationSettings(PrivacyDecision decision, const settings::Settings& settings, std::string_view executable_hash);

}  // namespace zhixu::tsf
