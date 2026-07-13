#pragma once

#include <string>
#include <vector>
#include <filesystem>

namespace rsync_assistant {

struct Endpoint {
  bool remote = false;
  bool rsync_daemon = false;
  std::string host;
  std::string path;
};

struct RemoteTaskStatus {
  std::string id;
  std::string state;
  std::string method;
};

[[nodiscard]] Endpoint parse_endpoint(const std::string& value);
[[nodiscard]] bool remote_rsync_available(const Endpoint& endpoint);
[[nodiscard]] bool remote_assistant_available(const Endpoint& endpoint);
// These discovery calls use ordinary OpenSSH and do not require rsync-assistant
// to be installed on the remote endpoint.
[[nodiscard]] std::string remote_ssh_home(const std::string& host);
[[nodiscard]] std::vector<std::string> remote_ssh_list(const Endpoint& endpoint);
[[nodiscard]] std::vector<std::string> ssh_config_hosts();
// The explicit path overload is primarily useful to callers that need to
// inspect a particular OpenSSH configuration (and to test Include handling).
[[nodiscard]] std::vector<std::string> ssh_config_hosts(const std::filesystem::path& config_path);
void remove_known_host(const std::string& host);
[[nodiscard]] std::vector<std::string> remote_assistant_list(const Endpoint& endpoint);
[[nodiscard]] std::vector<RemoteTaskStatus> remote_assistant_tasks(const Endpoint& endpoint);
[[nodiscard]] std::string remote_assistant_benchmark_root(const Endpoint& endpoint,
                                                           const std::string& token);
void remote_assistant_cleanup_benchmark(const Endpoint& endpoint,
                                        const std::string& token);

}  // namespace rsync_assistant
