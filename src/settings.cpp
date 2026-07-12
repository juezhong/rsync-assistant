#include "rsync_assistant/settings.hpp"

#include <fstream>
#include <stdexcept>

namespace rsync_assistant {
namespace {
std::string trim(std::string value) {
  const auto first = value.find_first_not_of(" \t\r");
  const auto last = value.find_last_not_of(" \t\r");
  return first == std::string::npos ? "" : value.substr(first, last - first + 1);
}
std::string unquote(std::string value) {
  value = trim(std::move(value));
  return value.size() >= 2 && value.front() == '"' && value.back() == '"'
             ? value.substr(1, value.size() - 2) : value;
}
}  // namespace

Settings Settings::load(const std::filesystem::path& path) {
  std::ifstream input{path};
  if (!input) throw std::runtime_error("cannot read settings");
  Settings settings;
  std::string section;
  for (std::string line; std::getline(input, line);) {
    line = trim(line);
    if (line.empty() || line.starts_with('#')) continue;
    if (line.front() == '[' && line.back() == ']') { section = line.substr(1, line.size() - 2); continue; }
    const auto equals = line.find('=');
    if (equals == std::string::npos) continue;
    const auto key = trim(line.substr(0, equals));
    const auto value = unquote(line.substr(equals + 1));
    const auto bool_value = value == "true";
    if (section == "transfer" && key == "dry_run") settings.dry_run = bool_value;
    if (section == "transfer" && key == "compression") settings.compression = bool_value;
    if (section == "transfer" && key == "benchmark_enabled") settings.benchmark_enabled = bool_value;
    if (section == "ai" && key == "enabled") settings.ai_enabled = bool_value;
    if (section == "ai" && key == "endpoint") settings.ai_endpoint = value;
    if (section == "ai" && key == "model") settings.ai_model = value;
    if (section == "ai" && key == "api_key") settings.api_key = value;
  }
  const auto permissions = std::filesystem::status(path).permissions();
  const auto public_bits = std::filesystem::perms::group_read | std::filesystem::perms::group_write |
                           std::filesystem::perms::others_read | std::filesystem::perms::others_write;
  if (settings.ai_enabled && !settings.api_key.empty() && (permissions & public_bits) != std::filesystem::perms::none)
    throw std::runtime_error("AI settings require an owner-only configuration file");
  return settings;
}

}  // namespace rsync_assistant
