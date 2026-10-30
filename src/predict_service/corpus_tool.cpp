#include "common/ipc/named_pipe.h"
#include "common/protocol/protocol_handshake.h"
#include "predict/imepack/package_store.h"
#include "predict/imepack/routing_index.h"
#include "predict/imepack/package_validator.h"
#include "predict/prediction/prediction_coordinator.h"
#include "common/paths/runtime_paths.h"
#include <windows.h>
#include <psapi.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
using Json=nlohmann::json;
std::string Utf8(const std::wstring& s){auto n=WideCharToMultiByte(CP_UTF8,0,s.data(),static_cast<int>(s.size()),nullptr,0,nullptr,nullptr);std::string v(n,'\0');WideCharToMultiByte(CP_UTF8,0,s.data(),static_cast<int>(s.size()),v.data(),n,nullptr,nullptr);return v;}
int wmain(int argc,wchar_t** argv) {
 if(argc<3)return 2;
 try{
  const std::wstring mode=argv[1];
  if(mode==L"control" || mode==L"service") {
   using namespace zhixu;const auto endpoint=ipc::PipeEndpoint::ForCurrentUser(mode==L"control"?L"broker-control":L"predict-service");
   if(!endpoint)return 3;ipc::NamedPipeClient pipe;if(!pipe.Connect(*endpoint,std::chrono::seconds(10)))return 4;
   protocol::Envelope request;request.set_protocol_version(protocol::kProtocolVersion);request.set_session_id("corpus-tool-"+std::to_string(GetCurrentProcessId()));request.set_request_id(1);request.set_message_type(protocol::v1::HELLO);
   protocol::v1::Hello hello;hello.set_min_version(1);hello.set_max_version(1);hello.SerializeToString(request.mutable_payload());
   auto response=pipe.Call(request,std::chrono::seconds(10));if(!response || response->message_type()!=protocol::v1::HELLO_ACK)return 4;
   request.set_request_id(2);
   if(mode==L"control"){
    protocol::v1::ControlCommand command;const std::wstring op=argv[2];
    if(op==L"status")command.set_operation(protocol::v1::GET_RUNTIME_STATUS);
    else if(op==L"enable")command.set_operation(protocol::v1::ENABLE_PACKAGE);
    else if(op==L"disable")command.set_operation(protocol::v1::DISABLE_PACKAGE);
    else if(op==L"remove")command.set_operation(protocol::v1::REMOVE_PACKAGE);
    else if(op==L"install")command.set_operation(protocol::v1::INSTALL_PACKAGE);
    else return 2;
    if(argc==4)command.set_arguments(Utf8(argv[3]));request.set_message_type(protocol::v1::CONTROL_COMMAND);command.SerializeToString(request.mutable_payload());
    response=pipe.Call(request,op==L"install"?std::chrono::hours(1):std::chrono::seconds(15));
    protocol::v1::ControlResult result;if(!response || !result.ParseFromString(response->payload()) || result.error_code()){std::cerr<<"BROKER_CONTROL_FAILED\n";return 1;}
    std::cout<<(result.data().empty()?"{}":result.data());return 0;
   }
   protocol::v1::PredictionRequest prediction;prediction.set_context_text(Utf8(argv[2]));prediction.set_candidate_limit(3);prediction.mutable_privacy()->set_allow_prediction(true);
   request.set_message_type(protocol::v1::PREDICTION_REQUEST);prediction.SerializeToString(request.mutable_payload());
   response=pipe.Call(request,std::chrono::seconds(2));protocol::v1::PredictionResult result;if(!response || !result.ParseFromString(response->payload()))return 1;
   Json out=Json::array();for(const auto& c:result.candidates())out.push_back(Json{{"text",c.text()},{"source",c.source()},{"package",c.package_id()}});std::cout<<out.dump();return 0;
  }
  if(mode==L"install" && argc==4){
   const std::filesystem::path source=argv[2],target=argv[3];
   zhixu::predict::imepack::PackageStore store(target);std::vector<std::filesystem::path> inputs;
   for(const auto& e:std::filesystem::directory_iterator(source))if(e.is_regular_file() && e.path().extension()==L".imepack")inputs.push_back(e.path());
   std::sort(inputs.begin(),inputs.end());if(inputs.empty())return 3;
   std::map<std::filesystem::path,std::string> previous;
   if(std::filesystem::exists(target))for(const auto& e:std::filesystem::directory_iterator(target))if(e.is_directory()){
    const auto active=e.path()/L"active.json";if(std::filesystem::exists(active)){std::ifstream f(active);previous[active]={std::istreambuf_iterator<char>(f),{}};}
   }
   const auto scratch=std::filesystem::temp_directory_path()/(L"ZhixuIME-corpus-install-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));if(!std::filesystem::create_directory(scratch))return 3;
   std::vector<std::string> activated;bool ok=true;
   for(const auto& path:inputs){
    const auto snapshot=scratch/L"input.imepack";std::filesystem::copy_file(path,snapshot,std::filesystem::copy_options::overwrite_existing);
    const auto valid=zhixu::predict::imepack::ValidatePackage(snapshot);
    if(!valid){std::cerr<<"VALIDATION_FAILED "<<Utf8(path.filename().wstring())<<" code="<<static_cast<int>(valid.error())<<'\n';ok=false;break;}
    const auto installed=store.InstallPackage(*valid);
    if(!installed && installed.error()!=zhixu::predict::imepack::PackageError::kAlreadyInstalled){std::cerr<<"INSTALL_FAILED "<<valid->package_id<<" code="<<static_cast<int>(installed.error())<<'\n';ok=false;break;}
    if(!installed && !store.ActivateVersion(valid->package_id,valid->version)){ok=false;break;}
    activated.push_back(valid->package_id);std::cout<<"INSTALLED "<<valid->package_id<<'\n'<<std::flush;
   }
   if(ok){const auto all=store.ActivePackageRoots();ok=all && zhixu::predict::imepack::BuildRoutingIndex(*all).has_value();}
   if(!ok){for(const auto& id:activated){const auto active=target/id/L"active.json";const auto it=previous.find(active);
     if(it==previous.end())store.Disable(id);else{const auto pending=active.wstring()+L".restore";std::ofstream f(pending,std::ios::binary);f<<it->second;f.close();MoveFileExW(pending.c_str(),active.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);}
    }}
   std::filesystem::remove_all(scratch);return ok?0:1;
  }
  zhixu::predict::imepack::PackageStore store{std::filesystem::path(argv[2])};const auto roots=store.ActivePackageRoots();if(!roots)return 3;
  auto ordered=*roots;std::stable_sort(ordered.begin(),ordered.end(),[](const auto& a,const auto& b){return (a.parent_path().filename().string().find("writing")!=std::string::npos)>(b.parent_path().filename().string().find("writing")!=std::string::npos);});
  zhixu::predict::PersonalizationStore learning({},false);zhixu::predict::PredictionCoordinator coordinator(ordered,learning);
  if(mode==L"predict" && argc==4){Json result=Json::array();for(const auto& s:coordinator.Predict({.committed_text=Utf8(argv[3])},3))result.push_back(Json{{"text",s.text},{"work",s.source_title},{"chapter",s.source_context}});std::cout<<result.dump();return 0;}
  if(mode==L"bench" && argc==5){
   std::ifstream input(std::filesystem::path(argv[3]),std::ios::binary);const auto queries=Json::parse(input);std::vector<double> timings;Json results=Json::array();size_t usable=0;
   for(int pass=0;pass<4;++pass)for(const auto& q:queries){
    const auto start=std::chrono::steady_clock::now();const auto suggestions=coordinator.Predict({.committed_text=q.get<std::string>()},3);
    const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    if(pass)timings.push_back(ms);else{Json options=Json::array();for(const auto& s:suggestions)options.push_back(s.text);if(!suggestions.empty())++usable;results.push_back(Json{{"query",q},{"suggestions",options}});}
   }
   std::sort(timings.begin(),timings.end());PROCESS_MEMORY_COUNTERS_EX memory{};GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),sizeof(memory));
   Json report{{"packages",ordered.size()},{"samples",timings.size()},{"retrieval_p50_ms",timings[timings.size()/2]},{"retrieval_p95_ms",timings[(timings.size()*95)/100]},{"total_p95_including_debounce_ms",100+timings[(timings.size()*95)/100]},{"working_set_bytes",memory.WorkingSetSize},{"nonempty_cases",usable},{"results",results}};
   std::ofstream output(std::filesystem::path(argv[4]),std::ios::binary);output<<report.dump(2);std::cout<<"BENCHMARK_SAVED\n";return 0;
  }
  return 2;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
