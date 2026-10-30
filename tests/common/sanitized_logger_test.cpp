#include <gtest/gtest.h>
#include <string>
#include <type_traits>
#include "common/diagnostics/sanitized_logger.h"
using namespace zhixu::diagnostics;
static_assert(!std::is_invocable_v<decltype(LogEvent), EventId, std::string, HRESULT>);
static_assert(!std::is_constructible_v<NumericField, FieldId, const char*>);
TEST(SanitizedLogger, PreservesNumericEvidenceWithBoundedRetention) {
  SanitizedLogger logger(2); logger.Record(EventId::ChildExited, {}, E_FAIL);
  std::array fields{NumericField{FieldId::DurationMicros, 120}};
  logger.Record(EventId::ProtocolRejected, fields, E_INVALIDARG);
  logger.Record(EventId::PipeDisconnected, {}, S_OK);
  auto events = logger.Snapshot(); ASSERT_EQ(events.size(), 2);
  EXPECT_EQ(events[0].id, EventId::ProtocolRejected); EXPECT_EQ(events[0].result, E_INVALIDARG);
  ASSERT_EQ(events[0].field_count, 1); EXPECT_EQ(events[0].fields[0].value, 120);
  EXPECT_EQ(events[1].id, EventId::PipeDisconnected);
}
TEST(SanitizedLogger, BoundsFieldsAndSupportsDisabledRetention) {
  SanitizedLogger logger; std::array<NumericField, 20> fields{};
  logger.Record(EventId::ProtocolRejected, fields);
  auto events = logger.Snapshot(); ASSERT_EQ(events.size(), 1); EXPECT_EQ(events[0].field_count, 8);
  SanitizedLogger disabled(0); disabled.Record(EventId::ChildExited, {}); EXPECT_TRUE(disabled.Snapshot().empty());
}
