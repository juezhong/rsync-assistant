#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace rsync_assistant {

enum class TaskState { ready, preflighting, awaiting_execution_confirmation, running, paused, completed, failed, cancelled, interrupted };
enum class TransferMethod { local_rsync, rsync_daemon, rsync_ssh, scp };

struct CreateReadyTask {
  std::string source;
  std::string destination;
  bool delete_extraneous = false;
  bool compression = false;
  bool dry_run = true;
  bool trusted_daemon = false;
  // Paths are relative to source.  An empty list transfers the whole source.
  std::vector<std::string> selected_relative_paths;
  bool flatten_selection = false;
  bool include_git_data = false;
  bool include_project_ignored = false;
};

struct TransferTask {
  std::string id;
  std::string source;
  std::string destination;
  TaskState state;
  std::string command;
  std::string output;
  bool delete_extraneous = false;
  bool compression = false;
  bool dry_run = true;
  TransferMethod method = TransferMethod::local_rsync;
  std::size_t selected_path_count = 0;
  bool flatten_selection = false;
  std::optional<int> exit_code;
};

class TaskControlService {
 public:
  explicit TaskControlService(const std::filesystem::path& database_path);
  ~TaskControlService();

  TaskControlService(const TaskControlService&) = delete;
  TaskControlService& operator=(const TaskControlService&) = delete;
  TaskControlService(TaskControlService&&) = delete;
  TaskControlService& operator=(TaskControlService&&) = delete;

  [[nodiscard]] TransferTask create_ready_task(const CreateReadyTask& request);
  [[nodiscard]] std::vector<TransferTask> list_tasks() const;
  void reap_completed();
  [[nodiscard]] std::string execution_log(const std::string& task_id) const;
  [[nodiscard]] TransferTask preflight(const std::string& task_id);
  [[nodiscard]] TransferTask execute(const std::string& task_id,
                                     bool delete_confirmed = false);
  [[nodiscard]] TransferTask execute_scp_fallback(const std::string& task_id);
  [[nodiscard]] TransferTask pause(const std::string& task_id);
  [[nodiscard]] TransferTask resume(const std::string& task_id);
  [[nodiscard]] TransferTask stop(const std::string& task_id);
  [[nodiscard]] TransferTask restart(const std::string& task_id);
  [[nodiscard]] TransferTask await_completion(const std::string& task_id);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace rsync_assistant
