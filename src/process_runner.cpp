#include "rsync_assistant/process_runner.hpp"

#include <array>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>

namespace rsync_assistant {

ProcessResult ProcessRunner::run(const std::vector<std::string>& arguments) const {
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
  std::string output;
  std::array<char, 4096> buffer{};
  ssize_t read_size;
  while ((read_size = read(output_pipe[0], buffer.data(), buffer.size())) > 0)
    output.append(buffer.data(), static_cast<std::size_t>(read_size));
  close(output_pipe[0]);
  int status = 0;
  waitpid(child, &status, 0);
  return {WIFEXITED(status) ? WEXITSTATUS(status) : 128, std::move(output)};
}

}  // namespace rsync_assistant
