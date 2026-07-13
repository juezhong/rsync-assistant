#include "rsync_assistant/transport_selector.hpp"

namespace rsync_assistant {

TransportMethod select_transport(const TransportCapabilities& capabilities,
                                 double daemon_advantage_threshold) {
  if (capabilities.daemon_available && capabilities.ssh_rsync_available &&
      capabilities.daemon_megabytes_per_second && capabilities.ssh_megabytes_per_second) {
    return *capabilities.daemon_megabytes_per_second >=
                   *capabilities.ssh_megabytes_per_second * daemon_advantage_threshold
               ? TransportMethod::rsync_daemon
               : TransportMethod::rsync_ssh;
  }
  if (capabilities.daemon_available) return TransportMethod::rsync_daemon;
  if (capabilities.ssh_rsync_available) return TransportMethod::rsync_ssh;
  if (capabilities.scp_available) return TransportMethod::scp_fallback;
  return TransportMethod::unavailable;
}

}  // namespace rsync_assistant
