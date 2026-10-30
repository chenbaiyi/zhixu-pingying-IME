#include "common/settings/suggestion_policy.h"
#include "common/ipc/named_pipe.h"
#include "common/paths/runtime_paths.h"
#include "common/settings/settings_store.h"
#include "common/protocol/protocol_handshake.h"
#include "predict/imepack/package_store.h"
#include "predict/imepack/routing_index.h"
#include <mutex>
#include <condition_variable>
#include <thread>
#include <optional>
#include "predict/personalization/personalization_store.h"
#include "predict/prediction/prediction_coordinator.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <vector>

namespace {

zhixu::protocol::Envelope Error(const zhixu::protocol::Envelope& request) {
  zhixu::protocol::Envelope response;
  response.set_protocol_version(zhixu::protocol::kProtocolVersion);
  response.set_session_id(request.session_id());
  response.set_request_id(request.request_id());
  response.set_context_version(request.context_version());
  return response;
}

std::vector<std::filesystem::path> EnabledPackageRoots(
    zhixu::predict::imepack::PackageStore& package_store,
    const zhixu::protocol::v1::PredictionRequest& request) {
  const auto active_roots = package_store.ActivePackageRoots();
  if (!active_roots) return {};
  if (request.enabled_package_ids().empty()) return *active_roots;

  std::vector<std::filesystem::path> enabled;
  for (const auto& root : *active_roots) {
    const auto package_id = root.parent_path().filename().string();
    if (std::find(request.enabled_package_ids().begin(), request.enabled_package_ids().end(), package_id) !=
        request.enabled_package_ids().end()) {
      enabled.push_back(root);
    }
  }
  return enabled;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  std::filesystem::path test_root;
  std::wstring pipe_role=L"predict-service";
  for(int i=1;i<argc;i+=2){
    if(i+1>=argc)return 2;
    const std::wstring flag=argv[i];
    if(flag==L"--data-root")test_root=argv[i+1];
    else if(flag==L"--pipe-role")pipe_role=argv[i+1];
    else return 2;
  }
  auto paths = zhixu::paths::RuntimePaths::ForCurrentUser();
  if (!paths) return 1;
  if(!test_root.empty()) *paths={.root=test_root,.settings=test_root/L"settings.json",.user_data=test_root/L"user-data",.packages=test_root/L"packages",.diagnostics=test_root/L"diagnostics"};
  zhixu::predict::PersonalizationStore personalization(paths->user_data / L"learning.db", true);
  zhixu::predict::imepack::PackageStore package_store(paths->packages);
  std::mutex routing_mutex;std::condition_variable routing_condition;
  std::optional<std::vector<std::filesystem::path>> pending_roots;
  std::vector<std::filesystem::path> last_roots;
  std::jthread routing_worker([&](std::stop_token stop){
    std::stop_callback wake(stop,[&]{routing_condition.notify_all();});
    std::unique_lock lock(routing_mutex);
    while(!stop.stop_requested()){
      routing_condition.wait(lock,[&]{return stop.stop_requested() || pending_roots.has_value();});
      if(stop.stop_requested())return;
      auto roots=std::move(*pending_roots);pending_roots.reset();lock.unlock();
      zhixu::predict::imepack::BuildRoutingIndex(roots);lock.lock();
    }
  });
  zhixu::protocol::ProtocolHandshake handshake;
  const auto endpoint = zhixu::ipc::PipeEndpoint::ForCurrentUser(pipe_role);
  if (!endpoint) return 1;
  for (;;) {
    zhixu::ipc::NamedPipeServer server;
    const auto served = server.Listen(*endpoint, [&](const zhixu::protocol::Envelope& request) {
    if (request.protocol_version() != zhixu::protocol::kProtocolVersion) return Error(request);
    if (request.message_type() == zhixu::protocol::v1::HELLO) {
      handshake.Reset();
      zhixu::protocol::v1::Hello hello;
      if (!hello.ParseFromString(request.payload())) return Error(request);
      const auto accepted = handshake.Accept(hello);
      if (!accepted) return Error(request);
      zhixu::protocol::Envelope response;
      response.set_protocol_version(zhixu::protocol::kProtocolVersion);
      response.set_message_type(zhixu::protocol::v1::HELLO_ACK);
      response.set_session_id(request.session_id());
      response.set_request_id(request.request_id());
      response.set_context_version(request.context_version());
      accepted->SerializeToString(response.mutable_payload());
      return response;
    }
    if (handshake.Validate(request.message_type()) && request.message_type()==zhixu::protocol::v1::LEARNING_OBSERVATION) {
      zhixu::protocol::v1::LearningObservation event;
      const auto settings=zhixu::settings::SettingsStore(paths->settings).Load();
      bool ok=event.ParseFromString(request.payload()) && event.privacy().allow_learning() && settings && settings->learning_enabled && !settings->privacy_mode;
      if(ok) ok=!event.policy_generation().empty() && event.policy_generation()==zhixu::settings::SuggestionPolicy(paths->settings,paths->packages);
      if(ok) ok=event.committed_text().size()<=4096 && event.context_before().size()<=4096 && event.identity().size()<=512 &&
        personalization.ObserveOnce(event.identity(),{.text=event.committed_text(),.context=event.context_before()});
      auto response=Error(request);response.set_message_type(zhixu::protocol::v1::CONTROL_RESULT);
      zhixu::protocol::v1::ControlResult result;result.set_error_code(ok?0:1);result.SerializeToString(response.mutable_payload());return response;
    }
    if (!handshake.Validate(request.message_type()) ||
        request.message_type() != zhixu::protocol::v1::PREDICTION_REQUEST) return Error(request);
    zhixu::protocol::v1::PredictionRequest prediction_request;
    if (!prediction_request.ParseFromString(request.payload())) return Error(request);
    zhixu::protocol::v1::PredictionResult result;
    result.set_final_result(true);
    result.set_policy_generation(zhixu::settings::SuggestionPolicy(paths->settings,paths->packages));
    const auto current_settings=zhixu::settings::SettingsStore(paths->settings).Load();
    if (!prediction_request.policy_generation().empty() && prediction_request.policy_generation()==result.policy_generation() && prediction_request.privacy().allow_prediction() && current_settings && current_settings->prediction_enabled && !current_settings->privacy_mode) {
      const auto roots=EnabledPackageRoots(package_store,prediction_request);
      if(roots!=last_roots){
        last_roots=roots;
        if(!zhixu::predict::imepack::RoutingIndexMatches(roots)){
          {std::lock_guard lock(routing_mutex);pending_roots=roots;}routing_condition.notify_one();
        }
      }
      zhixu::predict::PredictionCoordinator coordinator(roots, personalization);
      const auto suggestions = coordinator.Predict({
          .committed_text = prediction_request.context_text(),
          .allow_suggestions = true,
      }, std::min<std::size_t>(prediction_request.candidate_limit(), 30));
      for (const auto& suggestion : suggestions) {
        auto* candidate = result.add_candidates();
        candidate->set_text(suggestion.text);
        candidate->set_confidence(suggestion.source_weight);
        candidate->set_source(suggestion.source_title);
        candidate->set_author(suggestion.author);
        candidate->set_chapter(suggestion.source_context);
        candidate->set_package_id(suggestion.package_id);
        candidate->set_entry_id(suggestion.entry_id);
        switch (suggestion.kind) {
          case zhixu::predict::LocalSuggestionKind::kExactContinuation:
            candidate->set_kind(zhixu::protocol::v1::EXACT_CONTINUATION); break;
          case zhixu::predict::LocalSuggestionKind::kQuotation:
            candidate->set_kind(zhixu::protocol::v1::QUOTATION); break;
          case zhixu::predict::LocalSuggestionKind::kUserNGram:
            candidate->set_kind(zhixu::protocol::v1::USER_NGRAM); break;
          case zhixu::predict::LocalSuggestionKind::kBaseNGram:
            candidate->set_kind(zhixu::protocol::v1::BASE_NGRAM); break;
          case zhixu::predict::LocalSuggestionKind::kModelGenerated:
            candidate->set_kind(zhixu::protocol::v1::MODEL_GENERATED); break;
        }
      }
    }
    zhixu::protocol::Envelope response;
    response.set_protocol_version(zhixu::protocol::kProtocolVersion);
    response.set_message_type(zhixu::protocol::v1::PREDICTION_RESULT);
    response.set_session_id(request.session_id());
    response.set_request_id(request.request_id());
    response.set_context_version(request.context_version());
    result.SerializeToString(response.mutable_payload());
    return response;
    }, {});
    if (!served) return 1;
  }
}
