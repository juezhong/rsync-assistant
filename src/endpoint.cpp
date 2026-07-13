#include "rsync_assistant/endpoint.hpp"

#include "rsync_assistant/process_runner.hpp"
#include "rsync_assistant/rsync_locator.hpp"

#include <stdexcept>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <glob.h>
#include <set>
#include <sstream>

namespace rsync_assistant {
namespace {
std::string remote_shell_quote(const std::string& value) {
  std::string quoted{"'"};
  for (const char character : value)
    quoted += character == '\'' ? "'\"'\"'" : std::string(1, character);
  return quoted + "'";
}
}  // namespace

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

std::string remote_ssh_home(const std::string& host) {
  if (host.empty()) throw std::invalid_argument("SSH host is required");
  const auto result = ProcessRunner{}.run(
      {RSYNC_ASSISTANT_SSH_PATH, "-o", "ConnectTimeout=5", "--", host, "pwd"});
  if (result.exit_code != 0) throw std::runtime_error("SSH connection failed; authenticate with ssh " + host + " and retry");
  const auto end = result.output.find('\n');
  const auto home = result.output.substr(0, end);
  if (home.empty() || home.front() != '/') throw std::runtime_error("SSH host did not report an absolute home directory");
  return home;
}

std::vector<std::string> remote_ssh_list(const Endpoint& endpoint) {
  if (!endpoint.remote || endpoint.rsync_daemon)
    throw std::invalid_argument("SSH directory listing requires an SSH endpoint");
  // rsync over SSH already requires a remote POSIX shell.  The path is quoted
  // before becoming part of that shell command; paths with newlines are not
  // supported by the line-oriented picker protocol.
  const auto command = "find " + remote_shell_quote(endpoint.path) +
                       " -mindepth 1 -maxdepth 1 -exec sh -c 'for p do if [ -d \"$p\" ]; then printf \"%s/\\n\" \"$p\"; else printf \"%s\\n\" \"$p\"; fi; done' sh {} +";
  const auto result = ProcessRunner{}.run(
      {RSYNC_ASSISTANT_SSH_PATH, "-o", "ConnectTimeout=5", "--", endpoint.host, command});
  if (result.exit_code != 0)
    throw std::runtime_error("remote directory listing failed; verify SSH authentication and path access");
  std::vector<std::string> paths;
  std::size_t start = 0;
  while (start < result.output.size()) {
    const auto end = result.output.find('\n', start);
    const auto path = result.output.substr(start, end - start);
    if (!path.empty()) paths.push_back(path);
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return paths;
}

std::vector<std::string> ssh_config_hosts() {
  const char* home = std::getenv("HOME");
  if (home == nullptr || *home == '\0') return {};
  return ssh_config_hosts(std::filesystem::path{home} / ".ssh" / "config");
}

std::vector<std::string> ssh_config_hosts(const std::filesystem::path& config_path) {
  std::set<std::string> hosts;
  std::set<std::filesystem::path> visited;
  const auto read_config = [&](const auto& self, const std::filesystem::path& path) -> void {
    if (!visited.insert(path.lexically_normal()).second) return;
    std::ifstream config{path};
    std::string line;
    while (std::getline(config, line)) {
      const auto comment = line.find('#');
      if (comment != std::string::npos) line.erase(comment);
      std::istringstream fields{line};
      std::string key;
      fields >> key;
      if (key == "Host" || key == "host") {
        std::string host;
        while (fields >> host)
          if (host.find_first_of("*!?") == std::string::npos) hosts.insert(host);
      } else if (key == "Include" || key == "include") {
        std::string include;
        while (fields >> include) {
          const auto pattern = include.starts_with('/') ? include : (path.parent_path() / include).string();
          glob_t matches{};
          if (glob(pattern.c_str(), 0, nullptr, &matches) == 0)
            for (std::size_t i = 0; i < matches.gl_pathc; ++i) self(self, matches.gl_pathv[i]);
          globfree(&matches);
        }
      }
    }
  };
  read_config(read_config, config_path);
  return {hosts.begin(), hosts.end()};
}

void remove_known_host(const std::string& host) {
  if (host.empty()) throw std::invalid_argument("SSH host is required");
  const char* home = std::getenv("HOME");
  if (home == nullptr || *home == '\0') throw std::runtime_error("HOME is not set");
  const auto result = ProcessRunner{}.run({"ssh-keygen", "-R", host,
                                            "-f", (std::filesystem::path{home} / ".ssh" / "known_hosts").string()});
  if (result.exit_code != 0) throw std::runtime_error("could not remove known-host entry");
}

std::vector<std::string> remote_assistant_list(const Endpoint& endpoint) {
  if (!remote_assistant_available(endpoint))
    throw std::runtime_error("remote assistant is unavailable");
  const auto command = "rsync-assistant --control-list " + remote_shell_quote(endpoint.path);
  const auto result = ProcessRunner{}.run(
      {RSYNC_ASSISTANT_SSH_PATH, "-o", "BatchMode=yes", "--", endpoint.host, command});
  if (result.exit_code != 0) throw std::runtime_error("remote directory listing failed");
  std::vector<std::string> paths;
  std::size_t start = 0;
  while (start < result.output.size()) {
    const auto end = result.output.find('\n', start);
    const auto path = result.output.substr(start, end - start);
    if (!path.empty()) paths.push_back(path);
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return paths;
}

std::vector<RemoteTaskStatus> remote_assistant_tasks(const Endpoint& endpoint) {
  if (!remote_assistant_available(endpoint))
    throw std::runtime_error("remote assistant is unavailable");
  const auto result = ProcessRunner{}.run(
      {RSYNC_ASSISTANT_SSH_PATH, "-o", "BatchMode=yes", "-o", "ConnectTimeout=5",
       "--", endpoint.host, "rsync-assistant", "--control-tasks"});
  if (result.exit_code != 0) throw std::runtime_error("remote task status request failed");
  std::vector<RemoteTaskStatus> tasks;
  std::size_t start = 0;
  while (start < result.output.size()) {
    const auto end = result.output.find('\n', start);
    const auto line = result.output.substr(start, end - start);
    const auto first = line.find('\t');
    const auto second = first == std::string::npos ? first : line.find('\t', first + 1);
    if (first != std::string::npos && second != std::string::npos)
      tasks.push_back({line.substr(0, first), line.substr(first + 1, second - first - 1), line.substr(second + 1)});
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return tasks;
}

std::string remote_assistant_benchmark_root(const Endpoint& endpoint,
                                            const std::string& token) {
  if (!remote_assistant_available(endpoint))
    throw std::runtime_error("remote assistant is unavailable");
  const auto command = "rsync-assistant --control-benchmark-root " + remote_shell_quote(token);
  const auto result = ProcessRunner{}.run(
      {RSYNC_ASSISTANT_SSH_PATH, "-o", "BatchMode=yes", "-o", "ConnectTimeout=5",
       "--", endpoint.host, command});
  if (result.exit_code != 0 || result.output.empty())
    throw std::runtime_error("remote benchmark root request failed");
  const auto line_end = result.output.find('\n');
  return result.output.substr(0, line_end);
}

void remote_assistant_cleanup_benchmark(const Endpoint& endpoint,
                                        const std::string& token) {
  if (!remote_assistant_available(endpoint)) return;
  const auto command = "rsync-assistant --control-benchmark-clean " + remote_shell_quote(token);
  const auto result = ProcessRunner{}.run(
      {RSYNC_ASSISTANT_SSH_PATH, "-o", "BatchMode=yes", "-o", "ConnectTimeout=5",
       "--", endpoint.host, command});
  if (result.exit_code != 0)
    throw std::runtime_error("remote benchmark cleanup failed");
}

}  // namespace rsync_assistant
