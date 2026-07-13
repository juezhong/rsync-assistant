#include "rsync_assistant/process_runner.hpp"

#include <array>
#include <stdexcept>
#include <signal.h>
#include <utility>
#include <sys/wait.h>
#include <unistd.h>

namespace rsync_assistant {

ManagedProcess ProcessRunner::start(const std::vector<std::string>& arguments) const {
  if (arguments.empty()) throw std::invalid_argument("process arguments are empty");
  int output_pipe[2];
  if (pipe(output_pipe) != 0) throw std::runtime_error("create output pipe failed");
  const pid_t child = fork();
  if (child < 0) throw std::runtime_error("fork failed");
  if (child == 0) {
    dup2(output_pipe[1], STDOUT_FILENO);
    dup2(output_pipe[1], STDERR_FILENO);
    close(output_pipe[0]);
    close(output_pipe[1]);
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (const auto& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);
    execv(argv.front(), argv.data());
    _exit(127);
  }
  close(output_pipe[1]);
  return ManagedProcess{child, output_pipe[0]};
}

ProcessResult ProcessRunner::run(const std::vector<std::string>& arguments) const {
  return start(arguments).wait();
}

ManagedProcess::ManagedProcess(int pid, int output_descriptor)
    : pid_(pid), output_descriptor_(output_descriptor) {}

ManagedProcess::~ManagedProcess() { stop(); }
ManagedProcess::ManagedProcess(ManagedProcess&& other) noexcept
    : pid_(std::exchange(other.pid_, -1)), output_descriptor_(std::exchange(other.output_descriptor_, -1)) {}
ManagedProcess& ManagedProcess::operator=(ManagedProcess&& other) noexcept {
  if (this != &other) { stop(); pid_ = std::exchange(other.pid_, -1); output_descriptor_ = std::exchange(other.output_descriptor_, -1); }
  return *this;
}
bool ManagedProcess::active() const { return pid_ > 0; }
void ManagedProcess::pause() { if (active()) kill(pid_, SIGSTOP); }
void ManagedProcess::resume() { if (active()) kill(pid_, SIGCONT); }
void ManagedProcess::stop() {
  if (active()) {
    kill(pid_, SIGCONT);
    kill(pid_, SIGTERM);
    (void)wait();
  }
}
ProcessResult ManagedProcess::wait() {
  if (!active()) return {128, ""};
  int status = 0;
  waitpid(pid_, &status, 0);
  std::string output;
  std::array<char, 4096> buffer{};
  ssize_t read_size;
  while ((read_size = read(output_descriptor_, buffer.data(), buffer.size())) > 0)
    output.append(buffer.data(), static_cast<std::size_t>(read_size));
  close(output_descriptor_);
  output_descriptor_ = -1;
  pid_ = -1;
  return ProcessResult{WIFEXITED(status) ? WEXITSTATUS(status) : 128, std::move(output)};
}

std::optional<ProcessResult> ManagedProcess::try_wait() {
  if (!active()) return ProcessResult{128, ""};
  int status = 0;
  if (waitpid(pid_, &status, WNOHANG) == 0) return std::nullopt;
  std::string output;
  std::array<char, 4096> buffer{};
  ssize_t read_size;
  while ((read_size = read(output_descriptor_, buffer.data(), buffer.size())) > 0)
    output.append(buffer.data(), static_cast<std::size_t>(read_size));
  close(output_descriptor_);
  output_descriptor_ = -1;
  pid_ = -1;
  return ProcessResult{WIFEXITED(status) ? WEXITSTATUS(status) : 128, std::move(output)};
}

}  // namespace rsync_assistant
