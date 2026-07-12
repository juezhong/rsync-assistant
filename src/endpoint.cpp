#include "rsync_assistant/endpoint.hpp"

#include "rsync_assistant/process_runner.hpp"

#include <stdexcept>

namespace rsync_assistant {

Endpoint parse_endpoint(const std::string& value) {
  const auto separator = value.find(':');
  if (separator == std::string::npos || separator == 0 || value.starts_with('/'))
    return {false, "", value};
  const auto host = value.substr(0, separator);
  const auto path = value.substr(separator + 1);
  if (host.find('/') != std::string::npos || path.empty())
    throw std::invalid_argument("invalid remote endpoint");
  return {true, host, path};
}

bool remote_rsync_available(const Endpoint& endpoint) {
  if (!endpoint.remote) return true;
  const auto result = ProcessRunner{}.run(
      {RSYNC_ASSISTANT_SSH_PATH, "-o", "BatchMode=yes", "-o", "ConnectTimeout=5",
       endpoint.host, "rsync", "--version"});
  return result.exit_code == 0;
}

}  // namespace rsync_assistant
