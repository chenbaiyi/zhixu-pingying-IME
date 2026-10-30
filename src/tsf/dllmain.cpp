#include <Windows.h>
#include <msctf.h>

#include "tsf/class_factory.h"
#include "tsf/ids.h"
#include "tsf/module_lifetime.h"

#include <memory>
#include <string>

namespace zhixu::tsf {
namespace {

HMODULE module_handle = nullptr;
constexpr wchar_t kClassDescription[] = L"ZhixuIME Text Service";
constexpr wchar_t kProfileDescription[] = L"ZhixuIME";
constexpr wchar_t kClassesRoot[] = L"Software\\Classes\\CLSID\\";

[[nodiscard]] HRESULT FromLastError() noexcept {
  return HRESULT_FROM_WIN32(GetLastError());
}

[[nodiscard]] std::wstring GuidString(REFGUID guid) {
  wchar_t buffer[39]{};
  const int length = StringFromGUID2(guid, buffer, static_cast<int>(std::size(buffer)));
  return length == 0 ? std::wstring{} : std::wstring(buffer, length - 1);
}

[[nodiscard]] std::wstring PhysicalPath(const std::wstring& path) {
  const HANDLE file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return {};
  const std::unique_ptr<void, decltype(&CloseHandle)> handle(file, &CloseHandle);
  std::wstring result(MAX_PATH, L'\0');
  for (;;) {
    const DWORD length = GetFinalPathNameByHandleW(handle.get(), result.data(),
        static_cast<DWORD>(result.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (length == 0) return {};
    if (length < result.size()) {
      result.resize(length);
      if (result.rfind(L"\\\\?\\UNC\\", 0) == 0) return L"\\\\" + result.substr(8);
      if (result.rfind(L"\\\\?\\", 0) == 0) return result.substr(4);
      return result;
    }
    result.resize(static_cast<std::size_t>(length) + 1);
  }
}

[[nodiscard]] std::wstring ModulePath() {
  std::wstring result(MAX_PATH, L'\0');
  for (;;) {
    const DWORD copied = GetModuleFileNameW(module_handle, result.data(),
                                             static_cast<DWORD>(result.size()));
    if (copied == 0) {
      return {};
    }
    if (copied < result.size() - 1) {
      result.resize(copied);
      // Other hosts must see the same file even when this host redirects AppData.
      return PhysicalPath(result);
    }
    result.resize(result.size() * 2);
  }
}

HRESULT SetDefaultValue(HKEY root, const std::wstring& key_path, const wchar_t* value) {
  HKEY key = nullptr;
  const LSTATUS create_result = RegCreateKeyExW(root, key_path.c_str(), 0, nullptr, 0,
                                                 KEY_SET_VALUE, nullptr, &key, nullptr);
  if (create_result != ERROR_SUCCESS) {
    return HRESULT_FROM_WIN32(create_result);
  }
  const DWORD bytes = static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t));
  const LSTATUS write_result = RegSetValueExW(key, nullptr, 0, REG_SZ,
                                              reinterpret_cast<const BYTE*>(value), bytes);
  RegCloseKey(key);
  return write_result == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(write_result);
}

HRESULT RegisterComClass() {
  const std::wstring class_path = std::wstring(kClassesRoot) + GuidString(CLSID_ZhixuTextService);
  const HRESULT class_result = SetDefaultValue(HKEY_CURRENT_USER, class_path, kClassDescription);
  if (FAILED(class_result)) {
    return class_result;
  }
  const std::wstring module_path = ModulePath();
  if (module_path.empty()) {
    return FromLastError();
  }
  return SetDefaultValue(HKEY_CURRENT_USER, class_path + L"\\InprocServer32", module_path.c_str());
}

void UnregisterComClass() noexcept {
  const std::wstring class_path = std::wstring(kClassesRoot) + GuidString(CLSID_ZhixuTextService);
  RegDeleteTreeW(HKEY_CURRENT_USER, class_path.c_str());
}

class ScopedComInitialization final {
 public:
  ScopedComInitialization() : result_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
  ~ScopedComInitialization() {
    if (SUCCEEDED(result_)) {
      CoUninitialize();
    }
  }
  [[nodiscard]] bool available() const noexcept {
    return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE;
  }

 private:
  HRESULT result_;
};

HRESULT RegisterTsfProfileAndCategories() {
  ScopedComInitialization com;
  if (!com.available()) {
    return E_FAIL;
  }
  ITfInputProcessorProfileMgr* profiles = nullptr;
  HRESULT result = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_ITfInputProcessorProfileMgr,
                                    reinterpret_cast<void**>(&profiles));
  if (FAILED(result)) {
    return result;
  }
  const std::wstring module_path = ModulePath();
  result = profiles->RegisterProfile(
      CLSID_ZhixuTextService, MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED),
      GUID_ZhixuProfile, kProfileDescription,
      static_cast<ULONG>(std::size(kProfileDescription) - 1), module_path.c_str(),
      static_cast<ULONG>(module_path.size()), 0, nullptr, 0, TRUE, 0);
  profiles->Release();
  if (FAILED(result)) {
    return result;
  }

  ITfCategoryMgr* categories = nullptr;
  result = CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER,
                            IID_ITfCategoryMgr, reinterpret_cast<void**>(&categories));
  if (FAILED(result)) {
    return result;
  }
  result = categories->RegisterCategory(CLSID_ZhixuTextService, GUID_TFCAT_TIP_KEYBOARD,
                                        CLSID_ZhixuTextService);
  if (SUCCEEDED(result)) {
    result = categories->RegisterCategory(CLSID_ZhixuTextService,
                                          GUID_TFCAT_TIPCAP_IMMERSIVESUPPORT,
                                          CLSID_ZhixuTextService);
  }
  if (SUCCEEDED(result)) {
    // Modern text hosts can activate TSF without COM. The exported
    // DllGetClassObject factory supports this activation path.
    result = categories->RegisterCategory(CLSID_ZhixuTextService,
                                          GUID_TFCAT_TIPCAP_COMLESS,
                                          CLSID_ZhixuTextService);
  }
  categories->Release();
  return result;
}

void UnregisterTsfProfileAndCategories() noexcept {
  ScopedComInitialization com;
  if (!com.available()) {
    return;
  }
  ITfCategoryMgr* categories = nullptr;
  if (SUCCEEDED(CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER,
                                 IID_ITfCategoryMgr,
                                 reinterpret_cast<void**>(&categories)))) {
    categories->UnregisterCategory(CLSID_ZhixuTextService, GUID_TFCAT_TIPCAP_COMLESS,
                                   CLSID_ZhixuTextService);
    categories->UnregisterCategory(CLSID_ZhixuTextService, GUID_TFCAT_TIPCAP_IMMERSIVESUPPORT,
                                   CLSID_ZhixuTextService);
    categories->UnregisterCategory(CLSID_ZhixuTextService, GUID_TFCAT_TIP_KEYBOARD,
                                   CLSID_ZhixuTextService);
    categories->Release();
  }
  ITfInputProcessorProfileMgr* profiles = nullptr;
  if (SUCCEEDED(CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                                 IID_ITfInputProcessorProfileMgr,
                                 reinterpret_cast<void**>(&profiles)))) {
    profiles->UnregisterProfile(CLSID_ZhixuTextService,
                                MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED),
                                GUID_ZhixuProfile, 0);
    profiles->Release();
  }
}

}  // namespace
}  // namespace zhixu::tsf

extern "C" HRESULT STDAPICALLTYPE DllCanUnloadNow() {
  return zhixu::tsf::HasModuleReferences() ? S_FALSE : S_OK;
}

extern "C" HRESULT STDAPICALLTYPE DllGetClassObject(REFCLSID class_id, REFIID iid,
                                                      void** const object) {
  if (object == nullptr) {
    return E_POINTER;
  }
  *object = nullptr;
  if (!IsEqualCLSID(class_id, zhixu::tsf::CLSID_ZhixuTextService)) {
    return CLASS_E_CLASSNOTAVAILABLE;
  }
  return zhixu::tsf::CreateTsfClassFactory(iid, object);
}

extern "C" HRESULT STDAPICALLTYPE DllRegisterServer() {
  const HRESULT class_result = zhixu::tsf::RegisterComClass();
  if (FAILED(class_result)) {
    return class_result;
  }
  const HRESULT tsf_result = zhixu::tsf::RegisterTsfProfileAndCategories();
  if (FAILED(tsf_result)) {
    zhixu::tsf::UnregisterTsfProfileAndCategories();
    zhixu::tsf::UnregisterComClass();
  }
  return tsf_result;
}

extern "C" HRESULT STDAPICALLTYPE DllUnregisterServer() {
  zhixu::tsf::UnregisterTsfProfileAndCategories();
  zhixu::tsf::UnregisterComClass();
  return S_OK;
}

BOOL APIENTRY DllMain(HINSTANCE instance, const DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    zhixu::tsf::module_handle = instance;
  }
  return TRUE;
}
