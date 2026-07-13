#include "rsync_assistant/endpoint.hpp"

#include "rsync_assistant/process_runner.hpp"
#include "rsync_assistant/rsync_locator.hpp"

#include <stdexcept>

namespace rsync_assistant {

Endpoint parse_endpoint(const std::string& value) {
  const auto daemon_separator = value.find("::");
  if (daemon_separator != std::string::npos && daemon_separator > 0) {
    const auto host = value.substr(0, daemon_separator);
    const auto path = value.substr(daemon_separator + 2);
    if (path.empty()) throw std::invalid_argument("invalid rsync daemon endpoint");
    return {true, true, host, path};
  }
  const auto separator = value.find(':');
  if (separator == std::string::npos || separator == 0 || value.starts_with('/'))
    return {false, false, "", value};
  const auto host = value.substr(0, separator);
  const auto path = value.substr(separator + 1);
  if (host.find('/') != std::string::npos || path.empty())
    throw std::invalid_argument("invalid remote endpoint");
  return {true, false, host, path};
}

bool remote_rsync_available(const Endpoint& endpoint) {
  if (!endpoint.remote) return true;
  if (endpoint.rsync_daemon) {
    const auto result = ProcessRunner{}.run(
        {RsyncLocator{}.executable().string(), "--list-only",
         endpoint.host + "::" + endpoint.path});
    return result.exit_code == 0;
  }
  const auto result = ProcessRunner{}.run(
      {RSYNC_ASSISTANT_SSH_PATH, "-o", "BatchMode=yes", "-o", "ConnectTimeout=5",
       endpoint.host, "rsync", "--version"});
  return result.exit_code == 0;
}

bool remote_assistant_available(const Endpoint& endpoint) {
  if (!endpoint.remote || endpoint.rsync_daemon) return false;
  const auto result = ProcessRunner{}.run(
      {RSYNC_ASSISTANT_SSH_PATH, "-o", "BatchMode=yes", "-o", "ConnectTimeout=5",
       "--", endpoint.host, "rsync-assistant", "--control-ping"});
  return result.exit_code == 0 && result.output.find("rsync-assistant-control-v1") != std::string::npos;
}

}  // namespace rsync_assistant
