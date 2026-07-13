#pragma once

#include <filesystem>
#include <string>

namespace rsync_assistant {

struct Settings {
  bool dry_run = true;
  bool compression = false;
  bool benchmark_enabled = true;
  unsigned benchmark_size_mib = 64;
  unsigned benchmark_timeout_seconds = 15;
  unsigned benchmark_cache_hours = 24;
  double daemon_advantage_threshold = 1.1;
  bool ai_enabled = false;
  std::string ai_endpoint;
  std::string ai_model;
  std::string api_key;

  static Settings load(const std::filesystem::path& path);
  void validate() const;
  void save(const std::filesystem::path& path) const;
};

}  // namespace rsync_assistant
