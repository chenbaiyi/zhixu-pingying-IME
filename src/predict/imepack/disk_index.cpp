#include "predict/imepack/matching_text.h"
#include "predict/imepack/disk_index.h"
#include <windows.h>
#include <sqlite3.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <memory>
#include <string_view>
#include <unordered_set>
namespace zhixu::predict::imepack {
namespace {
using Json = nlohmann::json;
std::string Utf8(const std::filesystem::path& p) { const auto s=p.u8string(); return {s.begin(),s.end()}; }
struct Db { sqlite3* p=nullptr; ~Db(){ if(p) sqlite3_close(p); } };
struct Statement { sqlite3_stmt* p=nullptr; ~Statement(){if(p) sqlite3_finalize(p);} };
bool Exec(sqlite3* db,const char* sql) { return sqlite3_exec(db,sql,nullptr,nullptr,nullptr)==SQLITE_OK; }
bool Prepare(sqlite3* db,Statement& q,const char* sql) {return sqlite3_prepare_v2(db,sql,-1,&q.p,nullptr)==SQLITE_OK;}
bool Bind(sqlite3_stmt* p,int i,const std::string& v) {return sqlite3_bind_text(p,i,v.data(),static_cast<int>(v.size()),SQLITE_TRANSIENT)==SQLITE_OK;}
std::string Text(sqlite3_stmt* q,int col) {const auto* s=sqlite3_column_text(q,col);return s?std::string(reinterpret_cast<const char*>(s),sqlite3_column_bytes(q,col)):std::string{};}
std::vector<size_t> Boundaries(std::string_view s) {
 std::vector<size_t> b; for(size_t i=0;i<s.size();++i) if((static_cast<unsigned char>(s[i])&0xc0)!=0x80)b.push_back(i);b.push_back(s.size());return b;
}
std::string Canonical(std::string_view raw) {
 const auto simplified=SimplifiedMatch(raw);const std::string_view s=simplified;
 // UTF-8 characters, never byte-wise clipping. Remove matching punctuation only.
 const std::string punctuation="，。！？；：、“”‘’（）《》〈〉"; std::string result;
 const auto b=Boundaries(s);
 for(size_t i=0;i+1<b.size();++i){const auto c=s.substr(b[i],b[i+1]-b[i]);
  if(c.size()==1 && (static_cast<unsigned char>(c[0])<=32 || std::string_view(",.!?;:\"'()").find(c)!=std::string_view::npos))continue;
  if(c.size()>1 && punctuation.find(c)!=std::string::npos)continue;result+=c;
 }return result;
}
std::string Remainder(const std::string& original,size_t count) {
 const auto b=Boundaries(original);size_t consumed=0;
 for(size_t i=0;i+1<b.size();++i) {const auto c=original.substr(b[i],b[i+1]-b[i]);if(!Canonical(c).empty())++consumed;if(consumed==count)return original.substr(b[i+1]);}
 return {};
}
bool Insert(sqlite3* db,Statement& row,Statement& keyword,const Json& e,int kind,
 const std::string& match,const std::string& original,const std::string& text,double weight,const std::string& chapter) {
 const auto key=Canonical(match);if(key.empty() || text.empty() || !std::isfinite(weight))return true;
 sqlite3_reset(row.p);sqlite3_clear_bindings(row.p);
 const bool edition=e.value("source_url",std::string{}).starts_with("https://github.com/kanripo/");
 const auto work=e.value("work",std::string{})+(edition?"（底本）":"");
 const auto author=(edition?std::string("底本署名："):std::string{})+e.value("author",std::string{});
 const std::string values[]={key,original,text,e.value("id",std::string{}),work,author,chapter,e.value("tags",Json::array()).dump()};
 for(int i=0;i<8;++i)if(!Bind(row.p,i+1,values[i]))return false;
 sqlite3_bind_int(row.p,9,kind);sqlite3_bind_double(row.p,10,weight);
 if(sqlite3_step(row.p)!=SQLITE_DONE)return false;
 const auto id=sqlite3_last_insert_rowid(db);
 if(kind==0 && e.contains("keywords") && e["keywords"].is_array())for(const auto& k:e["keywords"]){
  if(!k.is_string())return false;const auto token=Canonical(k.get<std::string>());if(token.empty())continue;
  sqlite3_reset(keyword.p);sqlite3_clear_bindings(keyword.p);Bind(keyword.p,1,token);sqlite3_bind_int64(keyword.p,2,id);
  if(sqlite3_step(keyword.p)!=SQLITE_DONE)return false;
 }return true;
}
}
tl::expected<void,PackageError> BuildDiskIndex(const std::filesystem::path& root) {
 const auto pending=root/L"indexes"/L"lookup.pending.sqlite", destination=root/L"indexes"/L"lookup.sqlite";
 std::error_code ec;std::filesystem::remove(pending,ec);
 bool ok=false;
 {
  Db db; if(sqlite3_open_v2(Utf8(pending).c_str(),&db.p,SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE,nullptr)!=SQLITE_OK)return tl::unexpected(PackageError::kIo);
  if(!Exec(db.p,"PRAGMA cache_size=-8192; PRAGMA journal_mode=DELETE; BEGIN; CREATE TABLE lookup(match TEXT, original TEXT, text TEXT, entry TEXT, work TEXT, author TEXT, chapter TEXT, tags TEXT, kind INTEGER, weight REAL); CREATE TABLE keywords(token TEXT, rowid_ref INTEGER);"))return tl::unexpected(PackageError::kIo);
  Statement row,keyword;
  if(!Prepare(db.p,row,"INSERT INTO lookup VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10)") || !Prepare(db.p,keyword,"INSERT INTO keywords VALUES(?1,?2)"))return tl::unexpected(PackageError::kIo);
  std::ifstream manifest_file(root/L"manifest.json",std::ios::binary);const auto manifest=Json::parse(manifest_file,nullptr,false);
  if(manifest.is_discarded())return tl::unexpected(PackageError::kIntegrity);
  std::ifstream input(root/std::filesystem::u8path(manifest["entries"]["path"].get<std::string>()),std::ios::binary);
  if(!input)return tl::unexpected(PackageError::kIo);
  std::string line;
  try {while(std::getline(input,line)){
   if(line.size()>1024*1024)return tl::unexpected(PackageError::kLimitExceeded);
   const auto e=Json::parse(line,nullptr,false);if(e.is_discarded())return tl::unexpected(PackageError::kIntegrity);
   const auto type=e.value("type",std::string{});
   if(type=="continuation"){
    for(const auto& c:e.at("completions"))if(!Insert(db.p,row,keyword,e,0,e.at("trigger"),e.at("trigger"),c.at("text"),c.at("weight"),{}))return tl::unexpected(PackageError::kIo);
   } else if(type=="phrase"){
    if(!Insert(db.p,row,keyword,e,2,e.at("text"),e.at("text")," ",e.value("weight",0.5),{}))return tl::unexpected(PackageError::kIo);
   } else if(type=="quotation"){
    const auto& segments=e.at("segments");
    for(size_t i=0;i<segments.size();++i){
     const auto match=segments[i].get<std::string>();const auto next=i+1<segments.size()?segments[i+1].get<std::string>():std::string{};
     // Last segments also participate in prefix completion.
     const auto chapter=e.value("chapter","segment:"+std::to_string(i+1));
     if(!Insert(db.p,row,keyword,e,1,match,match,next.empty()?" ":next,1.0,chapter))return tl::unexpected(PackageError::kIo);
    }
   }
  }}catch(const Json::exception&){return tl::unexpected(PackageError::kIntegrity);}
  if(!input.eof())return tl::unexpected(PackageError::kIo);
  ok=Exec(db.p,"CREATE INDEX match_index ON lookup(match,kind); CREATE INDEX keyword_index ON keywords(token,rowid_ref); PRAGMA user_version=1; COMMIT;");
 }
 if(!ok || !MoveFileExW(pending.c_str(),destination.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){std::filesystem::remove(pending,ec);return tl::unexpected(PackageError::kIo);}return {};
}
std::vector<Suggestion> RecallDiskIndex(const std::filesystem::path& root,const PredictionContext& context,
 size_t limit,std::chrono::steady_clock::time_point deadline, bool priority_only) {
 std::vector<Suggestion> out; Db db;const auto path=root/L"indexes"/L"lookup.sqlite";
 if(sqlite3_open_v2(Utf8(path).c_str(),&db.p,SQLITE_OPEN_READONLY,nullptr)!=SQLITE_OK)return out;
 Exec(db.p,"PRAGMA cache_size=-512; PRAGMA query_only=ON;");
 sqlite3_progress_handler(db.p,1000,[](void* p){return std::chrono::steady_clock::now()>=*static_cast<std::chrono::steady_clock::time_point*>(p) ? 1 : 0;},&deadline);
 const auto normalized=Canonical(context.committed_text);const auto boundaries=Boundaries(normalized);
 std::unordered_set<std::string> seen;
 auto collect=[&](Statement& q,const std::string& matched,int mode,unsigned int distance=0) {
  while(out.size()<limit && std::chrono::steady_clock::now()<deadline && sqlite3_step(q.p)==SQLITE_ROW){
   const auto key=Text(q.p,0), original=Text(q.p,1);const int kind=sqlite3_column_int(q.p,8);
   if(kind==1 && Boundaries(matched).size()-1<3 && Boundaries(normalized).size()-1>2)continue;
   const bool partial=mode==0 && key!=matched && kind!=0;
   auto text=partial?Remainder(original,Boundaries(matched).size()-1):Text(q.p,2);
   if(text.empty() || text==" ")continue;
   if(!partial && kind==1 && !context.committed_text.empty()){
    const auto rest=Remainder(original,Boundaries(key).size()-1);
    const auto context_boundaries=Boundaries(context.committed_text);
    const auto last=context_boundaries.size()>1?context.committed_text.substr(context_boundaries[context_boundaries.size()-2]):std::string{};
    if(!rest.empty() && !context.committed_text.ends_with(rest) && !Canonical(last).empty())text=rest+text;
   }
   const auto id=Text(q.p,3);if(!seen.insert(id+"\n"+text).second)continue;
   const auto tags=Json::parse(Text(q.p,7),nullptr,false);if(!tags.is_array())continue;
   Suggestion s{.text=std::move(text),.kind=(mode==1 || kind==2)?LocalSuggestionKind::kBaseNGram:(kind==1?LocalSuggestionKind::kQuotation:LocalSuggestionKind::kExactContinuation),
    .source_weight=sqlite3_column_double(q.p,9),.package_id=root.parent_path().filename().string(),.entry_id=id,
    .tags=tags.get<std::vector<std::string>>(),.source_title=kind==1?Text(q.p,4):(kind==2?"成语词语":"写作搭配"),.author=Text(q.p,5),.source_context=Text(q.p,6)};
   s.source_order=static_cast<std::uint64_t>(sqlite3_column_int64(q.p,10));s.is_phrase_completion=kind==2 && partial;s.context_distance=distance;s.match_length=static_cast<unsigned int>(Boundaries(matched).size()-1);out.push_back(std::move(s));
  }
 };
 Statement exact,prefix,curated_prefix,phrase_prefix,keywords;
 if(!Prepare(db.p,exact,priority_only ? "SELECT lookup.*,rowid AS source_order FROM lookup WHERE match=?1 AND (kind=2 OR (kind=1 AND work IN ('唐诗三百首','宋词三百首','古诗十九首'))) ORDER BY kind DESC,weight DESC LIMIT 64" : "SELECT lookup.*,rowid AS source_order FROM lookup WHERE match=?1 ORDER BY kind DESC,weight DESC LIMIT 64") ||
    !Prepare(db.p,prefix,"SELECT lookup.*,rowid AS source_order FROM lookup WHERE match>=?1 AND match<?2 AND kind=1 ORDER BY match LIMIT 64") ||
    !Prepare(db.p,curated_prefix,"SELECT * FROM (SELECT lookup.*,rowid AS source_order FROM lookup WHERE match>=?1 AND match<?2 AND kind=1 ORDER BY match LIMIT 256) WHERE work IN ('唐诗三百首','宋词三百首','古诗十九首') LIMIT 32") ||
    !Prepare(db.p,phrase_prefix,"SELECT lookup.*,rowid AS source_order FROM lookup WHERE match>=?1 AND match<?2 AND kind=2 ORDER BY match LIMIT 64") ||
    !Prepare(db.p,keywords,"SELECT lookup.*,lookup.rowid AS source_order FROM keywords JOIN lookup ON lookup.rowid=keywords.rowid_ref WHERE token=?1 ORDER BY weight DESC LIMIT 8"))return out;
 const auto n=boundaries.size()-1;
 for(size_t len=std::min<size_t>(n,64);len>=2 && out.size()<limit && std::chrono::steady_clock::now()<deadline;--len){
  const auto suffix=normalized.substr(boundaries[n-len]);sqlite3_reset(exact.p);Bind(exact.p,1,suffix);collect(exact,suffix,0);
  sqlite3_reset(phrase_prefix.p);Bind(phrase_prefix.p,1,suffix);Bind(phrase_prefix.p,2,suffix+"\xF4\x8F\xBF\xBF");collect(phrase_prefix,suffix,0);
  if(len>=3){
   // Probe a bounded prefix window for curated collections before the ordinary
   // indexed prefix scan. Existing packages need no index rebuild.
   sqlite3_reset(curated_prefix.p);Bind(curated_prefix.p,1,suffix);Bind(curated_prefix.p,2,suffix+"\xF4\x8F\xBF\xBF");collect(curated_prefix,suffix,0);
   if(!priority_only) { sqlite3_reset(prefix.p);Bind(prefix.p,1,suffix);Bind(prefix.p,2,suffix+"\xF4\x8F\xBF\xBF");collect(prefix,suffix,0); }
  }
 }
 if(!priority_only && out.empty())for(size_t cursor=n;cursor>0 && out.size()<limit && std::chrono::steady_clock::now()<deadline;--cursor){
  const auto i=cursor-1;if(n-i>30)break;
  for(size_t len=std::min<size_t>(6,n-i);len>=2 && out.size()<limit;--len){
   if(n-i-len>24)continue;
   const auto token=normalized.substr(boundaries[i],boundaries[i+len]-boundaries[i]);sqlite3_reset(keywords.p);Bind(keywords.p,1,token);collect(keywords,token,1,static_cast<unsigned int>(n-i-len));
  }
 }return out;
}
}
