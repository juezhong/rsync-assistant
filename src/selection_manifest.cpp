#include "rsync_assistant/selection_manifest.hpp"

#include <stdexcept>

namespace rsync_assistant {

std::string SelectionManifest::nul_delimited_paths() const {
  std::string result;
  for (const auto& path : relative_paths) {
    if (path.empty() || path.is_absolute() || path.string().find("..") == 0)
      throw std::invalid_argument("selection paths must be relative to the selected root");
    result += path.generic_string();
    result += '\0';
  }
  return result;
}

}  // namespace rsync_assistant
