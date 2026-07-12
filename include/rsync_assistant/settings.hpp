#pragma once

#include <filesystem>
#include <string>

namespace rsync_assistant {

struct Settings {
  bool dry_run = true;
  bool compression = false;
  bool benchmark_enabled = true;
  bool ai_enabled = false;
  std::string ai_endpoint;
  std::string ai_model;
  std::string api_key;

  static Settings load(const std::filesystem::path& path);
};

}  // namespace rsync_assistant
