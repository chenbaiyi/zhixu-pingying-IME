#include <opencc/SimpleConverter.hpp>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <iostream>
int wmain(int argc,wchar_t** argv) {
 if(argc!=2)return 2;
 try {
  opencc::SimpleConverter converter(std::filesystem::path(argv[1]).string());
  auto rows=nlohmann::json::parse(std::cin);
  for(auto& row:rows){
   if(row.contains("segments")){
    row["original_segments"]=row["segments"];
    for(auto& s:row["segments"])s=converter.Convert(s.get<std::string>());
   }
   for(const auto* field:{"work","author","chapter","text"})if(row.contains(field) && row[field].is_string())row[field]=converter.Convert(row[field].get<std::string>());
   row["display_version"]="简体转换；原文另存";
  }
  std::cout<<rows.dump();return 0;
 }catch(const std::exception& e){std::cerr<<e.what();return 1;}
}
