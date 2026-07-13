#include "rsync_assistant/endpoint_benchmark.hpp"

#include "rsync_assistant/process_runner.hpp"
#include "rsync_assistant/rsync_locator.hpp"

#include <chrono>
#include <fstream>
#include <format>

namespace rsync_assistant {
namespace {
std::string token() {
  return std::format("benchmark-{}", std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
}
double copy_rate(const std::vector<std::string>& arguments, std::size_t bytes,
                 std::chrono::seconds timeout) {
  const auto started = std::chrono::steady_clock::now();
  const auto result = ProcessRunner{}.run_for(arguments, timeout);
  const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  if (result.exit_code != 0 || elapsed <= 0.0) return 0.0;
  return static_cast<double>(bytes) / (1024.0 * 1024.0) / elapsed;
}
}  // namespace

std::optional<TransportBenchmark> measure_endpoint_pair(const EndpointBenchmarkRequest& request) {
  if (!request.ssh_endpoint.remote || request.ssh_endpoint.rsync_daemon ||
      !request.daemon_endpoint.rsync_daemon || request.ssh_endpoint.host != request.daemon_endpoint.host ||
      request.size_mib == 0 || request.timeout <= std::chrono::seconds::zero())
    return std::nullopt;
  const auto id = token();
  const auto local_root = request.state_directory / "benchmarks" / id;
  const auto local_payload = local_root / "payload.bin";
  std::error_code error;
  std::filesystem::create_directories(local_root, error);
  if (error) return std::nullopt;
  const std::size_t bytes = static_cast<std::size_t>(request.size_mib) * 1024U * 1024U;
  {
    std::ofstream payload{local_payload, std::ios::binary | std::ios::trunc};
    std::string block(1024U * 1024U, '\0');
    for (unsigned index = 0; index < request.size_mib; ++index) payload.write(block.data(), block.size());
    if (!payload) { std::filesystem::remove_all(local_root, error); return std::nullopt; }
  }
  try {
    const auto remote_root = remote_assistant_benchmark_root(request.ssh_endpoint, id);
    const auto rsync = RsyncLocator{}.executable().string();
    const auto ssh_rate = copy_rate({rsync, "--", local_payload.string(),
                                     request.ssh_endpoint.host + ":" + remote_root + "/ssh"}, bytes, request.timeout);
    const auto daemon_target = request.daemon_endpoint.host + "::" + request.daemon_endpoint.path + "/.rsync-assistant-" + id + "/";
    const auto daemon_rate = copy_rate({rsync, "--", local_payload.string(), daemon_target}, bytes, request.timeout);
    // Removing the daemon payload through an empty rsync tree touches only the token directory.
    const auto empty = local_root / "empty";
    std::filesystem::create_directories(empty, error);
    (void)ProcessRunner{}.run_for({rsync, "--delete", "--", empty.string() + "/", daemon_target}, request.timeout);
    remote_assistant_cleanup_benchmark(request.ssh_endpoint, id);
    std::filesystem::remove_all(local_root, error);
    if (ssh_rate <= 0.0 || daemon_rate <= 0.0) return std::nullopt;
    return TransportBenchmark{daemon_rate, ssh_rate, std::chrono::system_clock::now()};
  } catch (...) {
    try { remote_assistant_cleanup_benchmark(request.ssh_endpoint, id); } catch (...) {}
    std::filesystem::remove_all(local_root, error);
    return std::nullopt;
  }
}

}  // namespace rsync_assistant
