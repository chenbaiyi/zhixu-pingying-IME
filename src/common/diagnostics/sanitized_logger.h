#pragma once
#include <Windows.h>
#include <array>
#include <cstdint>
#include <deque>
#include <mutex>
#include <span>
#include <vector>
namespace zhixu::diagnostics {
enum class EventId : uint32_t { ProtocolRejected = 1, PipeDisconnected, ChildExited, ModelUnavailable };
enum class FieldId : uint32_t { DurationMicros = 1, ProtocolVersion, Count, ProcessRole };
struct NumericField { FieldId id; uint64_t value; };
struct Event { EventId id; std::array<NumericField, 8> fields{}; size_t field_count = 0; HRESULT result = S_OK; };
class SanitizedLogger {
 public:
  explicit SanitizedLogger(size_t capacity = 256) : capacity_(capacity) {}
  void Record(EventId, std::span<const NumericField>, HRESULT = S_OK);
  std::vector<Event> Snapshot() const;
 private:
  size_t capacity_;
  mutable std::mutex mutex_;
  std::deque<Event> events_;
};
void LogEvent(EventId, std::span<const NumericField>, HRESULT = S_OK);
}
