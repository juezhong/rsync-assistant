#pragma once

#include "rsync_assistant/benchmark_cache.hpp"
#include "rsync_assistant/endpoint.hpp"

#include <chrono>
#include <filesystem>
#include <optional>

namespace rsync_assistant {

struct EndpointBenchmarkRequest {
  Endpoint ssh_endpoint;
  Endpoint daemon_endpoint;
  std::filesystem::path state_directory;
  unsigned size_mib = 64;
  std::chrono::seconds timeout{15};
};

// Copies only assistant-generated payloads to assistant-generated remote roots.
[[nodiscard]] std::optional<TransportBenchmark> measure_endpoint_pair(
    const EndpointBenchmarkRequest& request);

}  // namespace rsync_assistant
