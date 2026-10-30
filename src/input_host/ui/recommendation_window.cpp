#include "input_host/ui/recommendation_window.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <windowsx.h>
#include <wrl/client.h>

namespace zhixu::input_host::ui {
namespace {
using Microsoft::WRL::ComPtr;
constexpr int kPagingHeight = 28;
bool Contains(const D2D1_RECT_F& bounds, float x, float y) {
  return x >= bounds.left && x < bounds.right && y >= bounds.top && y < bounds.bottom;
}
ComPtr<IDWriteTextFormat> Format(IDWriteFactory* factory, float size) {
  ComPtr<IDWriteTextFormat> format;
  if (SUCCEEDED(factory->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"zh-CN", &format))) {
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    ComPtr<IDWriteInlineObject> ellipsis;
    if (SUCCEEDED(factory->CreateEllipsisTrimmingSign(format.Get(), &ellipsis))) {
      const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
      format->SetTrimming(&trimming, ellipsis.Get());
    }
  }
  return format;
}
}

void RecommendationWindow::SetRecommendations(std::vector<std::wstring> recommendations,
    std::function<void(unsigned int)> on_accept, std::vector<std::wstring> provenance) {
  recommendations_ = std::move(recommendations);
  if (recommendations_.size() > 6) recommendations_.resize(6);
  provenance_ = std::move(provenance);
  provenance_.resize(recommendations_.size());
  row_height_ = std::any_of(provenance_.begin(), provenance_.end(),
      [](const auto& value) { return !value.empty(); }) ? 48 : 26;
  selected_ = 0;
  page_ = 0;
  page_count_ = 1;
  on_page_ = {};
  on_accept_ = std::move(on_accept);
  if (hwnd()) InvalidateRect(hwnd(), nullptr, FALSE);
}

void RecommendationWindow::SetPaging(const unsigned int page, const unsigned int page_count,
    std::function<void(int)> on_page) {
  page_count_ = std::max(1U, page_count);
  page_ = std::min(page, page_count_ - 1);
  on_page_ = std::move(on_page);
  if (hwnd()) InvalidateRect(hwnd(), nullptr, FALSE);
}

D2D1_RECT_F RecommendationWindow::PageBounds(const bool forward, const float width) const noexcept {
  const float button_width = std::min(72.0F, std::max(0.0F, (width - 8) / 3));
  const float top = 4.0F + row_height_ * static_cast<float>(recommendations_.size());
  const float left = forward ? std::max(4.0F, width - 4 - button_width) : 4.0F;
  return D2D1::RectF(left, top, left + button_width, top + kPagingHeight);
}

void RecommendationWindow::Page(const int direction) {
  if (!on_page_ || page_count_ <= 1 ||
      (direction < 0 ? page_ == 0 : page_ + 1 >= page_count_)) return;
  auto callback = std::move(on_page_);
  on_page_ = {};
  // Keep the old page visible while the next presentation is queued, but revoke its actions.
  on_accept_ = {};
  callback(direction);
}

Size RecommendationWindow::Measure(const unsigned int dpi, const int maximum_width) {
  dpi_ = std::max(96U, dpi);
  const float scale = static_cast<float>(dpi_) / 96;
  const auto height = 8 + row_height_ * recommendations_.size() + (page_count_ > 1 ? kPagingHeight : 0);
  return {std::min(std::max(0, maximum_width), static_cast<int>(std::ceil(420 * scale))),
          static_cast<int>(std::ceil(height * scale))};
}

void RecommendationWindow::Accept(const unsigned int index) {
  if (!on_accept_ || index >= recommendations_.size()) return;
  auto callback = std::move(on_accept_);
  Dismiss();
  callback(index);
}

bool RecommendationWindow::AcceptShortcut(const unsigned int number, const bool alt_pressed) {
  if (!alt_pressed || number == 0 || number > recommendations_.size() || !on_accept_) return false;
  Accept(number - 1);
  return true;
}

void RecommendationWindow::Dismiss() noexcept { on_accept_ = {}; on_page_ = {}; Hide(); }

void RecommendationWindow::Render(ID2D1RenderTarget* target, IDWriteFactory* factory) {
  target->SetDpi(static_cast<float>(dpi_), static_cast<float>(dpi_));
  target->Clear(dark_mode() ? D2D1::ColorF(0x202020) : D2D1::ColorF(0xffffff));
  ComPtr<ID2D1SolidColorBrush> foreground, muted, green, white;
  target->CreateSolidColorBrush(D2D1::ColorF(dark_mode() ? 0xf5f5f5 : 0x333333), &foreground);
  target->CreateSolidColorBrush(D2D1::ColorF(dark_mode() ? 0xaaaaaa : 0x666666), &muted);
  target->CreateSolidColorBrush(D2D1::ColorF(0x00b578), &green);
  target->CreateSolidColorBrush(D2D1::ColorF(0xffffff), &white);
  auto text = Format(factory, 18), citation = Format(factory, 12);
  if (!foreground || !muted || !green || !white || !text || !citation) return;
  const float width = target->GetSize().width;
  for (std::size_t i = 0; i < recommendations_.size(); ++i) {
    const float top = 4.0F + row_height_ * static_cast<float>(i);
    const bool selected = static_cast<int>(i) == selected_;
    if (selected) target->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(4, top, std::max(4.0F, width - 4), top + row_height_), 6, 6), green.Get());
    const auto label = L"Alt+" + std::to_wstring(i + 1) + L"  " + recommendations_[i];
    target->DrawTextW(label.data(), static_cast<UINT32>(label.size()), text.Get(),
        D2D1::RectF(10, top, std::max(10.0F, width - 10), top + 26),
        selected ? white.Get() : foreground.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    if (!provenance_[i].empty()) target->DrawTextW(provenance_[i].data(),
        static_cast<UINT32>(provenance_[i].size()), citation.Get(),
        D2D1::RectF(10, top + 26, std::max(10.0F, width - 10), top + row_height_),
        selected ? white.Get() : muted.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
  }
  if (page_count_ > 1) {
    const auto previous = PageBounds(false, width), next = PageBounds(true, width);
    text->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    citation->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    target->DrawTextW(L"‹", 1, text.Get(), previous,
        page_ > 0 ? foreground.Get() : muted.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    target->DrawTextW(L"›", 1, text.Get(), next,
        page_ + 1 < page_count_ ? foreground.Get() : muted.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    const auto page_hint = std::to_wstring(page_ + 1) + L"/" + std::to_wstring(page_count_);
    target->DrawTextW(page_hint.data(), static_cast<UINT32>(page_hint.size()), citation.Get(),
        D2D1::RectF(previous.right, previous.top, next.left, next.bottom),
        muted.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
  }
}

LRESULT RecommendationWindow::HandleWindowMessage(UINT message, WPARAM w_param, LPARAM l_param) noexcept {
  if (message == WM_DPICHANGED) {
    RECT client{};
    GetClientRect(hwnd(), &client);
    const auto size = Measure(HIWORD(w_param), client.right);
    SetWindowPos(hwnd(), nullptr, 0, 0, size.width, size.height,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    return 0;
  }
  if (message == WM_LBUTTONUP || message == WM_MOUSEMOVE) {
    RECT client{};
    GetClientRect(hwnd(), &client);
    const int x = GET_X_LPARAM(l_param), y = GET_Y_LPARAM(l_param);
    const float dip_y = y * 96.0F / dpi_;
    if (x < 0 || x >= client.right || y >= client.bottom || dip_y < 4) return 0;
    if (message == WM_LBUTTONUP && page_count_ > 1) {
      const float dip_x = x * 96.0F / dpi_, width = client.right * 96.0F / dpi_;
      if (Contains(PageBounds(false, width), dip_x, dip_y)) { Page(-1); return 0; }
      if (Contains(PageBounds(true, width), dip_x, dip_y)) { Page(1); return 0; }
    }
    const auto row = static_cast<unsigned int>((dip_y - 4) / row_height_);
    if (row >= recommendations_.size()) return 0;
    if (message == WM_LBUTTONUP) Accept(row);
    else if (selected_ != static_cast<int>(row)) {
      selected_ = static_cast<int>(row);
      InvalidateRect(hwnd(), nullptr, FALSE);
    }
    return 0;
  }
  if (message == WM_KEYDOWN && w_param == VK_ESCAPE) { Dismiss(); return 0; }
  return DefWindowProcW(hwnd(), message, w_param, l_param);
}

}  // namespace zhixu::input_host::ui
