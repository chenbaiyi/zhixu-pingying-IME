#include "predict/personalization/personalization_store.h"

#include <sqlite3.h>

#include <filesystem>
#include <limits>
#include <string_view>
#include <system_error>
#include <vector>

namespace zhixu::predict {
namespace {

bool Execute(sqlite3* database, const char* statement) {
  char* error_message = nullptr;
  const auto result = sqlite3_exec(database, statement, nullptr, nullptr, &error_message);
  sqlite3_free(error_message);
  return result == SQLITE_OK;
}

bool BindAndStep(sqlite3* database, const char* statement, const std::string_view text) {
  sqlite3_stmt* query = nullptr;
  if (sqlite3_prepare_v2(database, statement, -1, &query, nullptr) != SQLITE_OK) return false;
  const auto finalize = [&query]() { sqlite3_finalize(query); };
  if (sqlite3_bind_text(query, 1, text.data(), static_cast<int>(text.size()), SQLITE_TRANSIENT) != SQLITE_OK) {
    finalize();
    return false;
  }
  const bool succeeded = sqlite3_step(query) == SQLITE_DONE;
  finalize();
  return succeeded;
}

bool BindAndStep(sqlite3* database, const char* statement, const std::string_view first,
                 const std::string_view second) {
  sqlite3_stmt* query = nullptr;
  if (sqlite3_prepare_v2(database, statement, -1, &query, nullptr) != SQLITE_OK) return false;
  const auto finalize = [&query]() { sqlite3_finalize(query); };
  if (sqlite3_bind_text(query, 1, first.data(), static_cast<int>(first.size()), SQLITE_TRANSIENT) != SQLITE_OK ||
      sqlite3_bind_text(query, 2, second.data(), static_cast<int>(second.size()), SQLITE_TRANSIENT) != SQLITE_OK) {
    finalize();
    return false;
  }
  const bool succeeded = sqlite3_step(query) == SQLITE_DONE;
  finalize();
  return succeeded;
}

int QueryInt(sqlite3* database, const char* statement, const std::string_view first,
             const std::string_view second = {}) {
  sqlite3_stmt* query = nullptr;
  if (sqlite3_prepare_v2(database, statement, -1, &query, nullptr) != SQLITE_OK) return 0;
  const auto finalize = [&query]() { sqlite3_finalize(query); };
  if (sqlite3_bind_text(query, 1, first.data(), static_cast<int>(first.size()), SQLITE_TRANSIENT) != SQLITE_OK ||
      (second.data() != nullptr &&
       sqlite3_bind_text(query, 2, second.data(), static_cast<int>(second.size()), SQLITE_TRANSIENT) != SQLITE_OK)) {
    finalize();
    return 0;
  }
  const int result = sqlite3_step(query) == SQLITE_ROW ? sqlite3_column_int(query, 0) : 0;
  finalize();
  return result;
}

}  // namespace

PersonalizationStore::PersonalizationStore(std::filesystem::path database_path, const bool allow_learning)
    : database_path_(std::move(database_path)), allow_learning_(allow_learning) {}

PersonalizationStore::~PersonalizationStore() {
  if (database_ != nullptr) sqlite3_close(database_);
}

bool PersonalizationStore::Open() {
  if (!allow_learning_) return true;
  if (database_ != nullptr) return true;
  std::error_code error;
  std::filesystem::create_directories(database_path_.parent_path(), error);
  const auto utf8_path = database_path_.u8string();
  if (error || sqlite3_open_v2(reinterpret_cast<const char*>(utf8_path.c_str()), &database_,
                               SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
    if (database_ != nullptr) {
      sqlite3_close(database_);
      database_ = nullptr;
    }
    return false;
  }
  return Execute(database_, "PRAGMA journal_mode=WAL;") &&
         Execute(database_, "CREATE TABLE IF NOT EXISTS word_frequency("
                            "text TEXT PRIMARY KEY NOT NULL, frequency INTEGER NOT NULL);") &&
         Execute(database_, "CREATE TABLE IF NOT EXISTS ngram_frequency("
                            "context TEXT NOT NULL, completion TEXT NOT NULL, frequency INTEGER NOT NULL, "
                            "last_selected_at INTEGER NOT NULL, PRIMARY KEY(context, completion));") &&
         Execute(database_, "CREATE TABLE IF NOT EXISTS blocked_suggestion("
                            "text TEXT PRIMARY KEY NOT NULL);") &&
         Execute(database_, "CREATE TABLE IF NOT EXISTS observed_commits(identity TEXT PRIMARY KEY NOT NULL);");
}

bool PersonalizationStore::Observe(const CommitObservation& observation) {
  if (!allow_learning_) return true;
  if (observation.text.empty() || !Open()) return observation.text.empty();
  if (!BindAndStep(database_, "INSERT INTO word_frequency(text, frequency) VALUES(?1, 1) "
                               "ON CONFLICT(text) DO UPDATE SET frequency = frequency + 1;",
                   observation.text)) {
    return false;
  }
  if (observation.context.empty()) return true;
  return BindAndStep(database_,
                     "INSERT INTO ngram_frequency(context, completion, frequency, last_selected_at) "
                     "VALUES(?1, ?2, 1, unixepoch()) ON CONFLICT(context, completion) DO UPDATE SET "
                     "frequency = frequency + 1, last_selected_at = unixepoch();",
                     observation.context, observation.text);
}

bool PersonalizationStore::ObserveOnce(const std::string& identity, const CommitObservation& observation) {
  if (!allow_learning_) return true;
  if (identity.empty() || !Open() || !Execute(database_, "BEGIN IMMEDIATE;")) return false;
  bool ok = BindAndStep(database_, "INSERT OR IGNORE INTO observed_commits VALUES(?1);", identity);
  const bool inserted = ok && sqlite3_changes(database_) != 0;
  if (inserted) ok = Observe(observation);
  if (ok) ok = Execute(database_, "COMMIT;");
  if (!ok) Execute(database_, "ROLLBACK;");
  return ok;
}

bool PersonalizationStore::Forget(const std::string& text) {
  if (!allow_learning_) return true;
  if (!Open()) return false;
  return Execute(database_, "BEGIN IMMEDIATE;") &&
         BindAndStep(database_, "DELETE FROM word_frequency WHERE text = ?1;", text) &&
         BindAndStep(database_, "DELETE FROM ngram_frequency WHERE completion = ?1;", text) &&
         Execute(database_, "COMMIT;");
}

bool PersonalizationStore::Block(const std::string& text) {
  if (!allow_learning_) return true;
  if (text.empty() || !Open()) return text.empty();
  return BindAndStep(database_, "INSERT OR IGNORE INTO blocked_suggestion(text) VALUES(?1);", text);
}

bool PersonalizationStore::Unblock(const std::string& text) {
  if (!allow_learning_) return true;
  return Open() && BindAndStep(database_, "DELETE FROM blocked_suggestion WHERE text = ?1;", text);
}

bool PersonalizationStore::Clear() {
  if (!allow_learning_) return true;
  return Open() && Execute(database_, "BEGIN IMMEDIATE; DELETE FROM word_frequency; "
                                             "DELETE FROM ngram_frequency; DELETE FROM blocked_suggestion; DELETE FROM observed_commits; COMMIT;");
}

int PersonalizationStore::Frequency(const std::string& text) {
  if (!allow_learning_ || !Open()) return 0;
  return QueryInt(database_, "SELECT frequency FROM word_frequency WHERE text = ?1;", text);
}

int PersonalizationStore::NGramFrequency(const std::string& context, const std::string& completion) {
  if (!allow_learning_ || !Open()) return 0;
  return QueryInt(database_, "SELECT frequency FROM ngram_frequency WHERE context = ?1 AND completion = ?2;",
                  context, completion);
}

std::vector<PersonalizedNGram> PersonalizationStore::RecallNGrams(const std::string& context,
                                                                    const size_t limit) {
  if (!allow_learning_ || context.empty() || limit == 0 || !Open()) return {};
  sqlite3_stmt* query = nullptr;
  constexpr char kStatement[] = "SELECT completion, frequency FROM ngram_frequency WHERE context = ?1 "
                                "ORDER BY frequency DESC, last_selected_at DESC, completion ASC LIMIT ?2;";
  if (sqlite3_prepare_v2(database_, kStatement, -1, &query, nullptr) != SQLITE_OK) return {};
  const auto finalize = [&query]() { sqlite3_finalize(query); };
  if (sqlite3_bind_text(query, 1, context.data(), static_cast<int>(context.size()), SQLITE_TRANSIENT) != SQLITE_OK ||
      sqlite3_bind_int64(query, 2, static_cast<sqlite3_int64>(
                                      std::min(limit, static_cast<size_t>(std::numeric_limits<sqlite3_int64>::max())))) !=
          SQLITE_OK) {
    finalize();
    return {};
  }
  std::vector<PersonalizedNGram> results;
  while (sqlite3_step(query) == SQLITE_ROW) {
    const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(query, 0));
    if (text == nullptr) continue;
    results.push_back({.completion = text, .frequency = sqlite3_column_int(query, 1)});
  }
  finalize();
  return results;
}

bool PersonalizationStore::IsBlocked(const std::string& text) {
  if (!allow_learning_ || !Open()) return false;
  return QueryInt(database_, "SELECT 1 FROM blocked_suggestion WHERE text = ?1;", text) != 0;
}

std::vector<std::string> PersonalizationStore::BlockedSuggestions() {
  if (!allow_learning_ || !Open()) return {};
  sqlite3_stmt* query = nullptr;
  if (sqlite3_prepare_v2(database_, "SELECT text FROM blocked_suggestion ORDER BY text ASC;", -1,
                         &query, nullptr) != SQLITE_OK) {
    return {};
  }
  const auto finalize = [&query]() { sqlite3_finalize(query); };
  std::vector<std::string> results;
  while (sqlite3_step(query) == SQLITE_ROW) {
    const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(query, 0));
    if (text != nullptr) results.emplace_back(text);
  }
  finalize();
  return results;
}

}  // namespace zhixu::predict
