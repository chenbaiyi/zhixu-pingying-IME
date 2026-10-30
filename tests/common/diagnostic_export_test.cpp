#include <gtest/gtest.h>
#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "common/diagnostics/diagnostic_export.h"

namespace zhixu::diagnostics {
namespace {

TEST(DiagnosticExport, EmitsOnlyStructuredNumericFields) {
  const auto output = std::filesystem::temp_directory_path() / L"ZhixuIME-diagnostics-tests" /
                      (L"events-" + std::to_wstring(GetCurrentProcessId()) + L".json");
  std::filesystem::create_directories(output.parent_path());
  const Event events[] = {{.id = EventId::PipeDisconnected,
                           .fields = {{{.id = FieldId::Count, .value = 7}}},
                           .field_count = 1,
                           .result = E_ACCESSDENIED}};

  const auto result = ExportDiagnostics(output, events);

  ASSERT_TRUE(result);
  std::ifstream file(output, std::ios::binary);
  const std::string document((std::istreambuf_iterator<char>(file)), {});
  EXPECT_NE(document.find("\"event_id\": 2"), std::string::npos);
  EXPECT_NE(document.find("\"field_id\": 3"), std::string::npos);
  EXPECT_NE(document.find("\"value\": 7"), std::string::npos);
  EXPECT_NE(document.find("\"hresult\": -2147024891"), std::string::npos);
  EXPECT_EQ(document.find("canary-user-text"), std::string::npos);
  EXPECT_EQ(document.find("memory_dump"), std::string::npos);
}

}  // namespace
}  // namespace zhixu::diagnostics
