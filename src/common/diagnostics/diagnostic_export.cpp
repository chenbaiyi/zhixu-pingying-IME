#include "common/diagnostics/diagnostic_export.h"

#include <algorithm>
#include <fstream>

#include <nlohmann/json.hpp>

namespace zhixu::diagnostics {

tl::expected<void, DiagnosticExportError> ExportDiagnostics(
    const std::filesystem::path& output, const std::span<const Event> events) {
  nlohmann::json document;
  document["events"] = nlohmann::json::array();
  for (const Event& event : events) {
    nlohmann::json exported_event{{"event_id", static_cast<std::uint32_t>(event.id)},
                                  {"hresult", static_cast<std::int32_t>(event.result)},
                                  {"fields", nlohmann::json::array()}};
    const size_t field_count = std::min(event.field_count, event.fields.size());
    for (size_t index = 0; index < field_count; ++index) {
      const NumericField& field = event.fields[index];
      exported_event["fields"].push_back({{"field_id", static_cast<std::uint32_t>(field.id)},
                                           {"value", field.value}});
    }
    document["events"].push_back(std::move(exported_event));
  }

  std::error_code error;
  std::filesystem::create_directories(output.parent_path(), error);
  if (error) {
    return tl::unexpected(DiagnosticExportError::Io);
  }
  std::ofstream file(output, std::ios::binary | std::ios::trunc);
  file << document.dump(2) << '\n';
  if (!file) {
    return tl::unexpected(DiagnosticExportError::Io);
  }
  return {};
}

}  // namespace zhixu::diagnostics
