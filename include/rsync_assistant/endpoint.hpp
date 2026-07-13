#pragma once

#include <string>
#include <vector>

namespace rsync_assistant {

struct Endpoint {
  bool remote = false;
  bool rsync_daemon = false;
  std::string host;
  std::string path;
};

[[nodiscard]] Endpoint parse_endpoint(const std::string& value);
[[nodiscard]] bool remote_rsync_available(const Endpoint& endpoint);
[[nodiscard]] bool remote_assistant_available(const Endpoint& endpoint);
[[nodiscard]] std::vector<std::string> remote_assistant_list(const Endpoint& endpoint);

}  // namespace rsync_assistant
