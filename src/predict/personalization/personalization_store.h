#pragma once

#include <filesystem>
#include <string>
#include <vector>

struct sqlite3;

namespace zhixu::predict {

struct CommitObservation {
  std::string text;
  std::string context;
};

struct PersonalizedNGram {
  std::string completion;
  int frequency = 0;
};

class PersonalizationStore final {
 public:
  PersonalizationStore(std::filesystem::path database_path, bool allow_learning);
  ~PersonalizationStore();

  PersonalizationStore(const PersonalizationStore&) = delete;
  PersonalizationStore& operator=(const PersonalizationStore&) = delete;

  [[nodiscard]] bool Observe(const CommitObservation& observation);
  [[nodiscard]] bool ObserveOnce(const std::string& identity, const CommitObservation& observation);
  [[nodiscard]] bool Forget(const std::string& text);
  [[nodiscard]] bool Block(const std::string& text);
  [[nodiscard]] bool Unblock(const std::string& text);
  [[nodiscard]] bool Clear();
  [[nodiscard]] int Frequency(const std::string& text);
  [[nodiscard]] int NGramFrequency(const std::string& context, const std::string& completion);
  [[nodiscard]] std::vector<PersonalizedNGram> RecallNGrams(const std::string& context, size_t limit);
  [[nodiscard]] bool IsBlocked(const std::string& text);
  [[nodiscard]] std::vector<std::string> BlockedSuggestions();

 private:
  [[nodiscard]] bool Open();

  std::filesystem::path database_path_;
  bool allow_learning_ = false;
  sqlite3* database_ = nullptr;
};

}  // namespace zhixu::predict
