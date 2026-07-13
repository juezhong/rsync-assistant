#include "rsync_assistant/benchmark_cache.hpp"

#include <sqlite3.h>

#include <chrono>
#include <stdexcept>

namespace rsync_assistant {
namespace {
void check(int result, sqlite3* database) {
  if (result != SQLITE_OK && result != SQLITE_DONE && result != SQLITE_ROW)
    throw std::runtime_error(sqlite3_errmsg(database));
}
class Statement {
 public:
  Statement(sqlite3* database, const char* sql) : database_(database) {
    check(sqlite3_prepare_v2(database, sql, -1, &statement_, nullptr), database_);
  }
  ~Statement() { sqlite3_finalize(statement_); }
  sqlite3_stmt* get() const { return statement_; }
 private:
  sqlite3* database_;
  sqlite3_stmt* statement_ = nullptr;
};
class Database {
 public:
  explicit Database(const std::filesystem::path& path) {
    check(sqlite3_open_v2(path.c_str(), &database_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr), database_);
    check(sqlite3_exec(database_, "CREATE TABLE IF NOT EXISTS transport_benchmarks ("
          "endpoint_pair TEXT PRIMARY KEY, daemon_mbps REAL NOT NULL, ssh_mbps REAL NOT NULL, measured_unix_seconds INTEGER NOT NULL)",
          nullptr, nullptr, nullptr), database_);
  }
  ~Database() { sqlite3_close(database_); }
  sqlite3* get() const { return database_; }
 private:
  sqlite3* database_ = nullptr;
};
long long unix_seconds(std::chrono::system_clock::time_point value) {
  return std::chrono::duration_cast<std::chrono::seconds>(value.time_since_epoch()).count();
}
}  // namespace

BenchmarkCache::BenchmarkCache(const std::filesystem::path& database_path)
    : database_path_(database_path) {}

std::optional<TransportBenchmark> BenchmarkCache::find_fresh(
    const std::string& endpoint_pair, std::chrono::hours maximum_age) const {
  if (maximum_age <= std::chrono::hours::zero()) return std::nullopt;
  Database database{database_path_};
  Statement statement{database.get(), "SELECT daemon_mbps, ssh_mbps, measured_unix_seconds FROM transport_benchmarks WHERE endpoint_pair=?"};
  check(sqlite3_bind_text(statement.get(), 1, endpoint_pair.c_str(), -1, SQLITE_TRANSIENT), database.get());
  if (sqlite3_step(statement.get()) != SQLITE_ROW) return std::nullopt;
  const auto measured = std::chrono::system_clock::time_point{std::chrono::seconds{sqlite3_column_int64(statement.get(), 2)}};
  if (std::chrono::system_clock::now() - measured > maximum_age) return std::nullopt;
  return TransportBenchmark{sqlite3_column_double(statement.get(), 0), sqlite3_column_double(statement.get(), 1), measured};
}

void BenchmarkCache::store(const std::string& endpoint_pair, const TransportBenchmark& benchmark) {
  if (endpoint_pair.empty() || benchmark.daemon_megabytes_per_second <= 0.0 || benchmark.ssh_megabytes_per_second <= 0.0)
    throw std::invalid_argument("benchmark must contain a nonempty endpoint pair and positive rates");
  Database database{database_path_};
  Statement statement{database.get(), "INSERT INTO transport_benchmarks (endpoint_pair, daemon_mbps, ssh_mbps, measured_unix_seconds) VALUES (?, ?, ?, ?) ON CONFLICT(endpoint_pair) DO UPDATE SET daemon_mbps=excluded.daemon_mbps, ssh_mbps=excluded.ssh_mbps, measured_unix_seconds=excluded.measured_unix_seconds"};
  check(sqlite3_bind_text(statement.get(), 1, endpoint_pair.c_str(), -1, SQLITE_TRANSIENT), database.get());
  check(sqlite3_bind_double(statement.get(), 2, benchmark.daemon_megabytes_per_second), database.get());
  check(sqlite3_bind_double(statement.get(), 3, benchmark.ssh_megabytes_per_second), database.get());
  check(sqlite3_bind_int64(statement.get(), 4, unix_seconds(benchmark.measured_at)), database.get());
  check(sqlite3_step(statement.get()), database.get());
}

}  // namespace rsync_assistant
