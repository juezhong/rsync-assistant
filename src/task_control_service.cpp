#include "rsync_assistant/task_control_service.hpp"

#include "rsync_assistant/process_runner.hpp"
#include "rsync_assistant/rsync_locator.hpp"
#include "rsync_assistant/endpoint.hpp"
#include "rsync_assistant/project_profile.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <memory>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <sqlite3.h>
#include <stdexcept>
#include <string_view>

namespace rsync_assistant {
namespace {

void check_sqlite(int result, sqlite3* database, std::string_view operation) {
  if (result != SQLITE_OK && result != SQLITE_DONE && result != SQLITE_ROW) {
    throw std::runtime_error(std::format("{}: {}", operation,
                                        sqlite3_errmsg(database)));
  }
}

std::string next_task_id() {
  static std::atomic_uint64_t counter{0};
  const auto timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::system_clock::now()
                                 .time_since_epoch())
                             .count();
  return std::format("task-{}-{}", timestamp, ++counter);
}

std::string shell_quote(const std::string& argument) {
  if (argument.find_first_of(" \t'\"\\$") == std::string::npos) return argument;
  std::string quoted{"'"};
  for (const char character : argument)
    quoted += character == '\'' ? "'\\''" : std::string(1, character);
  return quoted + "'";
}

std::string render_command(const std::vector<std::string>& arguments) {
  std::string command;
  for (const auto& argument : arguments) {
    if (!command.empty()) command += ' ';
    command += shell_quote(argument);
  }
  return command;
}

std::string deletion_summary(const std::string& output) {
  std::size_t count = 0;
  std::size_t offset = 0;
  while (offset < output.size()) {
    const auto end = output.find('\n', offset);
    const auto line = output.substr(offset, end - offset);
    if (line.find("deleting ") != std::string::npos) ++count;
    if (end == std::string::npos) break;
    offset = end + 1;
  }
  return std::format("\nrsync-assistant deletion preflight: {} destination entries would be deleted.\n", count);
}

void persist_destination_log(const TransferTask& task) {
  const auto destination = parse_endpoint(task.destination);
  if (destination.remote) return;

  std::error_code error;
  std::filesystem::create_directories(destination.path, error);
  if (error) return;
  std::ofstream log{std::filesystem::path{destination.path} /
                        (std::string{".rsync-assistant-"} + task.id + ".log"),
                    std::ios::trunc};
  if (!log) return;
  log << "task: " << task.id << '\n'
      << "command: " << task.command << "\n\n"
      << task.output;
}

class Statement {
 public:
  Statement(sqlite3* database, const char* sql) : database_(database) {
    check_sqlite(sqlite3_prepare_v2(database, sql, -1, &statement_, nullptr),
                 database, "prepare statement");
  }

  ~Statement() { sqlite3_finalize(statement_); }

  sqlite3_stmt* get() const { return statement_; }

 private:
  sqlite3* database_;
  sqlite3_stmt* statement_ = nullptr;
};

}  // namespace

struct TaskControlService::Impl {
  sqlite3* database = nullptr;
  std::mutex process_mutex;
  std::unordered_map<std::string, ManagedProcess> processes;

  explicit Impl(const std::filesystem::path& database_path) {
    check_sqlite(sqlite3_open_v2(database_path.c_str(), &database,
                                 SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                                     SQLITE_OPEN_FULLMUTEX,
                                 nullptr),
                 database, "open database");
    check_sqlite(sqlite3_exec(database,
                              "CREATE TABLE IF NOT EXISTS transfer_tasks ("
                              "id TEXT PRIMARY KEY, source TEXT NOT NULL, "
                              "destination TEXT NOT NULL, state TEXT NOT NULL, "
                              "command TEXT NOT NULL DEFAULT '', output TEXT NOT NULL DEFAULT '', "
                              "delete_extraneous INTEGER NOT NULL DEFAULT 0, compression INTEGER NOT NULL DEFAULT 0, "
                              "dry_run INTEGER NOT NULL DEFAULT 1)",
                              nullptr, nullptr, nullptr),
                 database, "create transfer_tasks table");
    char* migration_error = nullptr;
    const auto migration = sqlite3_exec(
        database,
        "ALTER TABLE transfer_tasks ADD COLUMN delete_extraneous INTEGER NOT NULL DEFAULT 0",
        nullptr, nullptr, &migration_error);
    if (migration != SQLITE_OK) sqlite3_free(migration_error);
    sqlite3_exec(database,
                 "ALTER TABLE transfer_tasks ADD COLUMN compression INTEGER NOT NULL DEFAULT 0",
                 nullptr, nullptr, nullptr);
    sqlite3_exec(database,
                 "ALTER TABLE transfer_tasks ADD COLUMN dry_run INTEGER NOT NULL DEFAULT 1",
                 nullptr, nullptr, nullptr);
    check_sqlite(sqlite3_exec(database,
                              "UPDATE transfer_tasks SET state='interrupted' "
                              "WHERE state IN ('running', 'paused')",
                              nullptr, nullptr, nullptr),
                 database, "recover interrupted tasks");
  }

  ~Impl() { sqlite3_close(database); }
};

TaskControlService::TaskControlService(const std::filesystem::path& database_path)
    : impl_(std::make_unique<Impl>(database_path)) {}

TaskControlService::~TaskControlService() = default;

TransferTask TaskControlService::create_ready_task(
    const CreateReadyTask& request) {
  if (request.source.empty() || request.destination.empty())
    throw std::invalid_argument("source and destination are required");
  const auto id = next_task_id();
  Statement statement{impl_->database,
                      "INSERT INTO transfer_tasks (id, source, destination, state, command, output, delete_extraneous, compression, dry_run) "
                      "VALUES (?, ?, ?, 'ready', '', '', ?, ?, ?)"};
  check_sqlite(sqlite3_bind_text(statement.get(), 1, id.c_str(), -1,
                                 SQLITE_TRANSIENT),
               impl_->database, "bind task id");
  check_sqlite(sqlite3_bind_text(statement.get(), 2, request.source.c_str(), -1,
                                 SQLITE_TRANSIENT),
               impl_->database, "bind source");
  check_sqlite(sqlite3_bind_text(statement.get(), 3, request.destination.c_str(),
                                 -1, SQLITE_TRANSIENT),
               impl_->database, "bind destination");
  check_sqlite(sqlite3_bind_int(statement.get(), 4, request.delete_extraneous),
               impl_->database, "bind delete option");
  check_sqlite(sqlite3_bind_int(statement.get(), 5, request.compression),
               impl_->database, "bind compression option");
  check_sqlite(sqlite3_bind_int(statement.get(), 6, request.dry_run),
               impl_->database, "bind dry-run option");
  check_sqlite(sqlite3_step(statement.get()), impl_->database, "insert task");
  return {id, request.source, request.destination, TaskState::ready, "", "", request.delete_extraneous, request.compression, request.dry_run};
}

std::vector<TransferTask> TaskControlService::list_tasks() const {
  Statement statement{impl_->database,
                      "SELECT id, source, destination, state, command, output, delete_extraneous, compression, dry_run FROM transfer_tasks ORDER BY rowid"};
  std::vector<TransferTask> tasks;
  while (sqlite3_step(statement.get()) == SQLITE_ROW) {
    const std::string state{reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 3))};
    const auto task_state = state == "ready" ? TaskState::ready :
                            state == "awaiting_confirmation" ? TaskState::awaiting_execution_confirmation :
                            state == "running" ? TaskState::running :
                            state == "paused" ? TaskState::paused :
                            state == "completed" ? TaskState::completed :
                            state == "cancelled" ? TaskState::cancelled :
                            state == "interrupted" ? TaskState::interrupted : TaskState::failed;
    tasks.push_back({
        reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 0)),
        reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 1)),
        reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 2)),
        task_state,
        reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 4)),
        reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 5)),
        sqlite3_column_int(statement.get(), 6) != 0,
        sqlite3_column_int(statement.get(), 7) != 0,
        sqlite3_column_int(statement.get(), 8) != 0,
    });
  }
  return tasks;
}

std::string TaskControlService::execution_log(const std::string& task_id) const {
  Statement statement{impl_->database,
                      "SELECT command, output FROM transfer_tasks WHERE id = ?"};
  check_sqlite(sqlite3_bind_text(statement.get(), 1, task_id.c_str(), -1,
                                 SQLITE_TRANSIENT),
               impl_->database, "bind task id");
  if (sqlite3_step(statement.get()) != SQLITE_ROW)
    throw std::runtime_error("unknown task");
  const auto* command = sqlite3_column_text(statement.get(), 0);
  const auto* output = sqlite3_column_text(statement.get(), 1);
  const std::string command_text = command == nullptr ? "" : reinterpret_cast<const char*>(command);
  const std::string output_text = output == nullptr ? "" : reinterpret_cast<const char*>(output);
  if (command_text.empty()) return output_text;
  return "Command proposal (reviewed locally):\n" + command_text +
         "\n\nExecution / preflight output:\n" + output_text;
}

TransferTask TaskControlService::preflight(const std::string& task_id) {
  auto tasks = list_tasks();
  const auto it = std::find_if(tasks.begin(), tasks.end(), [&](const auto& task) { return task.id == task_id; });
  if (it == tasks.end() || it->state != TaskState::ready) throw std::runtime_error("task is not ready for preflight");
  const auto source_endpoint = parse_endpoint(it->source);
  const auto destination_endpoint = parse_endpoint(it->destination);
  const auto remote_endpoint = source_endpoint.remote ? source_endpoint : destination_endpoint;
  if (remote_endpoint.remote && !remote_rsync_available(remote_endpoint))
    throw std::runtime_error("remote rsync is unavailable; confirm system scp fallback explicitly");
  const auto rsync = RsyncLocator{}.executable().string();
  std::vector<std::string> arguments{rsync, "--recursive", "--links", "--times", "--partial"};
  if (it->delete_extraneous) arguments.push_back("--delete");
  if (it->compression) arguments.push_back("--compress");
  if (!source_endpoint.remote) {
    for (const auto& exclusion : detect_project_profile(source_endpoint.path).exclusions) {
      arguments.push_back("--exclude");
      arguments.push_back(exclusion);
    }
  }
  arguments.insert(arguments.end(), {"--", it->source, it->destination});
  const auto command = render_command(arguments);
  auto preflight_arguments = arguments;
  preflight_arguments.insert(preflight_arguments.begin() + 5, "--dry-run");
  if (it->delete_extraneous) preflight_arguments.insert(preflight_arguments.begin() + 6, "--itemize-changes");
  const auto result = it->dry_run ? ProcessRunner{}.run(preflight_arguments) :
                                  ProcessResult{0, "Dry-run disabled by task settings; command is ready for confirmation.\n"};
  const auto state = result.exit_code == 0 ? "awaiting_confirmation" : "failed";
  Statement statement{impl_->database, "UPDATE transfer_tasks SET state=?, command=?, output=? WHERE id=?"};
  check_sqlite(sqlite3_bind_text(statement.get(), 1, state, -1, SQLITE_TRANSIENT), impl_->database, "bind state");
  check_sqlite(sqlite3_bind_text(statement.get(), 2, command.c_str(), -1, SQLITE_TRANSIENT), impl_->database, "bind command");
  const auto output = result.output + (it->delete_extraneous && it->dry_run ? deletion_summary(result.output) : "");
  check_sqlite(sqlite3_bind_text(statement.get(), 3, output.c_str(), -1, SQLITE_TRANSIENT), impl_->database, "bind output");
  check_sqlite(sqlite3_bind_text(statement.get(), 4, task_id.c_str(), -1, SQLITE_TRANSIENT), impl_->database, "bind id");
  check_sqlite(sqlite3_step(statement.get()), impl_->database, "update preflight");
  return list_tasks().at(static_cast<std::size_t>(std::distance(tasks.begin(), it)));
}

TransferTask TaskControlService::execute(const std::string& task_id, bool delete_confirmed) {
  auto tasks = list_tasks();
  const auto it = std::find_if(tasks.begin(), tasks.end(), [&](const auto& task) { return task.id == task_id; });
  if (it == tasks.end() || it->state != TaskState::awaiting_execution_confirmation) throw std::runtime_error("task is not awaiting execution confirmation");
  if (it->delete_extraneous && !delete_confirmed)
    throw std::runtime_error("delete requires explicit destructive confirmation");
  const auto rsync = RsyncLocator{}.executable().string();
  std::vector<std::string> arguments{rsync, "--recursive", "--links", "--times", "--partial"};
  if (it->delete_extraneous) arguments.push_back("--delete");
  if (it->compression) arguments.push_back("--compress");
  const auto local_source = parse_endpoint(it->source);
  if (!local_source.remote) {
    for (const auto& exclusion : detect_project_profile(local_source.path).exclusions) {
      arguments.push_back("--exclude");
      arguments.push_back(exclusion);
    }
  }
  arguments.insert(arguments.end(), {"--", it->source, it->destination});
  auto process = ProcessRunner{}.start(arguments);
  {
    std::lock_guard lock{impl_->process_mutex};
    impl_->processes.emplace(task_id, std::move(process));
  }
  Statement statement{impl_->database, "UPDATE transfer_tasks SET state=? WHERE id=?"};
  check_sqlite(sqlite3_bind_text(statement.get(), 1, "running", -1, SQLITE_TRANSIENT), impl_->database, "bind state");
  check_sqlite(sqlite3_bind_text(statement.get(), 2, task_id.c_str(), -1, SQLITE_TRANSIENT), impl_->database, "bind id");
  check_sqlite(sqlite3_step(statement.get()), impl_->database, "update execution");
  return list_tasks().at(static_cast<std::size_t>(std::distance(tasks.begin(), it)));
}

TransferTask TaskControlService::execute_scp_fallback(const std::string& task_id) {
  auto tasks = list_tasks();
  const auto it = std::find_if(tasks.begin(), tasks.end(), [&](const auto& task) { return task.id == task_id; });
  if (it == tasks.end() || it->state != TaskState::failed)
    throw std::runtime_error("scp fallback is available only after rsync failure");
  auto process = ProcessRunner{}.start({RSYNC_ASSISTANT_SCP_PATH, "-r", "--", it->source, it->destination});
  {
    std::lock_guard lock{impl_->process_mutex};
    impl_->processes.emplace(task_id, std::move(process));
  }
  Statement statement{impl_->database, "UPDATE transfer_tasks SET state=?, command=?, output='' WHERE id=?"};
  check_sqlite(sqlite3_bind_text(statement.get(), 1, "running", -1, SQLITE_TRANSIENT), impl_->database, "bind state");
  const auto command = std::string{RSYNC_ASSISTANT_SCP_PATH} + " -r -- " + it->source + " " + it->destination;
  check_sqlite(sqlite3_bind_text(statement.get(), 2, command.c_str(), -1, SQLITE_TRANSIENT), impl_->database, "bind command");
  check_sqlite(sqlite3_bind_text(statement.get(), 3, task_id.c_str(), -1, SQLITE_TRANSIENT), impl_->database, "bind id");
  check_sqlite(sqlite3_step(statement.get()), impl_->database, "start scp fallback");
  const auto refreshed = list_tasks();
  return *std::find_if(refreshed.begin(), refreshed.end(), [&](const auto& task) { return task.id == task_id; });
}

TransferTask TaskControlService::pause(const std::string& task_id) {
  std::lock_guard lock{impl_->process_mutex};
  auto it = impl_->processes.find(task_id);
  if (it == impl_->processes.end()) throw std::runtime_error("task is not running");
  it->second.pause();
  sqlite3_exec(impl_->database, ("UPDATE transfer_tasks SET state='paused' WHERE id='" + task_id + "'").c_str(), nullptr, nullptr, nullptr);
  const auto refreshed = list_tasks();
  return *std::find_if(refreshed.begin(), refreshed.end(), [&](const auto& task) { return task.id == task_id; });
}

TransferTask TaskControlService::resume(const std::string& task_id) {
  std::lock_guard lock{impl_->process_mutex};
  auto it = impl_->processes.find(task_id);
  if (it == impl_->processes.end()) throw std::runtime_error("task is not paused");
  it->second.resume();
  sqlite3_exec(impl_->database, ("UPDATE transfer_tasks SET state='running' WHERE id='" + task_id + "'").c_str(), nullptr, nullptr, nullptr);
  const auto refreshed = list_tasks();
  return *std::find_if(refreshed.begin(), refreshed.end(), [&](const auto& task) { return task.id == task_id; });
}

TransferTask TaskControlService::stop(const std::string& task_id) {
  std::lock_guard lock{impl_->process_mutex};
  auto it = impl_->processes.find(task_id);
  if (it == impl_->processes.end()) throw std::runtime_error("task is not running");
  it->second.stop();
  impl_->processes.erase(it);
  sqlite3_exec(impl_->database, ("UPDATE transfer_tasks SET state='cancelled' WHERE id='" + task_id + "'").c_str(), nullptr, nullptr, nullptr);
  const auto refreshed = list_tasks();
  return *std::find_if(refreshed.begin(), refreshed.end(), [&](const auto& task) { return task.id == task_id; });
}

TransferTask TaskControlService::restart(const std::string& task_id) {
  const auto tasks = list_tasks();
  const auto it = std::find_if(tasks.begin(), tasks.end(), [&](const auto& task) { return task.id == task_id; });
  if (it == tasks.end() || (it->state != TaskState::failed && it->state != TaskState::interrupted))
    throw std::runtime_error("only failed or interrupted rsync tasks can be re-prepared");
  Statement statement{impl_->database,
                      "UPDATE transfer_tasks SET state='ready', command='', output='' WHERE id=?"};
  check_sqlite(sqlite3_bind_text(statement.get(), 1, task_id.c_str(), -1, SQLITE_TRANSIENT),
               impl_->database, "bind task id");
  check_sqlite(sqlite3_step(statement.get()), impl_->database, "restart task");
  const auto refreshed = list_tasks();
  return *std::find_if(refreshed.begin(), refreshed.end(), [&](const auto& task) { return task.id == task_id; });
}

TransferTask TaskControlService::await_completion(const std::string& task_id) {
  ProcessResult result;
  {
    std::lock_guard lock{impl_->process_mutex};
    auto it = impl_->processes.find(task_id);
    if (it == impl_->processes.end()) throw std::runtime_error("task is not running");
    result = it->second.wait();
    impl_->processes.erase(it);
  }
  const auto state = result.exit_code == 0 ? "completed" : "failed";
  Statement statement{impl_->database, "UPDATE transfer_tasks SET state=?, output=? WHERE id=?"};
  check_sqlite(sqlite3_bind_text(statement.get(), 1, state, -1, SQLITE_TRANSIENT), impl_->database, "bind state");
  check_sqlite(sqlite3_bind_text(statement.get(), 2, result.output.c_str(), -1, SQLITE_TRANSIENT), impl_->database, "bind output");
  check_sqlite(sqlite3_bind_text(statement.get(), 3, task_id.c_str(), -1, SQLITE_TRANSIENT), impl_->database, "bind id");
  check_sqlite(sqlite3_step(statement.get()), impl_->database, "update completion");
  const auto refreshed = list_tasks();
  const auto completed = *std::find_if(refreshed.begin(), refreshed.end(), [&](const auto& task) { return task.id == task_id; });
  persist_destination_log(completed);
  return completed;
}

}  // namespace rsync_assistant
