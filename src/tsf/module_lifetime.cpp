#include "tsf/module_lifetime.h"

#include <atomic>

namespace zhixu::tsf {
namespace {

std::atomic_ulong module_references{0};

}  // namespace

void AddModuleReference() noexcept {
  ++module_references;
}

void ReleaseModuleReference() noexcept {
  --module_references;
}

bool HasModuleReferences() noexcept {
  return module_references.load() != 0;
}

}  // namespace zhixu::tsf
