#include <gtest/gtest.h>

#include "input_host/ui/candidate_window.h"
#include "input_host/ui/recommendation_window.h"
#include "input_host/ui/window_placement.h"

namespace zhixu::input_host::ui {
namespace {

TEST(WindowPlacement, RecommendationFlipsBelowAtTopEdge) {
  const Rect caret{100, 2, 102, 22};
  const Rect work{0, 0, 1920, 1080};

  const auto placed = PlaceCandidate(caret, work, {320, 80}, PlacementPreference::Above);

  EXPECT_FALSE(placed.placed_above);
  EXPECT_GE(placed.bounds.top, caret.bottom);
}

TEST(WindowPlacement, ClampsNegativeCoordinateMonitorAtEverySupportedScale) {
  const Rect work{-1920, 0, 0, 1080};
  for (const int scale : {100, 125, 150, 200}) {
    const int width = 320 * scale / 100;
    const int height = 80 * scale / 100;
    const Rect caret{-8, 900, -6, 920};

    const auto placed = PlaceCandidate(caret, work, {width, height}, PlacementPreference::Below);

    EXPECT_GE(placed.bounds.left, work.left);
    EXPECT_LE(placed.bounds.right, work.right);
    EXPECT_GE(placed.bounds.top, work.top);
    EXPECT_LE(placed.bounds.bottom, work.bottom);
  }
}

TEST(WindowPlacement, ClampsRightAndBottomAndFlipsAbove) {
  const Rect caret{1900, 1040, 1902, 1060};
  const Rect work{0, 0, 1920, 1080};

  const auto placed = PlaceCandidate(caret, work, {320, 80}, PlacementPreference::Below);

  EXPECT_TRUE(placed.placed_above);
  EXPECT_EQ(placed.bounds.right, work.right);
  EXPECT_LE(placed.bounds.bottom, caret.top);
}

TEST(CandidateWindow, UsesNonActivatingToolWindowStyles) {
  EXPECT_EQ(WindowBase::WindowStyle(), WS_POPUP);
  EXPECT_NE(WindowBase::ExtendedWindowStyle() & WS_EX_TOOLWINDOW, 0U);
  EXPECT_NE(WindowBase::ExtendedWindowStyle() & WS_EX_NOACTIVATE, 0U);
  EXPECT_NE(WindowBase::ExtendedWindowStyle() & WS_EX_TOPMOST, 0U);
}

TEST(CandidateWindow, AcceptsOnlyAltNumberShortcuts) {
  CandidateWindow window;
  unsigned int accepted = 99;
  window.SetCandidates({CandidateLine{.text = L"first", .index = 4},
                        CandidateLine{.text = L"second", .index = 7}},
                       [&accepted](const unsigned int index) { accepted = index; });

  EXPECT_FALSE(window.AcceptShortcut(1, false));
  EXPECT_TRUE(window.AcceptShortcut(2, true));
  EXPECT_EQ(accepted, 7U);
  EXPECT_FALSE(window.AcceptShortcut(3, true));
}

TEST(CandidateWindow, EscapeDismissesAnAlreadyShownWindowWithoutActivation) {
  CandidateWindow window;
  ASSERT_TRUE(window.Create(GetModuleHandleW(nullptr), {320, 80}));
  window.ShowAt({0, 0, 0, 0});
  ASSERT_TRUE(window.visible());

  SendMessageW(window.hwnd(), WM_KEYDOWN, VK_ESCAPE, 0);

  EXPECT_FALSE(window.visible());
}

TEST(CandidateWindow, CreatedWindowRetainsNoActivateStyle) {
  CandidateWindow window;
  ASSERT_TRUE(window.Create(GetModuleHandleW(nullptr), {320, 80}));

  const LONG_PTR extended_style = GetWindowLongPtrW(window.hwnd(), GWL_EXSTYLE);

  EXPECT_NE(extended_style & WS_EX_NOACTIVATE, 0);
  EXPECT_NE(extended_style & WS_EX_TOOLWINDOW, 0);
  EXPECT_NE(extended_style & WS_EX_TOPMOST, 0);
}

TEST(RecommendationWindow, AcceptsOnlyAltNumberShortcutsAndDismissesOnEscape) {
  RecommendationWindow window;
  unsigned int accepted = 99;
  window.SetRecommendations({L"first", L"second"},
                            [&accepted](const unsigned int index) { accepted = index; });

  EXPECT_FALSE(window.AcceptShortcut(1, false));
  EXPECT_TRUE(window.AcceptShortcut(2, true));
  EXPECT_EQ(accepted, 1U);
  EXPECT_FALSE(window.AcceptShortcut(3, true));

  ASSERT_TRUE(window.Create(GetModuleHandleW(nullptr), {320, 80}));
  window.ShowAt({0, 0, 0, 0});
  ASSERT_TRUE(window.visible());
  SendMessageW(window.hwnd(), WM_KEYDOWN, VK_ESCAPE, 0);
  EXPECT_FALSE(window.visible());
}

}  // namespace
TEST(CandidateWindow, NativeWindowIsPerMonitorDpiAware) {
  CandidateWindow window;
  ASSERT_TRUE(window.Create(GetModuleHandleW(nullptr), {200, 40}));
  EXPECT_TRUE(AreDpiAwarenessContextsEqual(GetWindowDpiAwarenessContext(window.hwnd()),
                                         DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2));
}

TEST(CandidateWindow, HorizontalLayoutSharesPixelHitTestsAtEveryDpi) {
  for (const auto dpi : {96U, 120U, 144U, 192U}) {
    CandidateWindow window;
    std::vector<CandidateLine> lines;
    for (unsigned int i = 0; i < 8; ++i) lines.push_back({.text = L"候选词", .index = i});
    window.SetCandidates(lines, {});
    const auto size = window.Measure(dpi, 900);
    EXPECT_LE(size.width, 900);
    EXPECT_EQ(size.height, static_cast<int>(40 * dpi / 96));
    const auto bounds = window.CandidateBounds();
    ASSERT_EQ(bounds.size(), 8U);
    for (unsigned int i = 0; i < 8; ++i) {
      EXPECT_EQ(window.HitTest((bounds[i].left + bounds[i].right) / 2,
                              (bounds[i].top + bounds[i].bottom) / 2), i);
      if (i) EXPECT_GE(bounds[i].left, bounds[i-1].right);
    }
    EXPECT_FALSE(window.HitTest(-1, 10).has_value());
    EXPECT_FALSE(window.HitTest(10, size.height).has_value());
  }
}


TEST(RecommendationWindow, MouseUsesScaledRowsAfterDpiChange) {
  RecommendationWindow window;
  ASSERT_TRUE(window.Create(GetModuleHandleW(nullptr), {320, 86}));
  unsigned int accepted = 99;
  window.SetRecommendations({L"first", L"second", L"third"},
      [&](unsigned int index) { accepted = index; });
  window.ShowAt({100, 100, 420, 186});
  RECT suggested{100, 100, 740, 272};
  SendMessageW(window.hwnd(), WM_DPICHANGED, MAKELONG(192, 192), reinterpret_cast<LPARAM>(&suggested));
  SendMessageW(window.hwnd(), WM_LBUTTONUP, 0, MAKELPARAM(20, 84));
  EXPECT_EQ(accepted, 1U);
}

TEST(RecommendationWindow, RejectsOutsideClicksAndConsumesSelectionOnce) {
  RecommendationWindow window;
  ASSERT_TRUE(window.Create(GetModuleHandleW(nullptr), {320, 60}));
  unsigned int calls = 0;
  window.SetRecommendations({L"first", L"second"}, [&](unsigned int) { ++calls; });
  window.ShowAt({100, 100, 420, 160});
  SendMessageW(window.hwnd(), WM_LBUTTONUP, 0, MAKELPARAM(-1, 10));
  EXPECT_EQ(calls, 0U);
  SendMessageW(window.hwnd(), WM_LBUTTONUP, 0, MAKELPARAM(600, 10));
  EXPECT_EQ(calls, 0U);
  SendMessageW(window.hwnd(), WM_LBUTTONUP, 0, MAKELPARAM(20, 10));
  SendMessageW(window.hwnd(), WM_LBUTTONUP, 0, MAKELPARAM(20, 10));
  EXPECT_EQ(calls, 1U);
}


TEST(RecommendationWindow, MeasuredCitationRowsShareScaledMouseBounds) {
  for (const unsigned int dpi : {96U, 120U, 144U, 192U}) {
    RecommendationWindow window;
    ASSERT_TRUE(window.Create(GetModuleHandleW(nullptr), {320, 100}));
    unsigned int accepted = 99;
    window.SetRecommendations({L"first", L"second", L"third"},
        [&](unsigned int index) { accepted = index; }, {L"Book · Author", L"", L""});
    const auto size = window.Measure(dpi, 280);
    EXPECT_LE(size.width, 280);
    const int left = 100, top = 100;
    window.ShowAt({left, top, left + size.width, top + size.height});
    // Second 48 DIP row: midpoint (4 + 48 + 24) == 76 DIP.
    SendMessageW(window.hwnd(), WM_LBUTTONUP, 0, MAKELPARAM(20, 76 * dpi / 96));
    EXPECT_EQ(accepted, 1U);
  }
}

TEST(RecommendationWindow, PagingFooterIsMeasuredOnlyForMultiplePages) {
  for (const unsigned int dpi : {96U, 120U, 144U, 192U}) {
    for (const bool cited : {false, true}) {
      RecommendationWindow window;
      window.SetRecommendations({L"first", L"second", L"third"}, {},
          cited ? std::vector<std::wstring>{L"Book", L"", L""} : std::vector<std::wstring>{});
      window.SetPaging(0, 1, {});
      const auto single = window.Measure(dpi, 280);
      window.SetPaging(0, 10, {});
      const auto paged = window.Measure(dpi, 280);
      EXPECT_EQ(single.height, static_cast<int>(((cited ? 152 : 86) * dpi + 95) / 96));
      EXPECT_EQ(paged.height, static_cast<int>(((cited ? 180 : 114) * dpi + 95) / 96));
      EXPECT_EQ(paged.width, 280);
    }
  }
}

TEST(RecommendationWindow, MousePageNavigationSharesScaledFooterBounds) {
  for (const unsigned int dpi : {96U, 120U, 144U, 192U}) {
    for (const bool cited : {false, true}) {
      RecommendationWindow window;
      ASSERT_TRUE(window.Create(GetModuleHandleW(nullptr), {420, 200}));
      int direction = 0;
      unsigned int accepted = 99;
      auto present = [&] {
        window.SetRecommendations({L"first", L"second", L"third"},
            [&](unsigned int index) { accepted = index; },
            cited ? std::vector<std::wstring>{L"Book", L"", L""} : std::vector<std::wstring>{});
        window.SetPaging(1, 10, [&](int delta) { direction = delta; });
        const auto size = window.Measure(dpi, 900);
        window.ShowAt({100, 100, 100 + size.width, 100 + size.height});
      };
      const int footer_y = (cited ? 162 : 96) * dpi / 96;
      present();
      const HWND foreground = GetForegroundWindow();
      SendMessageW(window.hwnd(), WM_LBUTTONUP, 0, MAKELPARAM(20 * dpi / 96, footer_y));
      EXPECT_EQ(direction, -1);
      EXPECT_EQ(accepted, 99U);
      EXPECT_TRUE(window.visible());
      EXPECT_EQ(GetForegroundWindow(), foreground);
      direction = 0;
      present();
      SendMessageW(window.hwnd(), WM_LBUTTONUP, 0, MAKELPARAM(400 * dpi / 96, footer_y));
      EXPECT_EQ(direction, 1);
      EXPECT_EQ(accepted, 99U);
      EXPECT_TRUE(window.visible());
      EXPECT_EQ(SendMessageW(window.hwnd(), WM_MOUSEACTIVATE, 0,
                            MAKELPARAM(HTCLIENT, WM_LBUTTONDOWN)), MA_NOACTIVATE);
    }
  }
}

TEST(RecommendationWindow, PageEndpointsAndFooterGapsDoNotNavigateOrAccept) {
  RecommendationWindow window;
  ASSERT_TRUE(window.Create(GetModuleHandleW(nullptr), {420, 114}));
  unsigned int page_calls = 0, accept_calls = 0;
  auto present = [&](unsigned int page) {
    window.SetRecommendations({L"first", L"second", L"third"},
        [&](unsigned int) { ++accept_calls; });
    window.SetPaging(page, 10, [&](int) { ++page_calls; });
    const auto size = window.Measure(96, 900);
    window.ShowAt({100, 100, 100 + size.width, 100 + size.height});
  };
  present(0);
  SendMessageW(window.hwnd(), WM_LBUTTONUP, 0, MAKELPARAM(20, 96));
  SendMessageW(window.hwnd(), WM_LBUTTONUP, 0, MAKELPARAM(210, 96));
  SendMessageW(window.hwnd(), WM_LBUTTONUP, 0, MAKELPARAM(-1, 96));
  SendMessageW(window.hwnd(), WM_LBUTTONUP, 0, MAKELPARAM(420, 96));
  SendMessageW(window.hwnd(), WM_LBUTTONUP, 0, MAKELPARAM(400, 114));
  EXPECT_EQ(page_calls, 0U);
  EXPECT_EQ(accept_calls, 0U);
  EXPECT_TRUE(window.AcceptShortcut(3, true));
  EXPECT_EQ(accept_calls, 1U);
  present(9);
  SendMessageW(window.hwnd(), WM_LBUTTONUP, 0, MAKELPARAM(400, 96));
  EXPECT_EQ(page_calls, 0U);
  EXPECT_TRUE(window.AcceptShortcut(1, true));
  EXPECT_EQ(accept_calls, 2U);
}

TEST(RecommendationWindow, NavigationConsumesOldPageCallbacksUntilNextPresentation) {
  RecommendationWindow window;
  ASSERT_TRUE(window.Create(GetModuleHandleW(nullptr), {420, 114}));
  unsigned int page_calls = 0, accept_calls = 0;
  window.SetRecommendations({L"first", L"second", L"third"},
      [&](unsigned int) { ++accept_calls; });
  window.SetPaging(0, 10, [&](int) { ++page_calls; });
  const auto size = window.Measure(96, 900);
  window.ShowAt({100, 100, 100 + size.width, 100 + size.height});
  SendMessageW(window.hwnd(), WM_LBUTTONUP, 0, MAKELPARAM(400, 96));
  SendMessageW(window.hwnd(), WM_LBUTTONUP, 0, MAKELPARAM(400, 96));
  SendMessageW(window.hwnd(), WM_LBUTTONUP, 0, MAKELPARAM(20, 10));
  EXPECT_FALSE(window.AcceptShortcut(1, true));
  EXPECT_EQ(page_calls, 1U);
  EXPECT_EQ(accept_calls, 0U);
  EXPECT_TRUE(window.visible());
  window.SetRecommendations({L"next"}, [&](unsigned int) { ++accept_calls; });
  window.SetPaging(1, 10, [&](int) { ++page_calls; });
  EXPECT_TRUE(window.AcceptShortcut(1, true));
  EXPECT_FALSE(window.AcceptShortcut(1, true));
  EXPECT_EQ(accept_calls, 1U);
}

TEST(RecommendationWindow, DismissRevokesPageNavigationCallbacks) {
  RecommendationWindow window;
  ASSERT_TRUE(window.Create(GetModuleHandleW(nullptr), {420, 114}));
  unsigned int page_calls = 0;
  window.SetRecommendations({L"first", L"second", L"third"}, [](unsigned int) {});
  window.SetPaging(0, 10, [&](int) { ++page_calls; });
  const auto size = window.Measure(96, 900);
  window.ShowAt({100, 100, 100 + size.width, 100 + size.height});
  window.Dismiss();
  SendMessageW(window.hwnd(), WM_LBUTTONUP, 0, MAKELPARAM(400, 96));
  EXPECT_EQ(page_calls, 0U);
  EXPECT_FALSE(window.AcceptShortcut(1, true));
  EXPECT_FALSE(window.visible());
}
}  // namespace zhixu::input_host::ui

namespace zhixu::input_host::ui {
TEST(RecommendationWindow, SixRowsAreMeasuredAndSixthShortcutSelectsOnce) {
  RecommendationWindow window; unsigned int selected = 99, calls = 0;
  window.SetRecommendations({L"one",L"two",L"three",L"four",L"five",L"six",L"seven"},
      [&](unsigned int i) { selected = i; ++calls; });
  EXPECT_EQ(window.Measure(96, 800).height, 8 + 6 * 26);
  EXPECT_FALSE(window.AcceptShortcut(7, true));
  EXPECT_TRUE(window.AcceptShortcut(6, true)); EXPECT_EQ(selected, 5U);
  EXPECT_FALSE(window.AcceptShortcut(6, true)); EXPECT_EQ(calls, 1U);
}
}

#include <algorithm>
namespace zhixu::input_host {
namespace {
TEST(CandidateGridWindow, SixColumnsFiveRowsScaleAndHitTestEveryCell) {
  for (unsigned dpi : {96u, 144u, 192u}) {
    ui::CandidateWindow window;
    std::vector<ui::CandidateLine> words;
    for (unsigned i=0; i<30; ++i)
      words.push_back({.text=L"候选词"+std::to_wstring(i+1), .index=i, .selected=i==7});
    unsigned accepted=99;
    window.SetCandidates(words, [&](unsigned i){accepted=i;});
    window.SetPageState(1, true, {});
    const auto size = window.Measure(dpi, 560*dpi/96);
    const auto& cells = window.CandidateBounds();
    ASSERT_EQ(cells.size(), 30u);
    EXPECT_GT(size.height, static_cast<int>(180*dpi/96));
    for (unsigned i=0; i<30; ++i) {
      EXPECT_EQ(window.HitTest((cells[i].left+cells[i].right)/2,
                               (cells[i].top+cells[i].bottom)/2), i);
      if (i>=6) EXPECT_GT(cells[i].top, cells[i-6].top);
      if (i%6) EXPECT_EQ(cells[i].top, cells[i-1].top);
    }
    EXPECT_TRUE(window.AcceptShortcut(2,true));
    EXPECT_EQ(accepted, 7u);
    window.SetPageState(0,true,{});
    window.SetCandidates(std::vector<ui::CandidateLine>(words.begin(),words.begin()+8),{});
    EXPECT_EQ(window.Measure(dpi,1000*dpi/96).height,static_cast<int>(40*dpi/96));
  }
}

TEST(CandidateGridWindow, DpiChangeKeepsBottomRowsInsideMonitorWorkArea) {
  ui::CandidateWindow window;
  ASSERT_TRUE(window.Create(GetModuleHandleW(nullptr), {560,240}));
  std::vector<ui::CandidateLine> words;
  for(unsigned i=0;i<30;++i) words.push_back({.text=L"候选",.index=i,.selected=i==0});
  window.SetCandidates(words,{});window.SetPageState(1,true,{});
  MONITORINFO monitor{sizeof(monitor)};
  ASSERT_TRUE(GetMonitorInfoW(MonitorFromWindow(window.hwnd(),MONITOR_DEFAULTTOPRIMARY),&monitor));
  const auto size=window.Measure(192, std::min(1120L,monitor.rcWork.right-monitor.rcWork.left),400);
  RECT suggested{monitor.rcWork.left,monitor.rcWork.bottom-size.height,
                 monitor.rcWork.left+size.width,monitor.rcWork.bottom};
  window.ShowAt({suggested.left,suggested.top,suggested.right,suggested.bottom});
  SendMessageW(window.hwnd(),WM_DPICHANGED,MAKELONG(192,192),reinterpret_cast<LPARAM>(&suggested));
  RECT after{};GetWindowRect(window.hwnd(),&after);
  EXPECT_LE(after.bottom,monitor.rcWork.bottom);
  EXPECT_GE(after.top,monitor.rcWork.top);
  EXPECT_LE(window.CandidateBounds()[29].bottom,after.bottom-after.top);
}
}

}

TEST(RuntimeSettings, ExplicitThemeOverridesSystemAndCanBeChanged) {
  zhixu::input_host::ui::CandidateWindow window;
  ASSERT_TRUE(window.Create(GetModuleHandleW(nullptr),{420,34}));
  window.ApplyTheme(2); EXPECT_TRUE(window.dark_mode());
  window.ApplyTheme(1); EXPECT_FALSE(window.dark_mode());
  window.ApplyTheme(2); EXPECT_TRUE(window.dark_mode());
}

TEST(RuntimeSettings, ExplicitThemeSurvivesSystemThemeNotifications) {
  zhixu::input_host::ui::CandidateWindow window;
  ASSERT_TRUE(window.Create(GetModuleHandleW(nullptr),{420,34}));
  const bool requested=!window.dark_mode(); window.ApplyTheme(requested?2:1);
  SendMessageW(window.hwnd(),WM_SETTINGCHANGE,0,0); EXPECT_EQ(window.dark_mode(),requested);
  SendMessageW(window.hwnd(),WM_THEMECHANGED,0,0); EXPECT_EQ(window.dark_mode(),requested);
}
