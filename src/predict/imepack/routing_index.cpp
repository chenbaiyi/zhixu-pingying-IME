#include "predict/imepack/matching_text.h"
#include "predict/imepack/routing_index.h"
#include <windows.h>
#include <sqlite3.h>
#include <chrono>
#include <unordered_set>
namespace zhixu::predict::imepack {
namespace {
struct Db {sqlite3* p=nullptr;~Db(){if(p)sqlite3_close(p);}};
struct Statement {sqlite3_stmt* p=nullptr;~Statement(){if(p)sqlite3_finalize(p);}};
std::string Utf8(const std::filesystem::path& p){const auto s=p.u8string();return {s.begin(),s.end()};}
bool Exec(sqlite3* p,const char* s){return sqlite3_exec(p,s,nullptr,nullptr,nullptr)==SQLITE_OK;}
bool Prepare(sqlite3* p,Statement& q,const char* s){return sqlite3_prepare_v2(p,s,-1,&q.p,nullptr)==SQLITE_OK;}
bool Bind(sqlite3_stmt* p,int n,const std::string& s){return sqlite3_bind_text(p,n,s.data(),static_cast<int>(s.size()),SQLITE_TRANSIENT)==SQLITE_OK;}
std::filesystem::path Base(const std::vector<std::filesystem::path>& roots){return roots.front().parent_path().parent_path();}
bool Open(Db& db,const std::vector<std::filesystem::path>& roots){
 if(roots.empty())return false;const auto base=Base(roots);
 for(const auto& p:roots)if(p.parent_path().parent_path()!=base)return false;
 const auto file=base/L".routing"/L"routing.sqlite";
 if(sqlite3_open_v2(Utf8(file).c_str(),&db.p,SQLITE_OPEN_READONLY,nullptr)!=SQLITE_OK)return false;
 Exec(db.p,"PRAGMA cache_size=-512; PRAGMA query_only=ON;");
 Statement q;if(!Prepare(db.p,q,"SELECT 1 FROM packages WHERE id=?1 AND version=?2"))return false;
 for(const auto& p:roots){sqlite3_reset(q.p);Bind(q.p,1,Utf8(p.parent_path().filename()));Bind(q.p,2,Utf8(p.filename()));if(sqlite3_step(q.p)!=SQLITE_ROW)return false;}
 return true;
}
std::vector<size_t> Boundaries(std::string_view s){std::vector<size_t> b;for(size_t i=0;i<s.size();++i)if((static_cast<unsigned char>(s[i])&0xc0)!=0x80)b.push_back(i);b.push_back(s.size());return b;}
std::string Canonical(std::string_view raw){const auto simplified=SimplifiedMatch(raw);const std::string_view s=simplified;const std::string punctuation="，。！？；：、“”‘’（）《》〈〉";std::string result;const auto b=Boundaries(s);
 for(size_t i=0;i+1<b.size();++i){const auto c=s.substr(b[i],b[i+1]-b[i]);if(c.size()==1 && (static_cast<unsigned char>(c[0])<=32 || std::string_view(",.!?;:\"'()").find(c)!=std::string_view::npos))continue;if(c.size()>1 && punctuation.find(c)!=std::string::npos)continue;result+=c;}return result;}
}
bool RoutingIndexMatches(const std::vector<std::filesystem::path>& roots){Db db;return roots.empty() || Open(db,roots);}
tl::expected<void,PackageError> BuildRoutingIndex(const std::vector<std::filesystem::path>& roots){
 if(roots.empty())return {};const auto base=Base(roots);
 for(const auto& p:roots)if(p.parent_path().parent_path()!=base)return tl::unexpected(PackageError::kUnsafePath);
 const auto dir=base/L".routing";std::error_code error;std::filesystem::create_directories(dir,error);if(error)return tl::unexpected(PackageError::kIo);
 const auto pending=dir/(L"routing."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(GetTickCount64())+L".sqlite");
 const auto destination=dir/L"routing.sqlite";bool ok=true;
 {
 Db db;if(sqlite3_open_v2(Utf8(pending).c_str(),&db.p,SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE,nullptr)!=SQLITE_OK)return tl::unexpected(PackageError::kIo);
 ok=Exec(db.p,"PRAGMA cache_size=-8192; PRAGMA journal_mode=DELETE; CREATE TABLE packages(id TEXT,version TEXT,PRIMARY KEY(id,version)); CREATE TABLE routes(token TEXT,id TEXT,PRIMARY KEY(token,id)) WITHOUT ROWID;");
 for(const auto& root:roots){
  if(!ok)break;
  if(!std::filesystem::is_regular_file(root/L"indexes"/L"lookup.sqlite",error) || error){ok=false;break;}
  {Statement attach;ok=Prepare(db.p,attach,"ATTACH DATABASE ?1 AS part") && Bind(attach.p,1,Utf8(root/L"indexes"/L"lookup.sqlite")) && sqlite3_step(attach.p)==SQLITE_DONE;}
  if(!ok)break;
  ok=Exec(db.p,"BEGIN;");
  {
   Statement q;ok=ok && Prepare(db.p,q,"INSERT OR IGNORE INTO routes SELECT substr(match,1,3),?1 FROM part.lookup UNION SELECT token,?1 FROM part.keywords");
   if(ok)ok=Bind(q.p,1,Utf8(root.parent_path().filename())) && sqlite3_step(q.p)==SQLITE_DONE;
  }
  {
   Statement q;ok=ok && Prepare(db.p,q,"INSERT INTO packages VALUES(?1,?2)");
   if(ok)ok=Bind(q.p,1,Utf8(root.parent_path().filename())) && Bind(q.p,2,Utf8(root.filename())) && sqlite3_step(q.p)==SQLITE_DONE;
  }
  if(!ok)Exec(db.p,"ROLLBACK;");else ok=Exec(db.p,"COMMIT;");
  ok=Exec(db.p,"DETACH DATABASE part;") && ok;
 }
 }
 if(ok)ok=MoveFileExW(pending.c_str(),destination.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE;
 if(!ok){std::filesystem::remove(pending,error);return tl::unexpected(PackageError::kIo);}return {};
}
std::vector<std::filesystem::path> RoutePackageRoots(const std::vector<std::filesystem::path>& roots,const PredictionContext& context){
 if(roots.empty())return {};Db db;if(!Open(db,roots))return roots;
 const auto key=Canonical(context.committed_text);const auto b=Boundaries(key);const auto n=b.size()-1;
 std::unordered_set<std::string> tokens,ids;
 if(n>=2)tokens.insert(key.substr(b[n-2]));
 for(size_t i=(n>64?n-64:0);i+2<n;++i)tokens.insert(key.substr(b[i],b[i+3]-b[i]));
 for(size_t i=(n>256?n-256:0);i<n;++i)for(size_t len=2;len<=6 && i+len<=n;++len)tokens.insert(key.substr(b[i],b[i+len]-b[i]));
 Statement q;if(!Prepare(db.p,q,"SELECT id FROM routes WHERE token=?1"))return roots;
 for(const auto& token:tokens){sqlite3_reset(q.p);Bind(q.p,1,token);int rc=SQLITE_OK;while((rc=sqlite3_step(q.p))==SQLITE_ROW){const auto* s=sqlite3_column_text(q.p,0);if(s)ids.insert(reinterpret_cast<const char*>(s));}if(rc!=SQLITE_DONE)return roots;}
 // Existing route files store three-character prefixes. A range over the final
 // two characters finds their owning packages without rebuilding the corpus.
 if(n>=2){
  const auto suffix=key.substr(b[n-2]);Statement prefix;
  if(!Prepare(db.p,prefix,"SELECT id FROM routes WHERE token>=?1 AND token<?2"))return roots;
  Bind(prefix.p,1,suffix);Bind(prefix.p,2,suffix+"\xF4\x8F\xBF\xBF");
  int rc=SQLITE_OK;while((rc=sqlite3_step(prefix.p))==SQLITE_ROW){const auto* s=sqlite3_column_text(prefix.p,0);if(s)ids.insert(reinterpret_cast<const char*>(s));}if(rc!=SQLITE_DONE)return roots;
 }
 std::vector<std::filesystem::path> result;for(const auto& root:roots)if(ids.contains(Utf8(root.parent_path().filename())))result.push_back(root);return result;
}
}
