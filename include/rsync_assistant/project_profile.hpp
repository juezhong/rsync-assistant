#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace rsync_assistant {

struct ProjectProfile {
  std::vector<std::string> exclusions;
  bool has_git_repository = false;
};

[[nodiscard]] ProjectProfile detect_project_profile(const std::filesystem::path& root);

}  // namespace rsync_assistant
