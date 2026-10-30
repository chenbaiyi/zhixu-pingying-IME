#include "common/diagnostics/sanitized_logger.h"
namespace zhixu::diagnostics {
void SanitizedLogger::Record(EventId id, std::span<const NumericField> fields, HRESULT result) {
  if (capacity_ == 0) {
    return;
  }
  Event event{.id = id, .result = result};
  event.field_count = std::min(fields.size(), event.fields.size());
  std::copy_n(fields.begin(), event.field_count, event.fields.begin());

  std::scoped_lock lock(mutex_);
  if (events_.size() == capacity_) {
    events_.pop_front();
  }
  events_.push_back(event);
}

std::vector<Event> SanitizedLogger::Snapshot() const {
  std::scoped_lock lock(mutex_);
  return {events_.begin(), events_.end()};
}
void LogEvent(EventId id, std::span<const NumericField> fields, HRESULT result) { static SanitizedLogger logger; logger.Record(id, fields, result); }
}
