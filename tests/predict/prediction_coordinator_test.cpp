#include "predict/personalization/personalization_store.h"
#include "predict/prediction/prediction_coordinator.h"

#include <chrono>
#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>

namespace zhixu::predict {
namespace {

class TemporaryDirectory final {
 public:
  TemporaryDirectory()
      : path_(std::filesystem::temp_directory_path() /
              (L"zhixu-coordinator-" +
               std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()))) {}
  ~TemporaryDirectory() { std::filesystem::remove_all(path_); }
  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
};

TEST(PredictionCoordinator, RanksExactPackageContinuationBeforeLocalNGramAndAppliesBlocks) {
  TemporaryDirectory temporary;
  const auto indexes = temporary.path() / L"package" / L"indexes";
  std::filesystem::create_directories(indexes);
  std::ofstream continuation(indexes / L"continuation.jsonl", std::ios::binary);
  continuation << R"({"id":"acceptance.daoyun","normalized_trigger":"道韵","text":"法则","weight":1.0,"tags":[]})"
               << '\n';
  continuation.close();

  PersonalizationStore personalization(temporary.path() / L"learning.db", true);
  ASSERT_TRUE(personalization.Observe({.text = "悠长", .context = "道韵"}));
  ASSERT_TRUE(personalization.Block("悠长"));

  PredictionCoordinator coordinator({temporary.path() / L"package"}, personalization);
  const auto ranked = coordinator.Predict({.committed_text = "道韵"}, 3);

  ASSERT_EQ(ranked.size(), 1u);
  EXPECT_EQ(ranked.front().text, "法则");
  EXPECT_EQ(ranked.front().kind, LocalSuggestionKind::kExactContinuation);
}

}  // namespace
}  // namespace zhixu::predict
