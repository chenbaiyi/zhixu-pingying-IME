#pragma once

#include <Windows.h>

namespace zhixu::protocol {
// Only a wake-up signal. Text and commit identity are fetched over the pipe.
inline constexpr UINT kRecommendationSelectedMessage = WM_APP + 0x371;
}  // namespace zhixu::protocol
