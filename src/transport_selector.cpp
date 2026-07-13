#include "rsync_assistant/transport_selector.hpp"

namespace rsync_assistant {

TransportMethod select_transport(const TransportCapabilities& capabilities,
                                 double daemon_advantage_threshold) {
  if (capabilities.daemon_available && capabilities.daemon_trusted && capabilities.ssh_rsync_available) {
    if (capabilities.daemon_megabytes_per_second && capabilities.ssh_megabytes_per_second &&
        *capabilities.daemon_megabytes_per_second >=
            *capabilities.ssh_megabytes_per_second * daemon_advantage_threshold)
      return TransportMethod::rsync_daemon;
    // Missing, stale, or failed measurement favors the simpler encrypted route.
    return TransportMethod::rsync_ssh;
  }
  if (capabilities.daemon_available && capabilities.daemon_trusted) return TransportMethod::rsync_daemon;
  if (capabilities.ssh_rsync_available) return TransportMethod::rsync_ssh;
  if (capabilities.scp_available) return TransportMethod::scp_fallback;
  return TransportMethod::unavailable;
}

}  // namespace rsync_assistant
