#include "predict/personalization/personalization_store.h"

#include <chrono>
#include <filesystem>

#include <gtest/gtest.h>

namespace zhixu::predict {
namespace {

class TemporaryDirectory final {
 public:
  TemporaryDirectory()
      : path_(std::filesystem::temp_directory_path() /
              (L"zhixu-personalization-" +
               std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()))) {}
  ~TemporaryDirectory() { std::filesystem::remove_all(path_); }
  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
};

TEST(PersonalizationStore, ObservesForgetsAndClearsLocalFrequency) {
  TemporaryDirectory temporary;
  PersonalizationStore store(temporary.path() / L"learning.db", true);

  ASSERT_TRUE(store.Observe({.text = "法则"}));
  ASSERT_TRUE(store.Observe({.text = "法则"}));
  EXPECT_EQ(store.Frequency("法则"), 2);

  ASSERT_TRUE(store.Observe({.text = "非常道", .context = "道可道"}));
  ASSERT_TRUE(store.Observe({.text = "非常道", .context = "道可道"}));
  ASSERT_TRUE(store.Observe({.text = "道非道", .context = "道可道"}));
  EXPECT_EQ(store.NGramFrequency("道可道", "非常道"), 2);
  const auto ngrams = store.RecallNGrams("道可道", 3);
  ASSERT_EQ(ngrams.size(), 2u);
  EXPECT_EQ(ngrams.front().completion, "非常道");
  EXPECT_EQ(ngrams.front().frequency, 2);

  ASSERT_TRUE(store.Block("机密"));
  EXPECT_TRUE(store.IsBlocked("机密"));
  EXPECT_EQ(store.BlockedSuggestions(), (std::vector<std::string>{"机密"}));
  ASSERT_TRUE(store.Unblock("机密"));
  EXPECT_FALSE(store.IsBlocked("机密"));

  ASSERT_TRUE(store.Forget("法则"));
  EXPECT_EQ(store.Frequency("法则"), 0);
  ASSERT_TRUE(store.Observe({.text = "法则"}));
  ASSERT_TRUE(store.Clear());
  EXPECT_EQ(store.Frequency("法则"), 0);
  EXPECT_EQ(store.NGramFrequency("道可道", "非常道"), 0);
  EXPECT_TRUE(store.RecallNGrams("道可道", 3).empty());
}

TEST(PersonalizationStore, DisabledLearningDoesNotCreateDatabase) {
  TemporaryDirectory temporary;
  const auto database_path = temporary.path() / L"learning.db";
  PersonalizationStore store(database_path, false);

  ASSERT_TRUE(store.Observe({.text = "法则"}));
  ASSERT_TRUE(store.Block("机密"));
  EXPECT_EQ(store.Frequency("法则"), 0);
  EXPECT_EQ(store.NGramFrequency("道可道", "非常道"), 0);
  EXPECT_TRUE(store.RecallNGrams("道可道", 3).empty());
  EXPECT_FALSE(store.IsBlocked("机密"));
  EXPECT_TRUE(store.BlockedSuggestions().empty());
  EXPECT_FALSE(std::filesystem::exists(database_path));
}

TEST(PersonalizationStore, ConfirmedCommitIdentityIsDurablyDeduplicated) {
 TemporaryDirectory temporary;
 const auto path=temporary.path()/L"learning.db";
 {
  PersonalizationStore store(path,true);
  ASSERT_TRUE(store.ObserveOnce("session:7", {.text="黯淡无光",.context="昏暗"}));
  ASSERT_TRUE(store.ObserveOnce("session:7", {.text="黯淡无光",.context="昏暗"}));
  EXPECT_EQ(store.Frequency("黯淡无光"),1);
 }
 PersonalizationStore reopened(path,true);
 ASSERT_TRUE(reopened.ObserveOnce("session:7", {.text="黯淡无光",.context="昏暗"}));
 EXPECT_EQ(reopened.NGramFrequency("昏暗","黯淡无光"),1);
}

}  // namespace
}  // namespace zhixu::predict

namespace zhixu::predict { namespace {
TEST(PersonalizationStore, UnicodeDatabasePathPersistsAndReopensLearning) {
  TemporaryDirectory temporary;const auto file=temporary.path()/L"中文资料"/L"学习.db";
  { PersonalizationStore store(file,true); ASSERT_TRUE(store.ObserveOnce("unicode-event",{.text="法则"}));EXPECT_EQ(store.Frequency("法则"),1); }
  EXPECT_TRUE(std::filesystem::exists(file));
  { PersonalizationStore store(file,true);EXPECT_EQ(store.Frequency("法则"),1); }
}
}}
