#pragma once

#include <string>
#include <vector>

namespace rsync_assistant {

struct ProcessResult {
  int exit_code;
  std::string output;
};

class ProcessRunner {
 public:
  [[nodiscard]] ProcessResult run(const std::vector<std::string>& arguments) const;
};

}  // namespace rsync_assistant
