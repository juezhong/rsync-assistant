#include "rsync_assistant/endpoint.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>

int main() {
  const auto local = rsync_assistant::parse_endpoint("/data/source");
  assert(!local.remote && local.path == "/data/source");
  const auto remote = rsync_assistant::parse_endpoint("tom@host:/data/target");
  assert(remote.remote && remote.host == "tom@host" && remote.path == "/data/target");

  const auto root = std::filesystem::temp_directory_path() /
                    ("rsync-assistant-ssh-config-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(root / "conf.d");
  std::ofstream{root / "config"} << "Host workstation *.wildcard\nInclude conf.d/*\n";
  std::ofstream{root / "conf.d" / "remote"} << "Host build user@adhoc !negated\n";
  const auto hosts = rsync_assistant::ssh_config_hosts(root / "config");
  assert((hosts == std::vector<std::string>{"build", "user@adhoc", "workstation"}));
  std::filesystem::remove_all(root);
}
