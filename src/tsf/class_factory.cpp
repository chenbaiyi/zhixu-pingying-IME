#include "tsf/input_host_transport.h"
#include "tsf/class_factory.h"
#include "tsf/module_lifetime.h"
#include "tsf/tsf_text_service.h"

#include <atomic>
#include <memory>
#include <new>

namespace zhixu::tsf {
namespace {

class TsfClassFactory final : public IClassFactory {
 public:
  TsfClassFactory() { AddModuleReference(); }
  ~TsfClassFactory() { ReleaseModuleReference(); }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** const object) override {
    if (object == nullptr) {
      return E_POINTER;
    }
    *object = nullptr;
    if (iid != IID_IUnknown && iid != IID_IClassFactory) {
      return E_NOINTERFACE;
    }
    *object = static_cast<IClassFactory*>(this);
    AddRef();
    return S_OK;
  }

  ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }

  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG remaining = --references_;
    if (remaining == 0) {
      delete this;
    }
    return remaining;
  }

  HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* const outer, REFIID iid,
                                           void** const object) override {
    if (object == nullptr) {
      return E_POINTER;
    }
    *object = nullptr;
    if (outer != nullptr) {
      return CLASS_E_NOAGGREGATION;
    }
    auto* const service = new (std::nothrow) TsfTextService(std::make_unique<InputHostTransport>());
    if (service == nullptr) {
      return E_OUTOFMEMORY;
    }
    const HRESULT result = service->QueryInterface(iid, object);
    service->Release();
    return result;
  }

  HRESULT STDMETHODCALLTYPE LockServer(const BOOL lock) override {
    if (lock) {
      AddModuleReference();
    } else {
      ReleaseModuleReference();
    }
    return S_OK;
  }

 private:
  std::atomic<ULONG> references_{1};
};

}  // namespace

HRESULT CreateTsfClassFactory(REFIID iid, void** const object) noexcept {
  auto* const factory = new (std::nothrow) TsfClassFactory();
  if (factory == nullptr) {
    return E_OUTOFMEMORY;
  }
  const HRESULT result = factory->QueryInterface(iid, object);
  factory->Release();
  return result;
}

}  // namespace zhixu::tsf
