#include "common/paths/runtime_paths.h"

#include <ShlObj.h>

namespace zhixu::paths {
tl::expected<RuntimePaths, HRESULT> RuntimePaths::ForCurrentUser() {
  PWSTR local_app_data = nullptr;
  const auto result = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DONT_VERIFY, nullptr, &local_app_data);
  if (FAILED(result)) {
    return tl::unexpected(result);
  }
  const std::filesystem::path root = std::filesystem::path(local_app_data) / L"ZhixuIME";
  CoTaskMemFree(local_app_data);
  return RuntimePaths{
      .root = root,
      .settings = root / L"settings.json",
      .user_data = root / L"user-data",
      .packages = root / L"packages",
      .diagnostics = root / L"diagnostics"};
}
}
