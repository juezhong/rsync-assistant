#pragma once

#include "rsync_assistant/task_control_service.hpp"

#include <filesystem>
#include <memory>

namespace rsync_assistant {

class TaskControlSocketServer {
 public:
  TaskControlSocketServer(TaskControlService& service,
                          std::filesystem::path socket_path);
  ~TaskControlSocketServer();

  TaskControlSocketServer(const TaskControlSocketServer&) = delete;
  TaskControlSocketServer& operator=(const TaskControlSocketServer&) = delete;

  void serve();
  void stop();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class TaskControlSocketClient {
 public:
  explicit TaskControlSocketClient(std::filesystem::path socket_path);

  [[nodiscard]] TransferTask create_ready_task(const CreateReadyTask& request) const;
  [[nodiscard]] std::vector<TransferTask> list_tasks() const;
  [[nodiscard]] std::string execution_log(const std::string& task_id) const;
  [[nodiscard]] TransferTask preflight(const std::string& task_id) const;
  [[nodiscard]] TransferTask execute(const std::string& task_id,
                                     bool delete_confirmed = false) const;
  [[nodiscard]] TransferTask execute_scp_fallback(const std::string& task_id) const;
  [[nodiscard]] TransferTask pause(const std::string& task_id) const;
  [[nodiscard]] TransferTask resume(const std::string& task_id) const;
  [[nodiscard]] TransferTask stop(const std::string& task_id) const;
  [[nodiscard]] TransferTask restart(const std::string& task_id) const;
  [[nodiscard]] TransferTask await_completion(const std::string& task_id) const;

 private:
  std::filesystem::path socket_path_;
};

}  // namespace rsync_assistant
