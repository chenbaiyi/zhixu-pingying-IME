#pragma once
#include "common/settings/settings_store.h"
#include "common/paths/runtime_paths.h"
#include <Windows.h>
#include <bcrypt.h>
#include <array>
#include <optional>
namespace zhixu::settings {
inline Settings LoadRuntimeSettings() {
  const auto paths=paths::RuntimePaths::ForCurrentUser();
  if (!paths) return Settings::SafeFallback();
  const auto loaded=SettingsStore(paths->settings).Load();
  return loaded ? *loaded : Settings::SafeFallback();
}
// The UI and TSF identify exactly the same file bytes; paths are never persisted.
inline std::optional<std::string> ExecutableHash(const std::filesystem::path& path) {
  HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
  if(file==INVALID_HANDLE_VALUE) return std::nullopt;
  BCRYPT_ALG_HANDLE algorithm=nullptr; BCRYPT_HASH_HANDLE hash=nullptr;
  std::array<unsigned char,32> digest{}; std::array<unsigned char,65536> buffer{};
  bool ok=BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0;
  if(ok) ok=BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0)>=0;
  while(ok){DWORD bytes=0;if(!ReadFile(file,buffer.data(),static_cast<DWORD>(buffer.size()),&bytes,nullptr)){ok=false;break;}if(!bytes)break;ok=BCryptHashData(hash,buffer.data(),bytes,0)>=0;}
  if(ok)ok=BCryptFinishHash(hash,digest.data(),static_cast<ULONG>(digest.size()),0)>=0;
  if(hash)BCryptDestroyHash(hash);if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);CloseHandle(file);
  if(!ok)return std::nullopt;
  std::string result;result.reserve(64);constexpr char hex[]="0123456789abcdef";
  for(auto b:digest){result+=hex[b>>4];result+=hex[b&15];}return result;
}
inline const std::string& CurrentApplicationHash() {
  static const std::string value=[] {std::wstring path(32768,L'\0');const auto n=GetModuleFileNameW(nullptr,path.data(),static_cast<DWORD>(path.size()));if(!n || n==path.size())return std::string{};path.resize(n);return ExecutableHash(path).value_or("");}();
  return value;
}
}
