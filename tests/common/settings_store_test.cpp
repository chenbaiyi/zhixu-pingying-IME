#include <gtest/gtest.h>
#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "common/settings/settings_store.h"

namespace zhixu::settings {
namespace {

std::filesystem::path UniqueSettingsPath(const wchar_t* const name) {
  return std::filesystem::temp_directory_path() / L"ZhixuIME-settings-tests" /
         (std::wstring(name) + L"-" + std::to_wstring(GetCurrentProcessId()) + L".json");
}

TEST(SettingsStore, MissingFileUsesProductDefaults) {
  const auto path = UniqueSettingsPath(L"defaults");
  std::filesystem::remove(path);
  SettingsStore store(path);

  const auto loaded = store.Load();

  ASSERT_TRUE(loaded);
  EXPECT_FALSE(loaded->privacy_mode);
  EXPECT_TRUE(loaded->learning_enabled);
  EXPECT_TRUE(loaded->prediction_enabled);
  EXPECT_FALSE(loaded->tab_accept_enabled);
}

TEST(SettingsStore, MalformedFileFallsBackSafelyWithoutDeletingRecoveryEvidence) {
  const auto path = UniqueSettingsPath(L"malformed");
  std::filesystem::create_directories(path.parent_path());
  std::ofstream(path) << "{ definitely-not-json";
  SettingsStore store(path);

  const auto loaded = store.Load();

  ASSERT_TRUE(loaded);
  EXPECT_TRUE(loaded->recovered_from_invalid);
  EXPECT_TRUE(loaded->privacy_mode);
  EXPECT_FALSE(loaded->learning_enabled);
  EXPECT_FALSE(loaded->prediction_enabled);
  EXPECT_TRUE(std::filesystem::exists(path));
}

TEST(SettingsStore, UnknownFutureVersionFailsClosedForPrediction) {
  const auto path = UniqueSettingsPath(L"future-version");
  std::filesystem::create_directories(path.parent_path());
  std::ofstream(path) << R"({"version":2,"prediction_enabled":true})";
  SettingsStore store(path);

  const auto loaded = store.Load();

  ASSERT_TRUE(loaded);
  EXPECT_TRUE(loaded->recovered_from_invalid);
  EXPECT_FALSE(loaded->prediction_enabled);
}

TEST(SettingsStore, SaveAtomicallyReplacesAnExistingSettingsDocument) {
  const auto path = UniqueSettingsPath(L"save");
  std::filesystem::remove(path);
  SettingsStore store(path);
  auto first = Settings::Defaults();
  first.candidate_count = 3;
  ASSERT_TRUE(store.Save(first));

  auto replacement = Settings::Defaults();
  replacement.privacy_mode = true;
  replacement.learning_enabled = false;
  replacement.prediction_enabled = false;
  replacement.candidate_count = 9;
  ASSERT_TRUE(store.Save(replacement));

  const auto loaded = store.Load();
  ASSERT_TRUE(loaded);
  EXPECT_TRUE(loaded->privacy_mode);
  EXPECT_FALSE(loaded->learning_enabled);
  EXPECT_FALSE(loaded->prediction_enabled);
  EXPECT_EQ(loaded->candidate_count, 9U);
  EXPECT_FALSE(std::filesystem::exists(path.wstring() + L".tmp-" +
                                       std::to_wstring(GetCurrentProcessId())));
}

TEST(SettingsStore, SaveSignalsTheCurrentUsersSettingsChangeEvent) {
  const auto path = UniqueSettingsPath(L"settings-event");
  const std::wstring event_name = SettingsChangedEventName();
  const HANDLE event = CreateEventW(nullptr, TRUE, FALSE, event_name.c_str());
  ASSERT_NE(event, nullptr);
  ASSERT_EQ(WaitForSingleObject(event, 0), WAIT_TIMEOUT);
  SettingsStore store(path);

  ASSERT_TRUE(store.Save(Settings::Defaults()));

  EXPECT_EQ(WaitForSingleObject(event, 0), WAIT_OBJECT_0);
  CloseHandle(event);
}

}  // namespace
}  // namespace zhixu::settings
