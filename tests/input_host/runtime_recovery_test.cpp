#include "input_host/session/session_controller.h"
#include "tsf/text_service.h"
#include <Windows.h>
#include <iostream>
#include <sstream>
#include <string>
class EmptyEngine final : public zhixu::input_host::InputEngine {
 public:
  zhixu::input_host::EngineState ProcessKey(const zhixu::input_host::EngineKey&) override { return {}; }
  zhixu::input_host::CommitOrState SelectCandidate(std::size_t) override { return {}; }
};
std::uint64_t FirstId() {
  EmptyEngine engine; zhixu::input_host::SessionController backend(engine);
  zhixu::protocol::v1::PrivacyDecision privacy;
  const auto action=backend.AcceptRecommendation("public",privacy,0);
  return action ? action->commit_request().request_id() : 0;
}
bool ChildId(std::uint64_t& id) {
  wchar_t image[32768]{};if(!GetModuleFileNameW(nullptr,image,32768))return false;
  SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};HANDLE read=nullptr,write=nullptr;
  if(!CreatePipe(&read,&write,&sa,0))return false;
  SetHandleInformation(read,HANDLE_FLAG_INHERIT,0);
  STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESTDHANDLES;
  startup.hStdOutput=write;startup.hStdError=write;
  PROCESS_INFORMATION process{};std::wstring command=L"\""+std::wstring(image)+L"\" --emit";
  const bool created=CreateProcessW(image,command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process)!=FALSE;
  CloseHandle(write);if(!created){CloseHandle(read);return false;}
  const DWORD waited=WaitForSingleObject(process.hProcess,10000);
  if(waited!=WAIT_OBJECT_0){TerminateProcess(process.hProcess,ERROR_TIMEOUT);WaitForSingleObject(process.hProcess,1000);}
  DWORD code=1;GetExitCodeProcess(process.hProcess,&code);CloseHandle(process.hThread);CloseHandle(process.hProcess);
  char buffer[128]{};DWORD bytes=0;const bool got=ReadFile(read,buffer,sizeof(buffer)-1,&bytes,nullptr)!=FALSE;CloseHandle(read);
  if(waited!=WAIT_OBJECT_0 || code || !got)return false;
  return bool(std::istringstream(std::string(buffer,bytes))>>id) && id!=0;
}
int main(int argc,char** argv) {
  if(argc==2 && std::string(argv[1])=="--emit"){std::cout<<FirstId()<<std::endl;return 0;}
  using namespace zhixu::tsf;TextService retained_application;
  std::uint64_t previous=0;
  for(unsigned i=0;i<4;++i){
    std::uint64_t id=0;if(!ChildId(id))return 2;
    const auto version=retained_application.context_version();
    if(id==previous || retained_application.ApplyCommit({id,version},false).kind!=CommitOutcomeKind::Rejected)return 3;
    const auto applied=retained_application.ApplyCommit({id,version},true);
    std::cout<<"backend="<<i<<" id="<<id<<" applied="<<(applied.kind==CommitOutcomeKind::Applied)<<std::endl;
    if(applied.kind!=CommitOutcomeKind::Applied)return 4;
    if(retained_application.ApplyCommit({id,version},true).kind!=CommitOutcomeKind::Rejected ||
       retained_application.ApplyCommit({id,retained_application.context_version()},true).kind!=CommitOutcomeKind::Rejected)return 5;
    previous=id;
  }
  std::cout<<"RESTART_IDENTITY_OK four-processes retained-TSF duplicate-and-failed-rejected"<<std::endl;
}
