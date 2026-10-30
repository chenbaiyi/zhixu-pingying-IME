#include <gtest/gtest.h>
#include <filesystem>

#include <Windows.h>
#include <msctf.h>

#include "tsf/ids.h"

namespace zhixu::tsf {
namespace {

using DllCanUnloadNowFn = HRESULT(STDAPICALLTYPE*)();
using DllGetClassObjectFn = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);

class TsfDll : public ::testing::Test {
 protected:
  void SetUp() override {
    module_ = LoadLibraryW(std::filesystem::u8path(ZHIXU_TSF_TEST_DLL).c_str());
    ASSERT_NE(module_, nullptr) << GetLastError();
    can_unload_ = reinterpret_cast<DllCanUnloadNowFn>(GetProcAddress(module_, "DllCanUnloadNow"));
    get_class_object_ =
        reinterpret_cast<DllGetClassObjectFn>(GetProcAddress(module_, "DllGetClassObject"));
    ASSERT_NE(can_unload_, nullptr);
    ASSERT_NE(get_class_object_, nullptr);
  }

  void TearDown() override {
    if (module_ != nullptr) {
      FreeLibrary(module_);
    }
  }

  HMODULE module_ = nullptr;
  DllCanUnloadNowFn can_unload_ = nullptr;
  DllGetClassObjectFn get_class_object_ = nullptr;
};

TEST_F(TsfDll, ExportsFactoryAndTracksOutstandingComObjects) {
  ASSERT_EQ(can_unload_(), S_OK);

  IClassFactory* factory = nullptr;
  ASSERT_EQ(get_class_object_(CLSID_ZhixuTextService, IID_IClassFactory,
                              reinterpret_cast<void**>(&factory)),
            S_OK);
  ASSERT_NE(factory, nullptr);
  EXPECT_EQ(can_unload_(), S_FALSE);

  IUnknown* service = nullptr;
  EXPECT_EQ(factory->CreateInstance(nullptr, IID_IUnknown,
                                    reinterpret_cast<void**>(&service)),
            S_OK);
  ASSERT_NE(service, nullptr);
  EXPECT_EQ(can_unload_(), S_FALSE);

  service->Release();
  factory->Release();
  EXPECT_EQ(can_unload_(), S_OK);
}

TEST_F(TsfDll, RejectsUnknownClassesAndAggregation) {
  const GUID unknown_class = {0x818caa2f, 0x7c05, 0x4f4f, {0xb1, 0xe2, 0xc6, 0xf1, 0xf8, 0xe0, 0xf5, 0x70}};
  void* object = nullptr;
  EXPECT_EQ(get_class_object_(unknown_class, IID_IClassFactory, &object), CLASS_E_CLASSNOTAVAILABLE);
  EXPECT_EQ(object, nullptr);

  IClassFactory* factory = nullptr;
  ASSERT_EQ(get_class_object_(CLSID_ZhixuTextService, IID_IClassFactory,
                              reinterpret_cast<void**>(&factory)),
            S_OK);
  EXPECT_EQ(factory->CreateInstance(factory, IID_IUnknown, &object), CLASS_E_NOAGGREGATION);
  EXPECT_EQ(object, nullptr);
  factory->Release();
}

}  // namespace
}  // namespace zhixu::tsf
