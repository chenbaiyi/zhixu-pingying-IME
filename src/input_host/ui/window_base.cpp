#include "input_host/ui/window_base.h"

namespace zhixu::input_host::ui {
namespace {

constexpr wchar_t kWindowClassName[] = L"ZhixuIme.NonActivatingWindow";

bool SystemDarkMode() noexcept {
  DWORD light = 1;
  DWORD bytes = sizeof(light);
  return RegGetValueW(HKEY_CURRENT_USER,
      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
      L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &bytes) == ERROR_SUCCESS && light == 0;
}

ATOM RegisterWindowClass(const HINSTANCE instance) noexcept {
  static const ATOM atom = [] (const HINSTANCE module) noexcept {
    WNDCLASSEXW window_class{.cbSize = sizeof(WNDCLASSEXW),
                             .lpfnWndProc = WindowBase::WindowProcedure,
                             .hInstance = module,
                             .hCursor = LoadCursorW(nullptr, IDC_ARROW),
                             .lpszClassName = kWindowClassName};
    return RegisterClassExW(&window_class);
  }(instance);
  return atom;
}

}  // namespace

WindowBase::~WindowBase() {
  if (hwnd_ != nullptr) {
    DestroyWindow(hwnd_);
  }
  ReleaseRenderTarget();
  if (dwrite_factory_ != nullptr) {
    dwrite_factory_->Release();
  }
  if (d2d_factory_ != nullptr) {
    d2d_factory_->Release();
  }
}

bool WindowBase::Create(const HINSTANCE instance, const Size initial_size) {
  if (hwnd_ != nullptr || instance == nullptr || RegisterWindowClass(instance) == 0 ||
      FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d_factory_)) ||
      FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                reinterpret_cast<IUnknown**>(&dwrite_factory_)))) {
    return false;
  }
  dark_mode_ = SystemDarkMode();
  // TSF caret coordinates are physical pixels. Prevent HWND virtualization.
  SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  hwnd_ = CreateWindowExW(ExtendedWindowStyle(), kWindowClassName, L"", WindowStyle(), 0, 0,
                          initial_size.width, initial_size.height, nullptr, nullptr, instance,
                          this);
  return hwnd_ != nullptr;
}

void WindowBase::ShowAt(const Rect bounds) noexcept {
  if (hwnd_ == nullptr) {
    return;
  }
  SetWindowPos(hwnd_, HWND_TOPMOST, bounds.left, bounds.top, bounds.right - bounds.left,
               bounds.bottom - bounds.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void WindowBase::Hide() noexcept {
  if (hwnd_ != nullptr) {
    ShowWindow(hwnd_, SW_HIDE);
  }
}

void WindowBase::SetDarkMode(const bool enabled) noexcept {
  dark_mode_ = enabled;
  if (hwnd_ != nullptr) InvalidateRect(hwnd_, nullptr, FALSE);
}

bool WindowBase::visible() const noexcept {
  return hwnd_ != nullptr && IsWindowVisible(hwnd_) != FALSE;
}

HWND WindowBase::hwnd() const noexcept { return hwnd_; }

LRESULT WindowBase::HandleWindowMessage(const UINT message, const WPARAM w_param,
                                        const LPARAM l_param) noexcept {
  return DefWindowProcW(hwnd_, message, w_param, l_param);
}

LRESULT CALLBACK WindowBase::WindowProcedure(const HWND hwnd, const UINT message,
                                              const WPARAM w_param, const LPARAM l_param) noexcept {
  auto* window = reinterpret_cast<WindowBase*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    window = static_cast<WindowBase*>(reinterpret_cast<CREATESTRUCTW*>(l_param)->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
    window->hwnd_ = hwnd;
    return TRUE;
  }
  if (window == nullptr) {
    return DefWindowProcW(hwnd, message, w_param, l_param);
  }
  if (message == WM_MOUSEACTIVATE) {
    return MA_NOACTIVATE;
  }
  if (message == WM_PRINTCLIENT) {
    window->PrintClient(reinterpret_cast<HDC>(w_param));
    return 0;
  }
  if (message == WM_PAINT) {
    window->Paint();
    return 0;
  }
  if (message == WM_SETTINGCHANGE || message == WM_THEMECHANGED) {
    window->ApplyTheme(window->theme_);
  }
  if (message == WM_DPICHANGED) {
    const auto* suggested = reinterpret_cast<const RECT*>(l_param);
    SetWindowPos(hwnd, HWND_TOPMOST, suggested->left, suggested->top,
                 suggested->right - suggested->left, suggested->bottom - suggested->top,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    return window->HandleWindowMessage(message, w_param, l_param);
  }
  if (message == WM_SIZE) {
    window->ReleaseRenderTarget();
  }
  return window->HandleWindowMessage(message, w_param, l_param);
}

void WindowBase::Paint() noexcept {
  PAINTSTRUCT paint{};
  BeginPaint(hwnd_, &paint);
  if (EnsureRenderTarget()) {
    render_target_->BeginDraw();
    Render(render_target_, dwrite_factory_);
    if (render_target_->EndDraw() == D2DERR_RECREATE_TARGET) {
      ReleaseRenderTarget();
    }
  }
  EndPaint(hwnd_, &paint);
}

void WindowBase::PrintClient(HDC destination) noexcept {
  if (!destination || !d2d_factory_) return;
  RECT client{};
  GetClientRect(hwnd_, &client);
  ID2D1DCRenderTarget* target = nullptr;
  const auto properties = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
  if (FAILED(d2d_factory_->CreateDCRenderTarget(&properties, &target))) return;
  if (SUCCEEDED(target->BindDC(destination, &client))) {
    target->BeginDraw();
    Render(target, dwrite_factory_);
    target->EndDraw();
  }
  target->Release();
}

bool WindowBase::EnsureRenderTarget() noexcept {
  if (render_target_ != nullptr) {
    return true;
  }
  RECT client{};
  GetClientRect(hwnd_, &client);
  return SUCCEEDED(d2d_factory_->CreateHwndRenderTarget(
      D2D1::RenderTargetProperties(),
      D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(client.right - client.left,
                                                           client.bottom - client.top)),
      &render_target_));
}

void WindowBase::ReleaseRenderTarget() noexcept {
  if (render_target_ != nullptr) {
    render_target_->Release();
    render_target_ = nullptr;
  }
}

}  // namespace zhixu::input_host::ui

void zhixu::input_host::ui::WindowBase::ApplyTheme(int theme) noexcept {
  theme_ = theme;
  SetDarkMode(theme == 2 || (theme == 0 && SystemDarkMode()));
}
