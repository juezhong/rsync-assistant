#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace rsync_assistant {

struct SelectionManifest {
  std::filesystem::path root;
  std::vector<std::filesystem::path> relative_paths;
  bool flatten = false;

  [[nodiscard]] std::string nul_delimited_paths() const;
};

}  // namespace rsync_assistant
