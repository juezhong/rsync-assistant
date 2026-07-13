#pragma once

#include <filesystem>
#include <vector>

namespace rsync_assistant {

struct PathEntry {
  std::filesystem::path path;
  bool directory;
  bool hidden;
};

[[nodiscard]] std::vector<PathEntry> scan_directory_level(
    const std::filesystem::path& directory, bool include_hidden = false);

}  // namespace rsync_assistant
