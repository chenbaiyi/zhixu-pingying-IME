#include "input_host/ui/candidate_window.h"
#include <algorithm>
#include <cmath>
#include <utility>
#include <commctrl.h>
#include <windowsx.h>
#include <wrl/client.h>

namespace zhixu::input_host::ui {
namespace {
using Microsoft::WRL::ComPtr;
ComPtr<IDWriteTextFormat> Format(IDWriteFactory* factory, float size) {
  ComPtr<IDWriteTextFormat> format;
  factory->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"zh-CN", &format);
  if (format) {
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
  }
  return format;
}
float TextWidth(IDWriteFactory* factory, IDWriteTextFormat* format, const std::wstring& text) {
  ComPtr<IDWriteTextLayout> layout;
  if (!format || FAILED(factory->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()),
      format, 100000.0F, 40.0F, &layout))) return 0;
  DWRITE_TEXT_METRICS metrics{};
  layout->GetMetrics(&metrics);
  return metrics.widthIncludingTrailingWhitespace;
}
std::wstring DisplayText(const CandidateLine& item) {
  auto result = item.text;
  if (!item.source.empty()) result += L" · " + item.source;
  if (!item.author.empty()) result += L" · " + item.author;
  return result;
}
bool Contains(const D2D1_RECT_F& r, float x, float y) {
  return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}
}
CandidateWindow::~CandidateWindow() {
  if (tooltip_) DestroyWindow(tooltip_);
}
void CandidateWindow::ClearTooltips() noexcept {
  if (!tooltip_) return;
  SendMessageW(tooltip_, TTM_POP, 0, 0);
  for (std::size_t i = 0; i < candidates_.size(); ++i) {
    TTTOOLINFOW info{.cbSize = sizeof(info)};
    info.hwnd = hwnd(); info.uId = i + 1;
    SendMessageW(tooltip_, TTM_DELTOOLW, 0, reinterpret_cast<LPARAM>(&info));
  }
}
void CandidateWindow::SetCandidates(std::vector<CandidateLine> candidates,
    std::function<void(unsigned int)> on_accept, std::wstring page_hint) {
  ClearTooltips();
  candidates_ = std::move(candidates); on_accept_ = std::move(on_accept);
  page_hint_ = std::move(page_hint);
  bounds_.clear(); pixel_bounds_.clear();
  if (hwnd()) InvalidateRect(hwnd(), nullptr, FALSE);
}
void CandidateWindow::SetPageState(unsigned int page, bool has_next, std::function<void(bool)> callback) {
  page_ = page; has_next_ = has_next; on_page_ = std::move(callback);
}
Size CandidateWindow::Measure(unsigned int dpi, int maximum_width, int maximum_height) {
  dpi_ = std::max(96U, dpi);
  const float scale = static_cast<float>(dpi_) / 96.0F;
  ComPtr<IDWriteFactory> factory;
  DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
      reinterpret_cast<IUnknown**>(factory.GetAddressOf()));
  auto text_format = factory ? Format(factory.Get(), 20) : ComPtr<IDWriteTextFormat>{};
  auto number_format = factory ? Format(factory.Get(), 16) : ComPtr<IDWriteTextFormat>{};
  std::vector<float> widths;
  number_widths_.clear();
  const float maximum = std::max(1.0F, maximum_width / scale);
  const bool expanded = page_ > 0;
  const float navigation = std::min(64.0F, maximum / 5.0F);
  float total = 8 + navigation;
  for (std::size_t i = 0; i < candidates_.size(); ++i) {
    const auto number = std::to_wstring(expanded ? i % 6 + 1 : i + 1);
    const float number_width = factory ? TextWidth(factory.Get(), number_format.Get(), number) : 10;
    number_widths_.push_back(number_width);
    const float width = 16 + number_width + (factory ? TextWidth(factory.Get(), text_format.Get(), DisplayText(candidates_[i])) : 20);
    widths.push_back(width); total += width + 4;
  }
  const float widest = widths.empty() ? 88 : *std::max_element(widths.begin(), widths.end());
  const float width = std::min(expanded ? 8 + 6 * std::clamp(widest, 88.0F, 140.0F) : total, maximum);
  const unsigned rows = std::max(1u, static_cast<unsigned>((widths.size() + 5) / 6));
  const float row_height = expanded && maximum_height > 0
      ? std::clamp((maximum_height / scale - 32) / rows, 1.0F, 40.0F) : 40;
  const float height = expanded ? rows * row_height + 32 : 40;
  float remaining = std::max(0.0F, width - 8 - navigation - 4 * static_cast<float>(widths.size()));
  // Cap the longest labels first, retaining all page-local indices on one row.
  float low = 0, high = total;
  for (int iteration = 0; iteration < 24; ++iteration) {
    const float cap = (low + high) / 2;
    float used = 0; for (float value : widths) used += std::min(value, cap);
    if (used > remaining) high = cap; else low = cap;
  }
  bounds_.clear(); pixel_bounds_.clear(); float x = 4;
  ClearTooltips();
  if (hwnd() && !tooltip_) {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES}; InitCommonControlsEx(&controls);
    tooltip_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE, TOOLTIPS_CLASSW, nullptr,
        WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, CW_USEDEFAULT, CW_USEDEFAULT,
        CW_USEDEFAULT, CW_USEDEFAULT, hwnd(), nullptr, GetModuleHandleW(nullptr), nullptr);
    if (tooltip_) SendMessageW(tooltip_, TTM_SETMAXTIPWIDTH, 0, 600);
  }
  for (std::size_t i = 0; i < widths.size(); ++i) {
    const float item_width = expanded ? (width - 8) / 6 : std::min(widths[i], low);
    const float left = expanded ? 4 + (i % 6) * item_width : x;
    const float top = expanded ? 4 + (i / 6) * row_height : 4;
    const auto r = D2D1::RectF(left, top, left + item_width - (expanded ? 4 : 0),
                              top + (expanded ? row_height - 4 : 32));
    bounds_.push_back(r);
    Rect pixels{static_cast<int>(std::round(r.left * scale)), static_cast<int>(std::round(r.top * scale)),
                static_cast<int>(std::round(r.right * scale)), static_cast<int>(std::round(r.bottom * scale))};
    pixel_bounds_.push_back(pixels);
    if (tooltip_) {
      TTTOOLINFOW info{.cbSize = sizeof(info)};
      info.uFlags = TTF_SUBCLASS; info.hwnd = hwnd(); info.uId = i + 1;
      info.rect = {pixels.left, pixels.top, pixels.right, pixels.bottom};
      info.lpszText = candidates_[i].text.data();
      SendMessageW(tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info));
    }
    x += item_width + 4;
  }
  previous_ = D2D1::RectF(width - navigation, expanded ? height - 28 : 4, width - navigation / 2, height - 4);
  next_ = D2D1::RectF(width - navigation / 2, expanded ? height - 28 : 4, width - 4, height - 4);
  return {static_cast<int>(std::ceil(width * scale)), static_cast<int>(std::ceil(height * scale))};
}
std::optional<unsigned int> CandidateWindow::HitTest(int x, int y) const noexcept {
  for (std::size_t i = 0; i < pixel_bounds_.size(); ++i) {
    const auto& r = pixel_bounds_[i];
    if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return static_cast<unsigned int>(i);
  }
  return std::nullopt;
}
bool CandidateWindow::AcceptShortcut(unsigned int number, bool alt_pressed) const {
  if (!alt_pressed || number == 0 || number > candidates_.size() || !on_accept_) return false;
  std::size_t index = number - 1;
  if (page_ > 0) {
    if (number > 6) return false;
    const auto selected = std::find_if(candidates_.begin(), candidates_.end(),
                                      [](const auto& item) { return item.selected; });
    if (selected != candidates_.end()) index += ((selected - candidates_.begin()) / 6) * 6;
  }
  if (index >= candidates_.size()) return false;
  on_accept_(candidates_[index].index); return true;
}
void CandidateWindow::Dismiss() noexcept { ClearTooltips(); Hide(); }
void CandidateWindow::Render(ID2D1RenderTarget* target, IDWriteFactory* factory) {
  target->SetDpi(static_cast<float>(dpi_), static_cast<float>(dpi_));
  target->Clear(dark_mode() ? D2D1::ColorF(0x202020) : D2D1::ColorF(0xffffff));
  ComPtr<ID2D1SolidColorBrush> green, foreground, muted, white, separator;
  target->CreateSolidColorBrush(D2D1::ColorF(0x00b578), &green);
  target->CreateSolidColorBrush(D2D1::ColorF(dark_mode() ? 0xf5f5f5 : 0x333333), &foreground);
  target->CreateSolidColorBrush(D2D1::ColorF(0x888888), &muted);
  target->CreateSolidColorBrush(D2D1::ColorF(0xffffff), &white);
  target->CreateSolidColorBrush(D2D1::ColorF(dark_mode() ? 0x383838 : 0xeeeeee), &separator);
  auto text = Format(factory, 20); auto number = Format(factory, 16);
  ComPtr<IDWriteInlineObject> ellipsis;
  if (text) {
    factory->CreateEllipsisTrimmingSign(text.Get(), &ellipsis);
    const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    text->SetTrimming(&trimming, ellipsis.Get());
  }
  if (!text || !number || !green || !foreground || !muted || !white) return;
  const auto highlighted = std::find_if(candidates_.begin(), candidates_.end(),
                                       [](const auto& item) { return item.selected; });
  const auto active_row = highlighted == candidates_.end() ? 0 : (highlighted - candidates_.begin()) / 6;
  for (std::size_t i = 0; i < bounds_.size(); ++i) {
    const auto& r = bounds_[i]; const auto& item = candidates_[i];
    if (item.selected) target->FillRoundedRectangle(D2D1::RoundedRect(r, 6, 6), green.Get());
    const bool numbered = page_ == 0 || i / 6 == static_cast<std::size_t>(active_row);
    const auto label = numbered ? std::to_wstring(page_ ? i % 6 + 1 : i + 1) : L"";
    auto number_rect = r; number_rect.left += 7; number_rect.right = std::min(r.right, number_rect.left + number_widths_[i]);
    target->DrawTextW(label.data(), static_cast<UINT32>(label.size()), number.Get(), number_rect,
        item.selected ? white.Get() : muted.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    auto text_rect = r; text_rect.left = numbered ? number_rect.right + 2 : r.left + 9; text_rect.right -= 6;
    const auto display_text = DisplayText(item);
    if (text_rect.right > text_rect.left) target->DrawTextW(display_text.data(), static_cast<UINT32>(display_text.size()),
        text.Get(), text_rect, item.selected ? white.Get() : foreground.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
  }
  if (page_ > 0) {
    if (separator) for (std::size_t i=6; i<bounds_.size(); i+=6)
      target->DrawLine(D2D1::Point2F(4, bounds_[i].top-2),
                       D2D1::Point2F(next_.right, bounds_[i].top-2), separator.Get());
    const auto hint = L"第 " + std::to_wstring(page_) + L" 组 · 第 " +
                      std::to_wstring(active_row + 1) + L" 行";
    const auto rect = D2D1::RectF(10, previous_.top, previous_.left - 4, previous_.bottom);
    target->DrawTextW(hint.data(), static_cast<UINT32>(hint.size()), number.Get(), rect, muted.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
  }
  const std::wstring left = L"‹", right = L"›";
  target->DrawTextW(left.data(), 1, text.Get(), previous_, page_ > 0 ? foreground.Get() : muted.Get());
  target->DrawTextW(right.data(), 1, text.Get(), next_, has_next_ ? foreground.Get() : muted.Get());
}
LRESULT CandidateWindow::HandleWindowMessage(UINT message, WPARAM w_param, LPARAM l_param) noexcept {
  if (message == WM_DPICHANGED) {
    RECT actual{}; GetWindowRect(hwnd(), &actual);
    MONITORINFO monitor{sizeof(monitor)};
    if (GetMonitorInfoW(MonitorFromWindow(hwnd(), MONITOR_DEFAULTTONEAREST), &monitor)) {
      const auto& work = monitor.rcWork;
      const auto size = Measure(HIWORD(w_param),
          std::min(actual.right-actual.left, work.right-work.left), work.bottom-work.top);
      const auto left = std::clamp(actual.left, work.left, std::max(work.left, work.right-size.width));
      const auto top = std::clamp(actual.top, work.top, std::max(work.top, work.bottom-size.height));
      SetWindowPos(hwnd(), nullptr, left, top, size.width, size.height, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    return 0;
  }
  if (message == WM_LBUTTONUP) {
    const int x = GET_X_LPARAM(l_param), y = GET_Y_LPARAM(l_param);
    if (const auto position = HitTest(x, y); position && on_accept_) {
      auto callback = on_accept_; on_accept_ = {}; on_page_ = {};
      callback(candidates_[*position].index); return 0;
    }
    const float scale = static_cast<float>(dpi_) / 96;
    if (on_page_ && ((page_ > 0 && Contains(previous_, x / scale, y / scale)) ||
                    (has_next_ && Contains(next_, x / scale, y / scale)))) {
      const bool forward = Contains(next_, x / scale, y / scale);
      auto callback = on_page_; on_page_ = {}; on_accept_ = {}; callback(forward); return 0;
    }
  }
  if (message == WM_KEYDOWN && w_param == VK_ESCAPE) { Dismiss(); return 0; }
  return DefWindowProcW(hwnd(), message, w_param, l_param);
}
}  // namespace zhixu::input_host::ui
