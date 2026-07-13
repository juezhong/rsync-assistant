#pragma once

#include <string>
#include <memory>
#include <optional>
#include <vector>

namespace rsync_assistant {

struct ProcessResult {
  int exit_code;
  std::string output;
};

class ManagedProcess;

class ProcessRunner {
 public:
  [[nodiscard]] ProcessResult run(const std::vector<std::string>& arguments) const;
  [[nodiscard]] ManagedProcess start(const std::vector<std::string>& arguments) const;
};

class ManagedProcess {
 public:
  ManagedProcess() = default;
  ~ManagedProcess();
  ManagedProcess(ManagedProcess&&) noexcept;
  ManagedProcess& operator=(ManagedProcess&&) noexcept;
  ManagedProcess(const ManagedProcess&) = delete;
  ManagedProcess& operator=(const ManagedProcess&) = delete;

  [[nodiscard]] bool active() const;
  void pause();
  void resume();
  void stop();
  [[nodiscard]] std::optional<ProcessResult> try_wait();
  [[nodiscard]] ProcessResult wait();

 private:
  friend class ProcessRunner;
  explicit ManagedProcess(int pid, int output_descriptor);
  int pid_ = -1;
  int output_descriptor_ = -1;
};

}  // namespace rsync_assistant
