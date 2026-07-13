#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>

namespace rsync_assistant {

struct TransportBenchmark {
  double daemon_megabytes_per_second = 0.0;
  double ssh_megabytes_per_second = 0.0;
  std::chrono::system_clock::time_point measured_at;
};

// Stores only measurements made with assistant-owned temporary data.
class BenchmarkCache {
 public:
  explicit BenchmarkCache(const std::filesystem::path& database_path);

  [[nodiscard]] std::optional<TransportBenchmark> find_fresh(
      const std::string& endpoint_pair,
      std::chrono::hours maximum_age = std::chrono::hours{24}) const;
  void store(const std::string& endpoint_pair, const TransportBenchmark& benchmark);

 private:
  std::filesystem::path database_path_;
};

}  // namespace rsync_assistant
