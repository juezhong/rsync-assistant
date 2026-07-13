#include "rsync_assistant/directory_scanner.hpp"

#include <algorithm>
#include <stdexcept>

namespace rsync_assistant {

std::vector<PathEntry> scan_directory_level(const std::filesystem::path& directory,
                                            bool include_hidden) {
  std::vector<PathEntry> entries;
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator(
           directory, std::filesystem::directory_options::skip_permission_denied, error)) {
    const auto name = entry.path().filename().string();
    const bool hidden = !name.empty() && name.front() == '.';
    if (!include_hidden && hidden) continue;
    entries.push_back({entry.path(), entry.is_directory(error), hidden});
  }
  if (error) throw std::runtime_error("cannot scan directory: " + error.message());
  std::sort(entries.begin(), entries.end(), [](const PathEntry& left, const PathEntry& right) {
    if (left.directory != right.directory) return left.directory > right.directory;
    return left.path.filename().string() < right.path.filename().string();
  });
  return entries;
}

}  // namespace rsync_assistant
