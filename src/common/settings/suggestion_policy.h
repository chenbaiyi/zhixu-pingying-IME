#pragma once
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <vector>
#include <string>
#include <cstdint>
namespace zhixu::settings {
// A snapshot of settings and immutable active versions. It never reads application text.
inline std::string SuggestionPolicy(const std::filesystem::path& settings,const std::filesystem::path& packages) {
 try {
  std::uint64_t hash=1469598103934665603ull;
  auto append=[&](const std::string& value){for(unsigned char c:value){hash^=c;hash*=1099511628211ull;}hash^=0xff;hash*=1099511628211ull;};
  auto read=[&](const std::filesystem::path& file){
   if(!std::filesystem::exists(file)){append("missing");return;}
   if(std::filesystem::file_size(file)>1024*1024)throw std::runtime_error("policy size");
   std::ifstream input(file,std::ios::binary);if(!input)throw std::runtime_error("policy read");
   append({std::istreambuf_iterator<char>(input),{}});
  };
  read(settings);std::vector<std::filesystem::path> active;
  if(std::filesystem::exists(packages))for(const auto& p:std::filesystem::directory_iterator(packages)){
   if(p.is_directory() && !p.is_symlink() && std::filesystem::exists(p.path()/L"active.json"))active.push_back(p.path()/L"active.json");
  }
  std::sort(active.begin(),active.end());for(const auto& p:active){const auto id=p.parent_path().filename().u8string();append({id.begin(),id.end()});read(p);}
  return std::to_string(hash);
 }catch(...){return {};}
}
}
