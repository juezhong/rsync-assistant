#include "rsync_assistant/project_profile.hpp"

namespace rsync_assistant {

ProjectProfile detect_project_profile(const std::filesystem::path& root) {
  ProjectProfile profile;
  const auto add = [&](std::string value) { profile.exclusions.push_back(std::move(value)); };
  if (std::filesystem::exists(root / "CMakeLists.txt")) {
    add("build/");
    add("cmake-build-*/");
  }
  if (std::filesystem::exists(root / "pyproject.toml") ||
      std::filesystem::exists(root / "requirements.txt")) {
    add(".venv/");
    add("venv/");
    add("__pycache__/");
  }
  if (std::filesystem::exists(root / "Cargo.toml")) add("target/");
  if (std::filesystem::exists(root / "package.json")) {
    add("node_modules/");
    add("dist/");
  }
  profile.has_git_repository = std::filesystem::exists(root / ".git");
  return profile;
}

}  // namespace rsync_assistant
