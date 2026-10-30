#include "tsf/input_scope_policy.h"

#include <algorithm>

namespace zhixu::tsf {

PrivacyDecision EvaluateInputScope(
    const InputScopeContext& context,
    const std::initializer_list<std::string_view> excluded_executable_hashes) {
  const bool sensitive_scope = context.input_scope == InputScope::Password ||
                               context.input_scope == InputScope::Pin;
  const bool excluded = std::ranges::find(excluded_executable_hashes,
                                          context.executable_hash) !=
                        excluded_executable_hashes.end();
  if (sensitive_scope || context.secure_desktop || context.privacy_mode || excluded) {
    return {.allow_composition = true, .allow_learning = false, .allow_prediction = false};
  }
  return {};
}

}  // namespace zhixu::tsf

zhixu::tsf::PrivacyDecision zhixu::tsf::ApplyApplicationSettings(PrivacyDecision d, const settings::Settings& s, std::string_view hash) {
  const bool excluded = !s.excluded_app_hashes.empty() && (hash.empty() || std::ranges::find(s.excluded_app_hashes,hash) != s.excluded_app_hashes.end());
  d.allow_learning = d.allow_learning && !s.privacy_mode && !excluded && s.learning_enabled;
  d.allow_prediction = d.allow_prediction && !s.privacy_mode && !excluded && s.prediction_enabled;
  return d;
}
