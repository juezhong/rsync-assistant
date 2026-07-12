#include "rsync_assistant/rsync_locator.hpp"

#include <stdexcept>

namespace rsync_assistant {

std::filesystem::path RsyncLocator::executable() const {
  const std::filesystem::path bundled{RSYNC_ASSISTANT_BUNDLED_RSYNC_PATH};
  if (std::filesystem::is_regular_file(bundled) &&
      (std::filesystem::status(bundled).permissions() &
       std::filesystem::perms::owner_exec) != std::filesystem::perms::none)
    return bundled;
  const std::filesystem::path system{RSYNC_ASSISTANT_SYSTEM_RSYNC_PATH};
  if (std::filesystem::is_regular_file(system)) return system;
  throw std::runtime_error("no usable rsync executable: build the bundled rsync or install rsync");
}

}  // namespace rsync_assistant
