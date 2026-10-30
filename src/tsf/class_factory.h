#pragma once

#include <Unknwn.h>

namespace zhixu::tsf {

HRESULT CreateTsfClassFactory(REFIID iid, void** object) noexcept;

}  // namespace zhixu::tsf
