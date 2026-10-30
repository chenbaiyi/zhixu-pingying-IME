#include "predict/prediction/retriever.h"
#include "predict/imepack/disk_index.h"
#include "predict/imepack/routing_index.h"
#include <chrono>

#include <windows.h>

#include <nlohmann/json.hpp>

#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace zhixu::predict {
namespace {

constexpr size_t kMaximumJsonLineBytes = 1024 * 1024;

bool NormalizeNfc(const std::string_view value, std::string* normalized) {
  if (value.size() > static_cast<size_t>(std::numeric_limits<int>::max())) return false;
  const auto wide_length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                               static_cast<int>(value.size()), nullptr, 0);
  if (wide_length == 0 && !value.empty()) return false;
  std::wstring wide(static_cast<size_t>(wide_length), L'\0');
  if (wide_length != 0 &&
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                          wide.data(), wide_length) == 0) return false;
  if (IsNormalizedString(NormalizationC, wide.data(), wide_length)) {
    *normalized = value;
    return true;
  }
  const auto normalized_length = NormalizeString(NormalizationC, wide.data(), wide_length, nullptr, 0);
  if (normalized_length <= 0 && !wide.empty()) return false;
  std::wstring wide_normalized(static_cast<size_t>(normalized_length), L'\0');
  if (normalized_length != 0 &&
      NormalizeString(NormalizationC, wide.data(), wide_length, wide_normalized.data(), normalized_length) <= 0) {
    return false;
  }
  const auto utf8_length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide_normalized.data(),
                                                normalized_length, nullptr, 0, nullptr, nullptr);
  if (utf8_length == 0 && !wide_normalized.empty()) return false;
  normalized->resize(static_cast<size_t>(utf8_length));
  return utf8_length == 0 || WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide_normalized.data(),
                                                   normalized_length, normalized->data(), utf8_length,
                                                   nullptr, nullptr) != 0;
}

bool HasRequiredString(const nlohmann::json& value, const char* key, std::string* output) {
  const auto field = value.find(key);
  if (field == value.end() || !field->is_string()) return false;
  *output = field->get<std::string>();
  return true;
}

bool HasStringArray(const nlohmann::json& value, const char* key, std::vector<std::string>* output) {
  const auto field = value.find(key);
  if (field == value.end() || !field->is_array()) return false;
  for (const auto& item : *field) {
    if (!item.is_string()) return false;
    output->push_back(item.get<std::string>());
  }
  return true;
}

}  // namespace

Retriever::Retriever(std::vector<std::filesystem::path> package_roots)
    : package_roots_(std::move(package_roots)) {}

std::vector<Suggestion> Retriever::Recall(const PredictionContext& context, const size_t limit) const {
  if (!context.allow_suggestions || limit == 0) return {};
  std::string normalized_context;
  if (!NormalizeNfc(context.committed_text, &normalized_context)) return {};

  std::vector<Suggestion> results;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(80);
  const auto roots = imepack::RoutePackageRoots(package_roots_, context);
  // Collect dictionary completions and curated selections across routed volumes
  // before a dense general quotation volume can exhaust the shared deadline.
  for (const auto& root : roots) {
    if (std::chrono::steady_clock::now() >= deadline) break;
    if (!std::filesystem::exists(root / L"indexes" / L"lookup.sqlite")) continue;
    auto priority = imepack::RecallDiskIndex(root, context, 128, deadline, true);
    results.insert(results.end(), std::make_move_iterator(priority.begin()), std::make_move_iterator(priority.end()));
  }
  for (const auto& package_root : roots) {
    if (std::chrono::steady_clock::now() >= deadline) break;
    if (std::filesystem::exists(package_root / L"indexes" / L"lookup.sqlite")) {
      auto recalled = imepack::RecallDiskIndex(package_root, context, 128, deadline);
      results.insert(results.end(), std::make_move_iterator(recalled.begin()), std::make_move_iterator(recalled.end()));
      continue;
    }
    std::ifstream input(package_root / L"indexes" / L"continuation.jsonl", std::ios::binary);
    std::string line;
    if (input) {
      while (std::getline(input, line)) {
        if (line.size() > kMaximumJsonLineBytes) break;
        const auto index_entry = nlohmann::json::parse(line, nullptr, false, false);
        if (index_entry.is_discarded() || !index_entry.is_object()) continue;
        std::string normalized_trigger;
        std::string text;
        std::string entry_id;
        std::vector<std::string> tags;
        if (!HasRequiredString(index_entry, "normalized_trigger", &normalized_trigger) ||
            !HasRequiredString(index_entry, "text", &text) || !HasRequiredString(index_entry, "id", &entry_id) ||
            normalized_trigger.empty() || !normalized_context.ends_with(normalized_trigger)) {
          continue;
        }
        if (index_entry.contains("tags") && !HasStringArray(index_entry, "tags", &tags)) continue;
        const auto weight = index_entry.find("weight");
        if (weight == index_entry.end() || !weight->is_number()) continue;
        results.push_back({
            .text = std::move(text),
            .kind = LocalSuggestionKind::kExactContinuation,
            .source_weight = weight->get<double>(),
            .package_id = package_root.filename().string(),
            .entry_id = std::move(entry_id),
            .tags = std::move(tags),
        });
      }
    }

    std::ifstream quotation_input(package_root / L"indexes" / L"quotation.jsonl", std::ios::binary);
    if (!quotation_input) continue;
    while (std::getline(quotation_input, line)) {
      if (line.size() > kMaximumJsonLineBytes) break;
      const auto index_entry = nlohmann::json::parse(line, nullptr, false, false);
      if (index_entry.is_discarded() || !index_entry.is_object()) continue;
      std::string match;
      std::string text;
      std::string entry_id;
      std::string title;
      std::string author;
      std::string source_context;
      std::vector<std::string> tags;
      if (!HasRequiredString(index_entry, "match", &match) || !HasRequiredString(index_entry, "text", &text) ||
          !HasRequiredString(index_entry, "id", &entry_id) || !HasRequiredString(index_entry, "work", &title) ||
          !HasRequiredString(index_entry, "author", &author) ||
          !HasRequiredString(index_entry, "source_context", &source_context) ||
          !HasStringArray(index_entry, "tags", &tags) || match.empty() ||
          !normalized_context.ends_with(match)) {
        continue;
      }
      const auto weight = index_entry.find("weight");
      if (weight == index_entry.end() || !weight->is_number()) continue;
      results.push_back({
          .text = std::move(text),
          .kind = LocalSuggestionKind::kQuotation,
          .source_weight = weight->get<double>(),
          .package_id = package_root.filename().string(),
          .entry_id = std::move(entry_id),
          .tags = std::move(tags),
          .source_title = std::move(title),
          .author = std::move(author),
          .source_context = std::move(source_context),
      });
    }
  }
  // Rank across packages before applying the presentation limit.
  return results;
}

}  // namespace zhixu::predict
