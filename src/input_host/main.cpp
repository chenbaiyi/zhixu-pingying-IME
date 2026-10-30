#include "input_host/engine/rime_engine.h"
#include "input_host/input_host_server.h"
#include "input_host/prediction/prediction_runtime.h"
#include "input_host/prediction/prediction_service_client.h"
#include "input_host/ui/candidate_window_host.h"
#include "input_host/ui/recommendation_window_host.h"

#include "common/ipc/named_pipe.h"
#include "common/paths/runtime_paths.h"

#include <filesystem>
#include <iterator>
#include <memory>
#include <system_error>
#include <utility>

#include <Windows.h>
#include "common/settings/settings_store.h"
#include "common/settings/suggestion_policy.h"

namespace zhixu::input_host {
namespace {

std::filesystem::path ModuleDirectory() {
  wchar_t path[MAX_PATH]{};
  const auto size = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
  if (size == 0 || size == std::size(path)) {
    return {};
  }
  return std::filesystem::path(path).parent_path();
}

}  // namespace
}  // namespace zhixu::input_host

int main() {
  const auto runtime_paths = zhixu::paths::RuntimePaths::ForCurrentUser();
  if (!runtime_paths) {
    return 1;
  }
  std::error_code error;
  std::filesystem::create_directories(runtime_paths->user_data, error);
  const auto module_directory = zhixu::input_host::ModuleDirectory();
  if (error || module_directory.empty()) {
    return 1;
  }
  zhixu::input_host::RimeEngine engine(
      {.shared_data_directory = module_directory / L"rime",
       .user_data_directory = runtime_paths->user_data,
       .schema_id = "zhixu_pinyin"});
  if (!engine.available()) {
    return 1;
  }
  zhixu::input_host::ui::CandidateWindowHost candidate_window;
  candidate_window.Start();
  zhixu::input_host::ui::RecommendationWindowHost recommendation_window;
  recommendation_window.Start();
  const auto prediction_endpoint = zhixu::ipc::PipeEndpoint::ForCurrentUser(L"predict-service");
  if (!prediction_endpoint) {
    return 1;
  }
  zhixu::input_host::PredictionServiceClient prediction_client(*prediction_endpoint);
  const auto learning_epoch=std::to_string(GetCurrentProcessId())+":"+std::to_string(GetTickCount64())+":";
  zhixu::input_host::LearningDispatcher learning_dispatcher([&prediction_client,&runtime_paths](const zhixu::input_host::ConfirmedCommit& commit) {
    const auto settings=zhixu::settings::SettingsStore(runtime_paths->settings).Load();
    if(!settings || settings->privacy_mode || !settings->learning_enabled || commit.policy_generation.empty() || commit.policy_generation != zhixu::settings::SuggestionPolicy(runtime_paths->settings,runtime_paths->packages))return true;
    return prediction_client.Observe(commit);
  });
  std::unique_ptr<zhixu::input_host::PredictionRuntime> prediction_runtime;
  zhixu::input_host::InputHostServer server(
      engine, &candidate_window,
      [&prediction_runtime, &learning_dispatcher, &learning_epoch](zhixu::input_host::ConfirmedCommit commit) {
        commit.session_id=learning_epoch+commit.session_id;
        learning_dispatcher.Submit(commit);
        if (prediction_runtime) prediction_runtime->Submit(std::move(commit));
      },
      [&prediction_runtime] {
        if (prediction_runtime) prediction_runtime->Invalidate();
      },
      &recommendation_window,false,[&runtime_paths]{return zhixu::settings::SuggestionPolicy(runtime_paths->settings,runtime_paths->packages);});
  prediction_runtime = std::make_unique<zhixu::input_host::PredictionRuntime>(
      [&prediction_client,&runtime_paths](const zhixu::input_host::ConfirmedCommit& commit) {
        if(commit.policy_generation.empty() || commit.policy_generation != zhixu::settings::SuggestionPolicy(runtime_paths->settings,runtime_paths->packages)) return std::optional<zhixu::protocol::v1::PredictionResult>{};
        return prediction_client.Predict(commit);
      },
      [&server](zhixu::protocol::v1::PredictionResult result, const std::uint64_t context_version) {
        server.OnPredictionResult(std::move(result), context_version);
      });
  server.SetSettingsProvider([&] {
    const auto loaded=zhixu::settings::SettingsStore(runtime_paths->settings).Load();
    const auto current=loaded ? *loaded : zhixu::settings::Settings::SafeFallback();
    candidate_window.SetTheme(static_cast<int>(current.theme));
    recommendation_window.SetTheme(static_cast<int>(current.theme));
    return current;
  });
  const auto result = server.Serve(L"tsf-input", {}, [](const DWORD client_pid) {
    DWORD foreground_pid = 0;
    const HWND foreground = GetForegroundWindow();
    if (foreground != nullptr) GetWindowThreadProcessId(foreground, &foreground_pid);
    return foreground_pid != 0 && foreground_pid == client_pid;
  });
  prediction_runtime->Invalidate();
  prediction_runtime.reset();
  candidate_window.Stop();
  recommendation_window.Stop();
  return result ? 0 : 1;
}
