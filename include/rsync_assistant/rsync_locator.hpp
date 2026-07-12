#pragma once

#include <filesystem>

namespace rsync_assistant {

class RsyncLocator {
 public:
  [[nodiscard]] std::filesystem::path executable() const;
};

}  // namespace rsync_assistant
