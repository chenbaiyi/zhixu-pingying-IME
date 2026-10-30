#pragma once

namespace zhixu::tsf {

void AddModuleReference() noexcept;
void ReleaseModuleReference() noexcept;
[[nodiscard]] bool HasModuleReferences() noexcept;

}  // namespace zhixu::tsf
