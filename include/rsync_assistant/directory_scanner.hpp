#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace rsync_assistant {

struct PathEntry {
  std::filesystem::path path;
  bool directory;
  bool hidden;
};

[[nodiscard]] std::vector<PathEntry> scan_directory_level(
    const std::filesystem::path& directory, bool include_hidden = false);
[[nodiscard]] std::vector<std::filesystem::path> search_paths(
    const std::filesystem::path& root, const std::string& query);

}  // namespace rsync_assistant
