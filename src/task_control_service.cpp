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
                              "delete_extraneous INTEGER NOT NULL DEFAULT 0)",
                              nullptr, nullptr, nullptr),
                 database, "create transfer_tasks table");
    char* migration_error = nullptr;
    const auto migration = sqlite3_exec(
        database,
        "ALTER TABLE transfer_tasks ADD COLUMN delete_extraneous INTEGER NOT NULL DEFAULT 0",
        nullptr, nullptr, &migration_error);
    if (migration != SQLITE_OK) sqlite3_free(migration_error);
  }

  ~Impl() { sqlite3_close(database); }
};

TaskControlService::TaskControlService(const std::filesystem::path& database_path)
    : impl_(std::make_unique<Impl>(database_path)) {}

TaskControlService::~TaskControlService() = default;

TransferTask TaskControlService::create_ready_task(
    const CreateReadyTask& request) {
  const auto id = next_task_id();
  Statement statement{impl_->database,
                      "INSERT INTO transfer_tasks (id, source, destination, state, command, output, delete_extraneous) "
                      "VALUES (?, ?, ?, 'ready', '', '', ?)"};
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
  check_sqlite(sqlite3_step(statement.get()), impl_->database, "insert task");
  return {id, request.source, request.destination, TaskState::ready, "", "", request.delete_extraneous};
}

std::vector<TransferTask> TaskControlService::list_tasks() const {
  Statement statement{impl_->database,
                      "SELECT id, source, destination, state, command, output, delete_extraneous FROM transfer_tasks ORDER BY rowid"};
  std::vector<TransferTask> tasks;
  while (sqlite3_step(statement.get()) == SQLITE_ROW) {
    const std::string state{reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 3))};
    const auto task_state = state == "ready" ? TaskState::ready :
                            state == "awaiting_confirmation" ? TaskState::awaiting_execution_confirmation :
                            state == "running" ? TaskState::running :
                            state == "paused" ? TaskState::paused :
                            state == "completed" ? TaskState::completed :
                            state == "cancelled" ? TaskState::cancelled : TaskState::failed;
    tasks.push_back({
        reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 0)),
        reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 1)),
        reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 2)),
        task_state,
        reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 4)),
        reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 5)),
        sqlite3_column_int(statement.get(), 6) != 0,
    });
  }
  return tasks;
}

std::string TaskControlService::execution_log(const std::string& task_id) const {
  Statement statement{impl_->database,
                      "SELECT output FROM transfer_tasks WHERE id = ?"};
  check_sqlite(sqlite3_bind_text(statement.get(), 1, task_id.c_str(), -1,
                                 SQLITE_TRANSIENT),
               impl_->database, "bind task id");
  if (sqlite3_step(statement.get()) != SQLITE_ROW)
    throw std::runtime_error("unknown task");
  const auto* output = sqlite3_column_text(statement.get(), 0);
  return output == nullptr ? "" : reinterpret_cast<const char*>(output);
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
  std::vector<std::string> arguments{rsync, "--recursive", "--links", "--times", "--partial", "--dry-run"};
  if (it->delete_extraneous) arguments.push_back("--delete");
  if (!source_endpoint.remote) {
    for (const auto& exclusion : detect_project_profile(source_endpoint.path).exclusions) {
      arguments.push_back("--exclude");
      arguments.push_back(exclusion);
    }
  }
  arguments.insert(arguments.end(), {"--", it->source, it->destination});
  const auto result = ProcessRunner{}.run(arguments);
  const auto state = result.exit_code == 0 ? "awaiting_confirmation" : "failed";
  const auto command = rsync + " --recursive --links --times --partial --dry-run" +
                       std::string{it->delete_extraneous ? " --delete" : ""} +
                       " -- " + it->source + " " + it->destination;
  Statement statement{impl_->database, "UPDATE transfer_tasks SET state=?, command=?, output=? WHERE id=?"};
  check_sqlite(sqlite3_bind_text(statement.get(), 1, state, -1, SQLITE_TRANSIENT), impl_->database, "bind state");
  check_sqlite(sqlite3_bind_text(statement.get(), 2, command.c_str(), -1, SQLITE_TRANSIENT), impl_->database, "bind command");
  check_sqlite(sqlite3_bind_text(statement.get(), 3, result.output.c_str(), -1, SQLITE_TRANSIENT), impl_->database, "bind output");
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
  return *std::find_if(refreshed.begin(), refreshed.end(), [&](const auto& task) { return task.id == task_id; });
}

}  // namespace rsync_assistant
