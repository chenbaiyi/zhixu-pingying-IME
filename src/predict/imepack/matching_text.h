#pragma once
#include <windows.h>
#include <string>
#include <string_view>
namespace zhixu::predict::imepack {
inline std::string SimplifiedMatch(std::string_view s) {
 if(s.empty())return {};
 int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),static_cast<int>(s.size()),nullptr,0);if(n<=0)return std::string(s);
 std::wstring w(n,L'\0');MultiByteToWideChar(CP_UTF8,0,s.data(),static_cast<int>(s.size()),w.data(),n);
 int count=LCMapStringEx(L"zh-CN",LCMAP_SIMPLIFIED_CHINESE,w.data(),n,nullptr,0,nullptr,nullptr,0);if(count<=0)return std::string(s);
 std::wstring converted(count,L'\0');LCMapStringEx(L"zh-CN",LCMAP_SIMPLIFIED_CHINESE,w.data(),n,converted.data(),count,nullptr,nullptr,0);
 n=WideCharToMultiByte(CP_UTF8,0,converted.data(),count,nullptr,0,nullptr,nullptr);std::string out(n,'\0');WideCharToMultiByte(CP_UTF8,0,converted.data(),count,out.data(),n,nullptr,nullptr);return out;
}
}
