#pragma once
#include <cstdint>

namespace zhixu::protocol {
// Stable wire bits, independent of Windows and Rime/X11 masks.
inline constexpr std::uint32_t kAlt = 1U;
inline constexpr std::uint32_t kControl = 2U;
inline constexpr std::uint32_t kShift = 4U;
inline constexpr std::uint32_t kCapsLock = 8U;
inline constexpr std::uint32_t kSuper = 16U;
inline constexpr std::uint32_t kHeldModifiers = kAlt | kControl | kShift | kSuper;
}  // namespace zhixu::protocol
