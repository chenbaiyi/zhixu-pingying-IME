#include <gtest/gtest.h>
#include <Windows.h>
#include <ShlObj.h>
#include "common/paths/runtime_paths.h"
TEST(RuntimePaths, UsesKnownFolderAndDoesNotCreateProductData) {
  PWSTR path = nullptr;
  ASSERT_EQ(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DONT_VERIFY, nullptr, &path), S_OK);
  std::filesystem::path expected = std::filesystem::path(path) / L"ZhixuIME"; CoTaskMemFree(path);
  const bool existed = std::filesystem::exists(expected);
  auto paths = zhixu::paths::RuntimePaths::ForCurrentUser(); ASSERT_TRUE(paths);
  EXPECT_EQ(paths->root, expected); EXPECT_EQ(paths->settings, expected / L"settings.json");
  EXPECT_EQ(paths->packages, expected / L"packages"); EXPECT_EQ(std::filesystem::exists(expected), existed);
}
