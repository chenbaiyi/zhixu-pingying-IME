#pragma once
#include <Windows.h>
#include <filesystem>
#include <tl/expected.hpp>
namespace zhixu::paths {
struct RuntimePaths {
  std::filesystem::path root, settings, user_data, packages, diagnostics;
  static tl::expected<RuntimePaths, HRESULT> ForCurrentUser();
};
}
