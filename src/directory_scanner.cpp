#include "rsync_assistant/directory_scanner.hpp"

#include "rsync_assistant/process_runner.hpp"

#include <algorithm>
#include <stdexcept>

namespace rsync_assistant {

namespace {
std::vector<std::filesystem::path> lines_to_paths(const std::string& output) {
  std::vector<std::filesystem::path> paths;
  std::size_t start = 0;
  while (start < output.size()) {
    const auto end = output.find('\n', start);
    const auto line = output.substr(start, end - start);
    if (!line.empty()) paths.emplace_back(line);
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return paths;
}
}  // namespace

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

std::vector<std::filesystem::path> search_paths(const std::filesystem::path& root,
                                                 const std::string& query,
                                                 bool include_hidden) {
  if (std::filesystem::is_regular_file(RSYNC_ASSISTANT_FD_PATH)) {
    std::vector<std::string> arguments{RSYNC_ASSISTANT_FD_PATH, "--type", "f", "--type", "d"};
    if (include_hidden) arguments.push_back("--hidden");
    arguments.insert(arguments.end(), {query, root.string()});
    const auto result = ProcessRunner{}.run(arguments);
    if (result.exit_code == 0) return lines_to_paths(result.output);
  }
  if (std::filesystem::is_regular_file(RSYNC_ASSISTANT_RG_PATH)) {
    std::vector<std::string> arguments{RSYNC_ASSISTANT_RG_PATH, "--files"};
    if (include_hidden) arguments.push_back("--hidden");
    arguments.push_back(root.string());
    const auto result = ProcessRunner{}.run(arguments);
    if (result.exit_code == 0) {
      auto paths = lines_to_paths(result.output);
      paths.erase(std::remove_if(paths.begin(), paths.end(), [&](const auto& path) {
        return path.filename().string().find(query) == std::string::npos;
      }), paths.end());
      return paths;
    }
  }
  std::vector<std::filesystem::path> paths;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(
           root, std::filesystem::directory_options::skip_permission_denied)) {
    const auto name = entry.path().filename().string();
    if (!include_hidden && !name.empty() && name.front() == '.') continue;
    if (entry.path().filename().string().find(query) != std::string::npos)
      paths.push_back(entry.path());
  }
  return paths;
}

}  // namespace rsync_assistant
