#pragma once

#include <optional>

namespace rsync_assistant {

enum class TransportMethod { rsync_daemon, rsync_ssh, scp_fallback, unavailable };

struct TransportCapabilities {
  bool daemon_available = false;
  bool ssh_rsync_available = false;
  bool scp_available = false;
  std::optional<double> daemon_megabytes_per_second;
  std::optional<double> ssh_megabytes_per_second;
};

[[nodiscard]] TransportMethod select_transport(const TransportCapabilities& capabilities,
                                               double daemon_advantage_threshold = 1.1);

}  // namespace rsync_assistant
