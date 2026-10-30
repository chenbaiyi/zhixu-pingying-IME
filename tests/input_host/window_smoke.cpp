#include "input_host/ui/candidate_window.h"
#include "input_host/ui/recommendation_window.h"

#include <dwmapi.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace zhixu::input_host::ui;
namespace {
int failures = 0;
void Check(bool value, const char* label) {
  if (!value) { ++failures; std::cerr << "FAIL " << label << '\n'; }
}
BOOL CALLBACK Monitor(HMONITOR monitor, HDC, LPRECT, LPARAM data) {
  MONITORINFO info{sizeof(info)};
  if (GetMonitorInfoW(monitor, &info)) reinterpret_cast<std::vector<Rect>*>(data)->push_back(
      {info.rcWork.left, info.rcWork.top, info.rcWork.right, info.rcWork.bottom});
  return TRUE;
}
void Paint(HWND hwnd) {
  MSG message{};
  while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
    TranslateMessage(&message); DispatchMessageW(&message);
  }
  RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
  DwmFlush();
}
void SaveWindow(HWND hwnd, const std::filesystem::path& path, bool dark) {
  Paint(hwnd);
  RECT rect{}; GetClientRect(hwnd, &rect);
  const int width = rect.right, height = rect.bottom;
  HDC source = GetDC(hwnd), memory = CreateCompatibleDC(source);
  HBITMAP bitmap = CreateCompatibleBitmap(source, width, height);
  const auto old = SelectObject(memory, bitmap);
  SendMessageW(hwnd, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(memory), PRF_CLIENT);
  // Padding (0,0) is outside every rounded selection and all text.
  Check(GetPixel(memory, 0, 0) == (dark ? RGB(32,32,32) : RGB(255,255,255)), "theme pixels");
  SelectObject(memory, old);
  BITMAPINFO info{};
  info.bmiHeader = {sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB};
  std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4);
  Check(GetDIBits(memory, bitmap, 0, height, pixels.data(), &info, DIB_RGB_COLORS) == height, "bitmap pixels");
  BITMAPFILEHEADER header{};
  header.bfType = 0x4d42;
  header.bfOffBits = sizeof(header) + sizeof(info.bmiHeader);
  header.bfSize = header.bfOffBits + static_cast<DWORD>(pixels.size());
  std::ofstream output(path, std::ios::binary);
  output.write(reinterpret_cast<const char*>(&header), sizeof(header));
  output.write(reinterpret_cast<const char*>(&info.bmiHeader), sizeof(info.bmiHeader));
  output.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
  Check(output.good(), "write own-window image");
  DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(hwnd, source);
}
void ValidateWindow(WindowBase& window, const Rect bounds, HWND foreground) {
  const auto style = GetWindowLongPtrW(window.hwnd(), GWL_EXSTYLE);
  Check((style & (WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST)) ==
        (WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST), "native window styles");
  Check(SendMessageW(window.hwnd(), WM_MOUSEACTIVATE, 0, 0) == MA_NOACTIVATE, "mouse no activation");
  Check(GetForegroundWindow() == foreground, "foreground unchanged");
  RECT actual{}; GetWindowRect(window.hwnd(), &actual);
  Check(actual.left == bounds.left && actual.top == bounds.top && actual.right == bounds.right &&
        actual.bottom == bounds.bottom, "physical placement");
}
}
int wmain(int argc, wchar_t** argv) {
  std::filesystem::path output;
  if (argc == 3 && std::wstring(argv[1]) == L"--output") output = argv[2];
  if (!output.empty()) std::filesystem::create_directories(output);
  SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  std::vector<Rect> monitors;
  EnumDisplayMonitors(nullptr, nullptr, Monitor, reinterpret_cast<LPARAM>(&monitors));
  Check(!monitors.empty(), "monitor enumeration");
  unsigned int cases = 0;
  for (size_t monitor = 0; monitor < monitors.size(); ++monitor) {
    const auto work = monitors[monitor];
    const std::vector<Rect> carets{{work.left,work.top,work.left+2,work.top+20},
        {work.right-2,work.top,work.right,work.top+20},
        {work.left,work.bottom-20,work.left+2,work.bottom},
        {work.right-2,work.bottom-20,work.right,work.bottom},
        {work.left+(work.right-work.left)/2,work.top+(work.bottom-work.top)/2,
         work.left+(work.right-work.left)/2+2,work.top+(work.bottom-work.top)/2+20}};
    for (const auto dpi : {96U,120U,144U,192U}) for (bool dark : {false,true}) {
      CandidateWindow candidate;
      RecommendationWindow recommendation;
      Check(candidate.Create(GetModuleHandleW(nullptr), {320,40}), "candidate create");
      Check(recommendation.Create(GetModuleHandleW(nullptr), {420,152}), "recommendation create");
      if (!candidate.hwnd() || !recommendation.hwnd()) return 1;
      candidate.SetDarkMode(dark); recommendation.SetDarkMode(dark);
      candidate.SetCandidates({{.text=L"你好",.index=0,.selected=true},
          {.text=L"知序输入法",.index=1}, {.text=L"长词完整上屏：春风又绿江南岸",.index=2},
          {.text=L"明月何时照我还",.index=3,.source=L"泊船瓜洲",.author=L"王安石"}}, {});
      candidate.SetPageState(0,true,{});
      for (size_t position=0; position<carets.size(); ++position) {
        unsigned int accepted = 99;
        recommendation.SetRecommendations({L"春风又绿江南岸",L"明月何时照我还",L"生成建议不冒充引文",L"海上生明月",L"天涯共此时",L"此时相望不相闻"},
            [&](unsigned int index){accepted=index;},
            {L"泊船瓜洲 · 王安石",L"泊船瓜洲 · 王安石",L"",L"望月怀远 · 张九龄",L"望月怀远 · 张九龄",L"春江花月夜 · 张若虚"});
        for (int type=0;type<2;++type) {
          WindowBase& window=type==0 ? static_cast<WindowBase&>(candidate) : recommendation;
          const Size size=type==0 ? candidate.Measure(dpi,work.right-work.left) :
                                     recommendation.Measure(dpi,work.right-work.left);
          const auto bounds=PlaceCandidate(carets[position],work,size,
              type==0?PlacementPreference::Below:PlacementPreference::Above).bounds;
          Check(bounds.left>=work.left && bounds.right<=work.right &&
                bounds.top>=work.top && bounds.bottom<=work.bottom, "monitor clamping");
          const HWND foreground=GetForegroundWindow();
          window.ShowAt(bounds);
          RECT suggested{bounds.left,bounds.top,bounds.right,bounds.bottom};
          SendMessageW(window.hwnd(),WM_DPICHANGED,MAKELONG(dpi,dpi),reinterpret_cast<LPARAM>(&suggested));
          Paint(window.hwnd());
          ValidateWindow(window,bounds,foreground);
          Check(AreDpiAwarenessContextsEqual(GetWindowDpiAwarenessContext(window.hwnd()),
              DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2),"native DPI awareness");
          if (!output.empty() && monitor==0 && position==4 && (dpi==96 || dpi==192)) {
            const auto name=std::string(type==0?"candidate-":"recommendation-")+
                (dark?"dark-":"light-")+std::to_string(dpi)+".bmp";
            SaveWindow(window.hwnd(),output/name,dark);
          }
          if (type==1) {
            SendMessageW(window.hwnd(),WM_LBUTTONUP,0,MAKELPARAM(20,268*dpi/96));
            Check(accepted==5,"scaled sixth recommendation mouse selection");
            Check(!window.visible(),"selection hides recommendation");
            SendMessageW(window.hwnd(),WM_LBUTTONUP,0,MAKELPARAM(20,268*dpi/96));
            Check(accepted==5,"selection remains single");
          }
          window.Hide();
          Check(!window.visible(),"hide window");
          Check(GetForegroundWindow()==foreground,"foreground after hide unchanged");
          ++cases;
        }
      }
    }
  }
  std::cout<<"MONITORS="<<monitors.size()<<" CASES="<<cases<<" FAILURES="<<failures<<'\n';
  return failures==0?0:1;
}