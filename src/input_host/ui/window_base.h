#pragma once

#include <Windows.h>
#include <d2d1.h>
#include <dwrite.h>

#include "input_host/ui/window_placement.h"

namespace zhixu::input_host::ui {

class WindowBase {
 public:
  WindowBase() = default;
  WindowBase(const WindowBase&) = delete;
  WindowBase& operator=(const WindowBase&) = delete;
  virtual ~WindowBase();

  [[nodiscard]] bool Create(HINSTANCE instance, Size initial_size);
  void ShowAt(Rect bounds) noexcept;
  void Hide() noexcept;
  void SetDarkMode(bool enabled) noexcept;
  void ApplyTheme(int theme) noexcept; // 0 system, 1 light, 2 dark
  [[nodiscard]] bool dark_mode() const noexcept { return dark_mode_; }
  [[nodiscard]] bool visible() const noexcept;
  [[nodiscard]] HWND hwnd() const noexcept;
  [[nodiscard]] static constexpr DWORD WindowStyle() noexcept { return WS_POPUP; }
  [[nodiscard]] static constexpr DWORD ExtendedWindowStyle() noexcept {
    return WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST;
  }
  static LRESULT CALLBACK WindowProcedure(HWND hwnd, UINT message, WPARAM w_param,
                                          LPARAM l_param) noexcept;

 protected:
  virtual void Render(ID2D1RenderTarget* target, IDWriteFactory* text_factory) = 0;
  virtual LRESULT HandleWindowMessage(UINT message, WPARAM w_param, LPARAM l_param) noexcept;

 private:
  void Paint() noexcept;
  void PrintClient(HDC destination) noexcept;
  [[nodiscard]] bool EnsureRenderTarget() noexcept;
  void ReleaseRenderTarget() noexcept;

  bool dark_mode_ = false;
  int theme_ = 0;
  HWND hwnd_ = nullptr;
  ID2D1Factory* d2d_factory_ = nullptr;
  IDWriteFactory* dwrite_factory_ = nullptr;
  ID2D1HwndRenderTarget* render_target_ = nullptr;
};

}  // namespace zhixu::input_host::ui
