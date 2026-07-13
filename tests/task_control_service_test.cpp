#include "rsync_assistant/task_control_service.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

int main() {
  const auto unique = std::to_string(std::chrono::steady_clock::now()
                                         .time_since_epoch()
                                         .count());
  const auto database = std::filesystem::temp_directory_path() /
                        ("rsync-assistant-task-control-" + unique + ".sqlite3");

  {
    rsync_assistant::TaskControlService service{database};
    const auto created = service.create_ready_task({"/source", "/destination"});

    assert(created.state == rsync_assistant::TaskState::ready);
    assert(created.source == "/source");
    assert(created.destination == "/destination");
    assert(created.command.find("--partial") != std::string::npos);
    assert(service.execution_log(created.id).find("Command proposal") != std::string::npos);

    const auto tasks = service.list_tasks();
    assert(tasks.size() == 1);
    assert(tasks.front().id == created.id);
  }

  {
    rsync_assistant::TaskControlService reopened{database};
    const auto tasks = reopened.list_tasks();
    assert(tasks.size() == 1);
    assert(tasks.front().state == rsync_assistant::TaskState::ready);
  }

  std::filesystem::remove(database);

  const auto transfer_root = std::filesystem::temp_directory_path() /
                             ("rsync-assistant-transfer-" + unique);
  const auto source = transfer_root / "source";
  const auto destination = transfer_root / "destination";
  std::filesystem::create_directories(source);
  std::filesystem::create_directories(destination);
  std::ofstream{source / "payload.txt"} << "payload";

  rsync_assistant::TaskControlService transfer_service{transfer_root / "tasks.sqlite3"};
  const auto transfer_task = transfer_service.create_ready_task(
      {source.string() + "/", destination.string() + "/"});
  const auto preview = transfer_service.preflight(transfer_task.id);
  assert(preview.state == rsync_assistant::TaskState::awaiting_execution_confirmation);
  assert(!std::filesystem::exists(destination / "payload.txt"));
  (void)transfer_service.execute(transfer_task.id);
  const auto finished = transfer_service.await_completion(transfer_task.id);
  assert(finished.state == rsync_assistant::TaskState::completed);
  assert(finished.exit_code && *finished.exit_code == 0);
  assert(std::filesystem::exists(destination / "payload.txt"));
  assert(transfer_service.execution_log(transfer_task.id).find("Exit status: 0") != std::string::npos);

  std::filesystem::create_directories(source / "nested");
  std::ofstream{source / "nested" / "chosen.txt"} << "chosen";
  std::ofstream{source / "not-chosen.txt"} << "not chosen";
  const auto selected_destination = transfer_root / "selected";
  std::filesystem::create_directories(selected_destination);
  rsync_assistant::CreateReadyTask selected_request{source.string() + "/", selected_destination.string() + "/"};
  selected_request.selected_relative_paths = {"nested/chosen.txt"};
  const auto selected_task = transfer_service.create_ready_task(selected_request);
  (void)transfer_service.preflight(selected_task.id);
  (void)transfer_service.execute(selected_task.id);
  (void)transfer_service.await_completion(selected_task.id);
  assert(std::filesystem::exists(selected_destination / "nested" / "chosen.txt"));
  assert(!std::filesystem::exists(selected_destination / "not-chosen.txt"));

  rsync_assistant::CreateReadyTask remote_selection{"backup:/srv/source", selected_destination.string()};
  remote_selection.selected_relative_paths = {"project/file.txt", "notes.md"};
  const auto remote_selection_task = transfer_service.create_ready_task(remote_selection);
  assert(remote_selection_task.selected_path_count == 2);
  assert(remote_selection_task.command.find("--files-from=") != std::string::npos);
  bool rejected_cross_host_selection = false;
  try {
    remote_selection.selected_relative_paths = {"other-host:/srv/other"};
    (void)transfer_service.create_ready_task(remote_selection);
  } catch (const std::invalid_argument&) {
    rejected_cross_host_selection = true;
  }
  assert(rejected_cross_host_selection);

  std::filesystem::create_directories(source / ".git");
  std::ofstream{source / ".git" / "config"} << "git metadata";
  const auto project_destination = transfer_root / "project";
  std::filesystem::create_directories(project_destination);
  const auto project_task = transfer_service.create_ready_task(
      {source.string() + "/", project_destination.string() + "/"});
  (void)transfer_service.preflight(project_task.id);
  (void)transfer_service.execute(project_task.id);
  (void)transfer_service.await_completion(project_task.id);
  assert(!std::filesystem::exists(project_destination / ".git" / "config"));

  std::ofstream{source / "CMakeLists.txt"} << "project(test)";
  std::filesystem::create_directories(source / "build");
  std::ofstream{source / "build" / "artifact"} << "artifact";
  const auto include_build_destination = transfer_root / "include-build";
  std::filesystem::create_directories(include_build_destination);
  rsync_assistant::CreateReadyTask include_build_request{source.string() + "/", include_build_destination.string() + "/"};
  include_build_request.include_project_ignored = true;
  const auto include_build_task = transfer_service.create_ready_task(include_build_request);
  (void)transfer_service.preflight(include_build_task.id);
  (void)transfer_service.execute(include_build_task.id);
  (void)transfer_service.await_completion(include_build_task.id);
  assert(std::filesystem::exists(include_build_destination / "build" / "artifact"));

  std::ofstream{destination / "stale.txt"} << "stale";
  const auto deleting_task = transfer_service.create_ready_task(
      {source.string() + "/", destination.string() + "/", true});
  (void)transfer_service.preflight(deleting_task.id);
  bool rejected_without_confirmation = false;
  try {
    (void)transfer_service.execute(deleting_task.id);
  } catch (const std::runtime_error&) {
    rejected_without_confirmation = true;
  }
  assert(rejected_without_confirmation);
  (void)transfer_service.execute(deleting_task.id, true);
  (void)transfer_service.await_completion(deleting_task.id);
  assert(!std::filesystem::exists(destination / "stale.txt"));

  bool rejected_delete_without_dry_run = false;
  try {
    (void)transfer_service.create_ready_task(
        {source.string(), destination.string(), true, false, false});
  } catch (const std::invalid_argument&) {
    rejected_delete_without_dry_run = true;
  }
  assert(rejected_delete_without_dry_run);

  rsync_assistant::CreateReadyTask untrusted_daemon_request{source.string(), "untrusted-host::module"};
  const auto untrusted_daemon_task = transfer_service.create_ready_task(untrusted_daemon_request);
  bool rejected_untrusted_daemon = false;
  try {
    (void)transfer_service.preflight(untrusted_daemon_task.id);
  } catch (const std::runtime_error&) {
    rejected_untrusted_daemon = true;
  }
  assert(rejected_untrusted_daemon);
  const auto after_failed_preflight = transfer_service.list_tasks();
  const auto failed_task = std::find_if(after_failed_preflight.begin(), after_failed_preflight.end(),
                                        [&](const auto& task) { return task.id == untrusted_daemon_task.id; });
  assert(failed_task != after_failed_preflight.end());
  assert(failed_task->state == rsync_assistant::TaskState::failed);
  assert(transfer_service.execution_log(untrusted_daemon_task.id).find("trusted-LAN authorization") != std::string::npos);
  std::filesystem::remove_all(transfer_root);
}
