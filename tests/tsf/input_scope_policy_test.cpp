#include <gtest/gtest.h>

#include "tsf/input_scope_policy.h"

namespace zhixu::tsf {
namespace {

TEST(InputScopePolicy, PasswordAllowsCompositionButDisablesLearningAndPrediction) {
  const auto decision = EvaluateInputScope({.input_scope = InputScope::Password});

  EXPECT_TRUE(decision.allow_composition);
  EXPECT_FALSE(decision.allow_learning);
  EXPECT_FALSE(decision.allow_prediction);
}

TEST(InputScopePolicy, PinAndSecureDesktopDisableLearningAndPrediction) {
  const auto pin = EvaluateInputScope({.input_scope = InputScope::Pin});
  const auto secure = EvaluateInputScope({.secure_desktop = true});

  EXPECT_TRUE(pin.allow_composition);
  EXPECT_FALSE(pin.allow_learning);
  EXPECT_FALSE(pin.allow_prediction);
  EXPECT_TRUE(secure.allow_composition);
  EXPECT_FALSE(secure.allow_learning);
  EXPECT_FALSE(secure.allow_prediction);
}

TEST(InputScopePolicy, PrivacyModeAndExcludedApplicationDisableLearningAndPrediction) {
  const auto privacy = EvaluateInputScope({.privacy_mode = true});
  const auto excluded = EvaluateInputScope(
      {.executable_hash = "a0b1"}, {"a0b1"});

  EXPECT_TRUE(privacy.allow_composition);
  EXPECT_FALSE(privacy.allow_learning);
  EXPECT_FALSE(privacy.allow_prediction);
  EXPECT_TRUE(excluded.allow_composition);
  EXPECT_FALSE(excluded.allow_learning);
  EXPECT_FALSE(excluded.allow_prediction);
}

TEST(InputScopePolicy, OrdinaryFieldAllowsAllLocalInputFeatures) {
  const auto decision = EvaluateInputScope({});

  EXPECT_TRUE(decision.allow_composition);
  EXPECT_TRUE(decision.allow_learning);
  EXPECT_TRUE(decision.allow_prediction);
}

}  // namespace
}  // namespace zhixu::tsf

TEST(RuntimeSettings, ExcludedApplicationsAndPrivacyCannotLearnOrPredict) {
  auto s=zhixu::settings::Settings::Defaults(); s.excluded_app_hashes={"abc"};
  auto d=zhixu::tsf::ApplyApplicationSettings({},s,"abc"); EXPECT_TRUE(d.allow_composition);EXPECT_FALSE(d.allow_learning);EXPECT_FALSE(d.allow_prediction);
  d=zhixu::tsf::ApplyApplicationSettings({},s,""); EXPECT_FALSE(d.allow_learning);EXPECT_FALSE(d.allow_prediction);
  d=zhixu::tsf::ApplyApplicationSettings({},s,"def"); EXPECT_TRUE(d.allow_learning);EXPECT_TRUE(d.allow_prediction);
  s.privacy_mode=true;d=zhixu::tsf::ApplyApplicationSettings({},s,"def");EXPECT_FALSE(d.allow_learning);EXPECT_FALSE(d.allow_prediction);
  s.privacy_mode=false;s.learning_enabled=false;s.prediction_enabled=false;s.excluded_app_hashes.clear();
  d=zhixu::tsf::ApplyApplicationSettings({},s,"def");EXPECT_FALSE(d.allow_learning);EXPECT_FALSE(d.allow_prediction);
  s.learning_enabled=true;s.prediction_enabled=true;
  d=zhixu::tsf::ApplyApplicationSettings({true,false,false},s,"def");EXPECT_FALSE(d.allow_learning);EXPECT_FALSE(d.allow_prediction);
}

#include "common/settings/runtime_settings.h"
#include <fstream>
TEST(RuntimeSettings, ExecutableIdentityUsesSha256OfBytesAndRejectsMissingFiles) {
  auto path=std::filesystem::temp_directory_path()/("zhixu-identity-"+std::to_string(GetCurrentProcessId())+".exe");
  {std::ofstream out(path,std::ios::binary);out<<"abc";}
  EXPECT_EQ(zhixu::settings::ExecutableHash(path).value_or(""),"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  std::filesystem::remove(path); EXPECT_FALSE(zhixu::settings::ExecutableHash(path));
}
