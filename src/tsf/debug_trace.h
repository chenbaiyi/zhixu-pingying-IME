#pragma once

#include <Windows.h>
#include <atomic>
#include <cstdio>
#include <string>

namespace zhixu::tsf {

// Temporary, opt-in probe. Never log keys, composition text, or application text.
inline void TraceTsfResult(const char* stage, const HRESULT result) noexcept {
  wchar_t enabled[2]{};
  if (GetEnvironmentVariableW(L"ZHIXU_TSF_PROBE", enabled, 2) != 1 || enabled[0] != L'1') return;
  static std::atomic<unsigned int> events{0};
  if (events.fetch_add(1) >= 256) return;
  try {
    wchar_t temporary_directory[MAX_PATH]{};
    const auto length = GetTempPathW(MAX_PATH, temporary_directory);
    if (length == 0 || length >= MAX_PATH) return;
    const auto path = std::wstring(temporary_directory) + L"ZhixuIME-tsf-probe.log";
    const HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                   OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    char line[160]{};
    const auto count = std::snprintf(line, sizeof(line),
        "[DEBUG-tsf-edit] pid=%lu stage=%s hr=%08lX\r\n",
        GetCurrentProcessId(), stage, static_cast<unsigned long>(result));
    if (count > 0 && count < static_cast<int>(sizeof(line))) {
      DWORD written = 0;
      WriteFile(file, line, static_cast<DWORD>(count), &written, nullptr);
    }
    CloseHandle(file);
  } catch (...) {
    // A diagnostic probe must not affect input handling.
  }
}

}  // namespace zhixu::tsf
