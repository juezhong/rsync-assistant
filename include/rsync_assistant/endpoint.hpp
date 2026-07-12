#pragma once

#include <string>

namespace rsync_assistant {

struct Endpoint {
  bool remote = false;
  std::string host;
  std::string path;
};

[[nodiscard]] Endpoint parse_endpoint(const std::string& value);
[[nodiscard]] bool remote_rsync_available(const Endpoint& endpoint);

}  // namespace rsync_assistant
