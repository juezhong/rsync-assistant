#include "rsync_assistant/task_control_socket.hpp"
#include "rsync_assistant/directory_scanner.hpp"
#include "rsync_assistant/endpoint.hpp"
#include "rsync_assistant/process_runner.hpp"
#include "rsync_assistant/project_profile.hpp"
#include "rsync_assistant/rsync_locator.hpp"
#include "rsync_assistant/settings.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <chrono>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <exception>
#include <fcntl.h>
#include <sys/file.h>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <functional>
#include <signal.h>
#include <unistd.h>

namespace {
volatile sig_atomic_t daemon_stop_requested = 0;

void request_daemon_stop(int) { daemon_stop_requested = 1; }

class TuiSessionLock {
 public:
  explicit TuiSessionLock(const std::filesystem::path& state_dir)
      : path_(state_dir / "tui-session.lock") {
    std::filesystem::create_directories(state_dir);
    descriptor_ = open(path_.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (descriptor_ < 0 && errno == EEXIST) {
      std::ifstream existing{path_};
      pid_t owner = -1;
      existing >> owner;
      if (owner <= 0 || (kill(owner, 0) != 0 && errno == ESRCH)) {
        std::filesystem::remove(path_);
        descriptor_ = open(path_.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
      }
    }
    if (descriptor_ < 0)
      throw std::runtime_error("another rsync-assistant TUI session is already active");
    const auto pid = std::to_string(getpid()) + "\n";
    if (write(descriptor_, pid.data(), pid.size()) != static_cast<ssize_t>(pid.size())) {
      close(descriptor_);
      descriptor_ = -1;
      std::filesystem::remove(path_);
      throw std::runtime_error("cannot initialize TUI session lock");
    }
  }

  ~TuiSessionLock() {
    if (descriptor_ >= 0) close(descriptor_);
    std::error_code error;
    std::filesystem::remove(path_, error);
  }

  TuiSessionLock(const TuiSessionLock&) = delete;
  TuiSessionLock& operator=(const TuiSessionLock&) = delete;

 private:
  std::filesystem::path path_;
  int descriptor_ = -1;
};

class DaemonLock {
 public:
  explicit DaemonLock(const std::filesystem::path& state_dir)
      : path_(state_dir / "daemon.lock") {
    std::filesystem::create_directories(state_dir);
    descriptor_ = open(path_.c_str(), O_CREAT | O_RDWR, 0600);
    if (descriptor_ < 0 || flock(descriptor_, LOCK_EX | LOCK_NB) != 0) {
      if (descriptor_ >= 0) close(descriptor_);
      descriptor_ = -1;
      throw std::runtime_error("another rsync-assistant daemon already owns this state directory");
    }
  }
  ~DaemonLock() {
    if (descriptor_ >= 0) {
      flock(descriptor_, LOCK_UN);
      close(descriptor_);
    }
  }
  DaemonLock(const DaemonLock&) = delete;
  DaemonLock& operator=(const DaemonLock&) = delete;
 private:
  std::filesystem::path path_;
  int descriptor_ = -1;
};

std::filesystem::path state_directory(int argc, char* argv[]) {
  if (argc >= 3 && std::string_view{argv[argc - 2]} == "--state-dir") return argv[argc - 1];
  return std::filesystem::temp_directory_path() / "rsync-assistant";
}

void write_network_rsyncd_template(const std::filesystem::path& path) {
  if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
  std::ofstream output{path, std::ios::trunc};
  output << "# Review binding, authentication and firewall rules before starting rsync daemon.\n"
         << "# Do not expose this daemon on untrusted networks.\n"
         << "use chroot = no\n"
         << "read only = false\n"
         << "auth users = REPLACE_ME\n"
         << "secrets file = REPLACE_WITH_OWNER_ONLY_SECRETS_FILE\n"
         << "hosts allow = REPLACE_WITH_TRUSTED_SUBNET\n\n"
         << "[transfer]\n"
         << "path = REPLACE_WITH_TRANSFER_ROOT\n"
         << "comment = rsync-assistant managed transfer root\n";
  if (!output) throw std::runtime_error("cannot write rsync daemon configuration");
}

std::string quote_preview(const std::string& value) {
  if (value.find_first_of(" \t'\"\\$") == std::string::npos) return value;
  std::string quoted{"'"};
  for (const char character : value) quoted += character == '\'' ? "'\\''" : std::string(1, character);
  return quoted + "'";
}

std::string draft_command_proposal(const std::string& source, const std::string& destination,
                                   bool delete_extraneous, bool compression,
                                   bool include_git_data, bool has_selection,
                                   bool flatten_selection, bool include_project_ignored) {
  if (source.empty() || destination.empty()) return "Complete source and destination to generate a command proposal.";
  std::vector<std::string> arguments{rsync_assistant::RsyncLocator{}.executable().string(),
                                     "--recursive", "--links", "--times", "--partial"};
  if (delete_extraneous) arguments.push_back("--delete");
  if (compression) arguments.push_back("--compress");
  const auto endpoint = rsync_assistant::parse_endpoint(source);
  if (!endpoint.remote) {
    const auto profile = rsync_assistant::detect_project_profile(endpoint.path);
    if (!include_project_ignored)
      for (const auto& exclusion : profile.exclusions) {
        arguments.push_back("--exclude");
        arguments.push_back(exclusion);
      }
    if (profile.has_git_repository && !include_git_data) {
      arguments.push_back("--exclude");
      arguments.push_back(".git/");
    }
  }
  if (has_selection) {
    arguments.push_back("--from0");
    arguments.push_back("--files-from=<assistant-managed-selection-manifest>");
    if (flatten_selection) arguments.push_back("--no-relative");
  }
  arguments.insert(arguments.end(), {"--", source, destination});
  std::string result;
  for (const auto& argument : arguments) {
    if (!result.empty()) result += ' ';
    result += quote_preview(argument);
  }
  return result;
}

int clamp_selection(int selection, std::size_t count) {
  if (count == 0) return 0;
  return std::clamp(selection, 0, static_cast<int>(count) - 1);
}

bool valid_benchmark_token(std::string_view token) {
  return !token.empty() && token.size() <= 80 && std::all_of(token.begin(), token.end(), [](unsigned char character) {
    return std::isalnum(character) || character == '-' || character == '_';
  });
}

int run_daemon(const std::filesystem::path& state_dir,
               const rsync_assistant::Settings& settings) {
  std::filesystem::create_directories(state_dir);
  DaemonLock daemon_lock{state_dir};
  const auto daemon_root = state_dir / "managed-rsync-root";
  const auto daemon_config = state_dir / "managed-rsyncd.conf";
  std::filesystem::create_directories(daemon_root);
  const auto port = 20000 + static_cast<unsigned>(std::hash<std::string>{}(state_dir.string()) % 20000);
  {
    std::ofstream output{daemon_config, std::ios::trunc};
    if (!output) throw std::runtime_error("cannot write managed loopback rsync daemon configuration");
    output << "address = 127.0.0.1\n"
           << "port = " << port << "\n"
           << "use chroot = no\n"
           << "read only = false\n\n"
           << "[rsync-assistant-loopback]\n"
           << "path = " << daemon_root.string() << "\n"
           << "comment = assistant lifecycle-bound loopback-only module\n";
  }
  std::filesystem::permissions(daemon_config,
                               std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                               std::filesystem::perm_options::replace);
  rsync_assistant::ManagedProcess loopback_rsync;
  try {
    loopback_rsync = rsync_assistant::ProcessRunner{}.start({
        rsync_assistant::RsyncLocator{}.executable().string(), "--daemon", "--no-detach",
        "--config=" + daemon_config.string(), "--address=127.0.0.1", "--port=" + std::to_string(port)});
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if (const auto result = loopback_rsync.try_wait())
      std::cerr << "rsync-assistant: managed loopback rsync daemon unavailable: " << result->output;
  } catch (const std::exception& error) {
    std::cerr << "rsync-assistant: managed loopback rsync daemon unavailable: " << error.what() << '\n';
  }
  rsync_assistant::TaskControlService service{state_dir / "tasks.sqlite3", settings};
  rsync_assistant::TaskControlSocketServer server{service, state_dir / "control.sock"};
  daemon_stop_requested = 0;
  signal(SIGINT, request_daemon_stop);
  signal(SIGTERM, request_daemon_stop);
  std::exception_ptr serve_error;
  std::thread server_thread{[&] {
    try { server.serve(); }
    catch (...) { serve_error = std::current_exception(); daemon_stop_requested = 1; }
  }};
  while (!daemon_stop_requested)
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  server.stop();
  server_thread.join();
  if (serve_error) std::rethrow_exception(serve_error);
  return 0;
}

int run_tui(const std::filesystem::path& state_dir,
            rsync_assistant::Settings settings,
            const std::filesystem::path& settings_path) {
  TuiSessionLock session_lock{state_dir};
  rsync_assistant::TaskControlSocketClient client{state_dir / "control.sock"};
  auto screen = ftxui::ScreenInteractive::Fullscreen();
  std::string status = "r: refresh  d: deployment guide  q: quit";
  std::string selected_log;
  std::string remote_task_status = "R: inspect assistant-enabled remote task status";
  std::vector<rsync_assistant::TransferTask> tasks;
  int selected = 0;
  bool creating = false;
  int wizard_step = 0;
  std::string source;
  std::string destination;
  std::vector<std::string> selected_source_paths;
  bool flatten_selection = false;
  bool copy_source_contents = false;
  bool create_destination_parents = false;
  bool delete_extraneous = false;
  bool compression = settings.compression;
  bool dry_run = settings.dry_run;
  bool trusted_daemon = false;
  bool source_has_git_repository = false;
  bool include_git_data = false;
  bool source_has_project_ignores = false;
  bool include_project_ignored = false;
  bool delete_confirming = false;
  std::string delete_confirmation;
  bool scp_confirming = false;
  bool settings_open = false;
  bool help_open = false;
  std::string scp_confirmation;
  bool browsing = false;
  std::filesystem::path browse_directory = std::filesystem::current_path();
  std::vector<rsync_assistant::PathEntry> browse_entries;
  std::future<std::pair<unsigned, std::vector<rsync_assistant::PathEntry>>> browse_scan;
  bool browse_scanning = false;
  unsigned browse_scan_generation = 0;
  int browse_selected = 0;
  bool browse_hidden = false;
  bool browse_include_ignored = false;
  bool browse_search = false;
  std::string browse_query;
  bool browse_destination = false;
  bool browse_remote = false;
  std::filesystem::path browse_root;
  std::unordered_set<std::string> browse_selected_paths;
  std::string browse_remote_prefix;
  std::string browse_remote_host;
  std::string browse_manual_host;
  std::string browse_manual_path;
  std::vector<std::string> browse_ssh_hosts;
  std::filesystem::path browse_remote_directory;
  auto source_input = ftxui::Input(&source, "Source path");
  auto destination_input = ftxui::Input(&destination, "Destination path");
  auto delete_checkbox = ftxui::Checkbox("Delete destination-only files (--delete)", &delete_extraneous);
  auto compression_checkbox = ftxui::Checkbox("Compress transfer (--compress)", &compression);
  auto dry_run_checkbox = ftxui::Checkbox("Dry-run before execution", &dry_run);
  auto trusted_daemon_checkbox = ftxui::Checkbox("Trust direct rsync daemon on LAN", &trusted_daemon);
  auto copy_source_contents_checkbox = ftxui::Checkbox("Copy directory contents only (project/ -> destination/...)", &copy_source_contents);
  auto create_destination_parents_checkbox = ftxui::Checkbox("Create missing Destination parent directories", &create_destination_parents);
  auto include_git_checkbox = ftxui::Checkbox("Include repository data (.git)", &include_git_data);
  auto include_project_ignored_checkbox = ftxui::Checkbox("Include detected build/dependency paths", &include_project_ignored);
  auto browse_search_input = ftxui::Input(&browse_query, "Search paths");
  auto browse_manual_host_input = ftxui::Input(&browse_manual_host, "Host or user@host");
  auto browse_manual_path_input = ftxui::Input(&browse_manual_path, "Absolute remote path (optional)");
  auto delete_confirmation_input = ftxui::Input(&delete_confirmation, "Type DELETE");
  auto scp_confirmation_input = ftxui::Input(&scp_confirmation, "Type SCP");
  auto settings_dry_run = ftxui::Checkbox("Default dry-run", &settings.dry_run);
  auto settings_compression = ftxui::Checkbox("Default compression", &settings.compression);
  auto settings_benchmark = ftxui::Checkbox("Enable benchmarks", &settings.benchmark_enabled);
  std::string settings_benchmark_size = std::to_string(settings.benchmark_size_mib);
  std::string settings_benchmark_timeout = std::to_string(settings.benchmark_timeout_seconds);
  std::string settings_benchmark_cache = std::to_string(settings.benchmark_cache_hours);
  std::string settings_benchmark_threshold = std::to_string(settings.daemon_advantage_threshold);
  auto settings_benchmark_size_input = ftxui::Input(&settings_benchmark_size, "MiB");
  auto settings_benchmark_timeout_input = ftxui::Input(&settings_benchmark_timeout, "seconds");
  auto settings_benchmark_cache_input = ftxui::Input(&settings_benchmark_cache, "hours");
  auto settings_benchmark_threshold_input = ftxui::Input(&settings_benchmark_threshold, "ratio");
  auto settings_ai_enabled = ftxui::Checkbox("Enable AI explanations", &settings.ai_enabled);
  auto settings_ai_endpoint = ftxui::Input(&settings.ai_endpoint, "AI endpoint");
  auto settings_ai_model = ftxui::Input(&settings.ai_model, "AI model");
  auto settings_api_key = ftxui::Input(&settings.api_key, "API key");
  auto source_form = ftxui::Container::Vertical({source_input});
  auto destination_form = ftxui::Container::Vertical({destination_input});
  auto option_form = ftxui::Container::Vertical({copy_source_contents_checkbox, create_destination_parents_checkbox, dry_run_checkbox, compression_checkbox, delete_checkbox,
                                                   trusted_daemon_checkbox, include_git_checkbox,
                                                   include_project_ignored_checkbox});
  auto delete_form = ftxui::Container::Vertical({delete_confirmation_input});
  auto scp_form = ftxui::Container::Vertical({scp_confirmation_input});
  auto settings_form = ftxui::Container::Vertical({settings_dry_run, settings_compression,
                                                    settings_benchmark, settings_benchmark_size_input,
                                                    settings_benchmark_timeout_input, settings_benchmark_cache_input,
                                                    settings_benchmark_threshold_input, settings_ai_enabled,
                                                    settings_ai_endpoint, settings_ai_model, settings_api_key});
  auto inactive_form = ftxui::Renderer([] { return ftxui::text(""); });
  auto review_form = ftxui::Renderer([] { return ftxui::text(""); });
  // Only the controls rendered by the active modal participate in focus navigation.
  int active_form = 0;
  auto form = ftxui::Container::Tab({inactive_form, source_form, destination_form, option_form,
                                     review_form, delete_form, scp_form, settings_form}, &active_form);
  auto scan_browser = [&] {
    if (browse_scanning) return;
    const auto directory = browse_directory;
    const auto root = browse_root;
    const auto search = browse_search;
    const auto query = browse_query;
    const auto hidden = browse_hidden;
    const auto include_ignored = browse_include_ignored;
    const auto generation = ++browse_scan_generation;
    browse_scanning = true;
    browse_scan = std::async(std::launch::async, [directory, root, search, query, hidden, include_ignored, generation] {
      std::vector<rsync_assistant::PathEntry> entries;
      if (search && !query.empty()) {
        for (const auto& path : rsync_assistant::search_paths(root, query, hidden))
          entries.push_back({path, std::filesystem::is_directory(path), false});
      } else {
        entries = rsync_assistant::scan_directory_level(directory, hidden);
      }
      if (!include_ignored) {
        const auto profile = rsync_assistant::detect_project_profile(root);
        entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const auto& entry) {
          std::error_code error;
          const auto relative = std::filesystem::relative(entry.path, root, error).generic_string();
          if (error) return false;
          if (relative == ".git" || relative.starts_with(".git/")) return true;
          for (const auto& exclusion : profile.exclusions) {
            const auto prefix = exclusion.ends_with('/') ? exclusion.substr(0, exclusion.size() - 1) : exclusion;
            if (relative == prefix || relative.starts_with(prefix + "/")) return true;
          }
          return false;
        }), entries.end());
      }
      return std::pair{generation, std::move(entries)};
    });
  };
  auto collect_browser_scan = [&] {
    if (!browse_scanning || browse_scan.wait_for(std::chrono::milliseconds{0}) != std::future_status::ready) return;
    try {
      auto [generation, entries] = browse_scan.get();
      if (generation == browse_scan_generation) {
        browse_entries = std::move(entries);
        browse_selected = clamp_selection(browse_selected, browse_entries.size());
      }
    } catch (const std::exception& error) { status = error.what(); }
    browse_scanning = false;
  };
  auto refresh = [&] {
    try {
      tasks = client.list_tasks();
      selected = clamp_selection(selected, tasks.size());
      selected_log = tasks.empty() ? "No selected task" : client.execution_log(tasks.at(selected).id);
      std::string task_lines;
      const auto label = [](rsync_assistant::TaskState state) {
        if (state == rsync_assistant::TaskState::ready) return "Ready";
        if (state == rsync_assistant::TaskState::preflighting) return "Preflighting";
        if (state == rsync_assistant::TaskState::awaiting_execution_confirmation) return "Confirm";
        if (state == rsync_assistant::TaskState::running) return "Running";
        if (state == rsync_assistant::TaskState::paused) return "Paused";
        if (state == rsync_assistant::TaskState::completed) return "Completed";
        if (state == rsync_assistant::TaskState::cancelled) return "Cancelled";
        if (state == rsync_assistant::TaskState::interrupted) return "Interrupted";
        return "Failed";
      };
      const auto method_label = [](rsync_assistant::TransferMethod method) {
        if (method == rsync_assistant::TransferMethod::rsync_daemon) return "daemon";
        if (method == rsync_assistant::TransferMethod::rsync_ssh) return "rsync-ssh";
        if (method == rsync_assistant::TransferMethod::scp) return "scp";
        return "local";
      };
      for (std::size_t index = 0; index < tasks.size(); ++index)
        task_lines += (static_cast<int>(index) == selected ? "> " : "  ") +
                      std::string{label(tasks[index].state)} + " " + tasks[index].id +
                      (tasks[index].exit_code ? " (exit " + std::to_string(*tasks[index].exit_code) + ")" : "") + "\n";
      for (std::size_t index = 0; index < tasks.size(); ++index) {
        const auto marker = static_cast<int>(index) == selected ? "> " : "  ";
        task_lines += marker + std::string{"  via "} + method_label(tasks[index].method) + "\n";
      }
      if (task_lines.empty()) task_lines = "No tasks yet";
      status = "r: refresh  q: quit\n\n" + task_lines;
    } catch (const std::exception& error) {
      status = std::string{"Daemon unavailable: "} + error.what();
    }
  };
  refresh();
  auto root = ftxui::Renderer(form, [&] {
    auto dashboard = ftxui::hbox({
        ftxui::vbox({ftxui::text("Tasks") | ftxui::bold, ftxui::separator(),
                     ftxui::paragraph(status)}) |
            ftxui::border | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 30),
        ftxui::vbox({ftxui::text("Current task") | ftxui::bold, ftxui::separator(),
                     ftxui::paragraph(selected_log.empty() ? "No execution output yet" : selected_log),
                     ftxui::separator(), ftxui::text("Program log"),
                     ftxui::text("Daemon connected through local socket")}) |
            ftxui::border | ftxui::flex,
        ftxui::vbox({ftxui::text("Details") | ftxui::bold,
                     ftxui::separator(), ftxui::text("Task actions") | ftxui::bold,
                     ftxui::text("Enter  preflight / execute"),
                     ftxui::text("p      pause / resume"),
                     ftxui::text("x      stop"),
                     ftxui::text("w      wait for completion"),
                     ftxui::text("e      re-prepare failed task"),
                     ftxui::text("c      use scp after rsync failure"),
                     ftxui::separator(), ftxui::text("Navigation") | ftxui::bold,
                     ftxui::text("N      new task"),
                     ftxui::text("S      settings"),
                     ftxui::text("D      daemon deployment guide"),
                     ftxui::text("R      remote task status"),
                     ftxui::text("?      complete shortcut help"),
                     ftxui::separator(), ftxui::paragraph(remote_task_status)}) |
            ftxui::border | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 28),
    });
    if (help_open) {
      return ftxui::dbox({dashboard | ftxui::dim,
                          ftxui::window(ftxui::text("Keyboard shortcuts"),
                                        ftxui::vbox({ftxui::text("Dashboard" ) | ftxui::bold,
                                                     ftxui::text("Enter preflight/execute   p pause/resume   x stop   w wait"),
                                                     ftxui::text("e re-prepare   c scp fallback   n new task   s settings"),
                                                     ftxui::text("d deployment guide   R remote task status   q quit"),
                                                     ftxui::separator(),
                                                     ftxui::text("Path picker") | ftxui::bold,
                                                     ftxui::text("J/K or arrows move   H parent   L enter directory"),
                                                     ftxui::text("Space toggle source item   Enter confirm   Esc cancel"),
                                                     ftxui::separator(), ftxui::text("Esc: close")})) | ftxui::center});
    }
    if (delete_confirming) {
      return ftxui::dbox({dashboard | ftxui::dim,
                          ftxui::window(ftxui::text("Confirm destination deletion"),
                                        ftxui::vbox({ftxui::text("Type DELETE then press Enter"),
                                                     delete_confirmation_input->Render()})) | ftxui::center});
    }
    if (scp_confirming) {
      return ftxui::dbox({dashboard | ftxui::dim,
                          ftxui::window(ftxui::text("Confirm scp fallback"),
                                        ftxui::vbox({ftxui::text("scp has no rsync dry-run or resume guarantee."),
                                                     ftxui::text("Type SCP then press Enter"),
                                                     scp_confirmation_input->Render()})) | ftxui::center});
    }
    if (settings_open) {
      return ftxui::dbox({dashboard | ftxui::dim,
                          ftxui::window(ftxui::text("Settings"),
                                        ftxui::vbox({settings_dry_run->Render(), settings_compression->Render(),
                                                     settings_benchmark->Render(), ftxui::text("Benchmark payload MiB:"), settings_benchmark_size_input->Render(),
                                                     ftxui::text("Benchmark timeout seconds:"), settings_benchmark_timeout_input->Render(),
                                                     ftxui::text("Benchmark cache hours:"), settings_benchmark_cache_input->Render(),
                                                     ftxui::text("Daemon advantage ratio:"), settings_benchmark_threshold_input->Render(),
                                                     settings_ai_enabled->Render(),
                                                     settings_ai_endpoint->Render(), settings_ai_model->Render(),
                                                     settings_api_key->Render(), ftxui::separator(),
                                                     ftxui::text("Ctrl-S: save  Esc: close"),
                                                     ftxui::text(settings_path.string())})) | ftxui::center});
    }
    if (!creating) return dashboard;
    if (browsing) {
      collect_browser_scan();
      std::string entries = browse_destination ? "h: parent  l: enter  /: search  g: hidden  i: ignored  Enter: select\n\n" :
                                                 "h: parent  l: enter  /: search  g: hidden  i: ignored  Space: toggle  F: flatten  Enter: confirm\n\n";
      if (browse_search) entries += "Search: " + browse_query + " (Enter: apply, Esc: cancel)\n";
      if (browse_scanning) entries += "Scanning…\n";
      if (browse_remote && browse_remote_host.empty()) {
        entries += "SSH Host (choose with j/k + Enter, or type manual host below):\n";
        for (std::size_t index = 0; index < browse_ssh_hosts.size(); ++index)
          entries += std::string{static_cast<int>(index) == browse_selected ? "> " : "  "} + browse_ssh_hosts[index] + "\n";
      }
      entries += browse_directory.string() + "\n";
      for (std::size_t index = 0; index < browse_entries.size(); ++index)
        entries += std::string{static_cast<int>(index) == browse_selected ? "> " : "  "} +
                   (!browse_destination && browse_selected_paths.contains(browse_entries[index].path.string()) ? "[x] " : "[ ] ") +
                   browse_entries[index].path.filename().string() +
                   (browse_entries[index].directory ? "/\n" : "\n");
      if (!browse_destination && !browse_remote)
        entries += "\nRoot: " + browse_root.string() + "\nStructure: " + (flatten_selection ? "flatten" : "preserve relative paths");
      return ftxui::dbox({dashboard | ftxui::dim,
                          ftxui::window(ftxui::text("Select source path"),
                                        ftxui::vbox({ftxui::paragraph(entries),
                                                     browse_remote && browse_remote_host.empty() ? browse_manual_host_input->Render() : ftxui::text(""),
                                                     browse_remote && !browse_remote_host.empty() ? browse_manual_path_input->Render() : ftxui::text(""),
                                                     browse_remote && !browse_remote_host.empty() ? ftxui::text("Enter an absolute path above, or select entries below.") : ftxui::text(" ")})) | ftxui::center});
    }
    ftxui::Elements contents;
    if (wizard_step == 0) {
      contents = {ftxui::text("Step 1/4: Source"), source_input->Render(),
                  ftxui::text("The Source is read from. It may be local or remote."),
                  ftxui::text("F2: browse local source  F3: browse remote source"),
                  ftxui::text("Enter: next  Esc: cancel")};
    } else if (wizard_step == 1) {
      contents = {ftxui::text("Step 2/4: Destination"), destination_input->Render(),
                  ftxui::text("The Destination is the single directory written to."),
                  ftxui::text("F2: browse local destination  F3: browse remote destination"),
                  ftxui::text("Enter: next  F4: previous  Esc: cancel")};
    } else if (wizard_step == 2) {
      contents = {ftxui::text("Step 3/4: transfer options"),
                  ftxui::text("Directory Source mode: preserve directory level is the default."),
                  copy_source_contents_checkbox->Render(), create_destination_parents_checkbox->Render(), dry_run_checkbox->Render(),
                  compression_checkbox->Render(), delete_checkbox->Render(), trusted_daemon_checkbox->Render(),
                  source_has_git_repository ? include_git_checkbox->Render() : ftxui::text("No .git repository detected at source root"),
                  source_has_project_ignores ? include_project_ignored_checkbox->Render() : ftxui::text("No detected project build/dependency paths"), ftxui::separator(),
                  ftxui::text("Enter: review  F4: previous  Esc: cancel")};
    } else {
      contents = {ftxui::text("Step 4/4: review"),
                  ftxui::text("Source: " + source), ftxui::text("Destination: " + destination),
                  ftxui::text(copy_source_contents ? "Result: source contents enter Destination directly" :
                                                    "Result: source directory level is preserved under Destination"),
                  ftxui::text(std::string{"Dry-run: "} + (dry_run ? "enabled" : "disabled")),
                  ftxui::text(std::string{"Compression: "} + (compression ? "enabled" : "disabled")),
                  ftxui::text(std::string{"Deletion: "} + (delete_extraneous ? "enabled" : "disabled")),
                  ftxui::text(selected_source_paths.empty() ? "Selection: whole source" :
                              "Selection: " + std::to_string(selected_source_paths.size()) + " entries (" + (flatten_selection ? "flatten" : "preserve paths") + ")"),
                  ftxui::separator(), ftxui::text("Command proposal:"),
                  ftxui::paragraph(draft_command_proposal(copy_source_contents && selected_source_paths.empty() && !source.ends_with('/') ? source + "/" : source, destination, delete_extraneous, compression, include_git_data,
                                                          !selected_source_paths.empty(), flatten_selection, include_project_ignored)),
                  ftxui::separator(), ftxui::text("Enter: create Ready Task  F4: previous  Esc: cancel")};
    }
    return ftxui::dbox({dashboard | ftxui::dim,
                        ftxui::window(ftxui::text("New task"), ftxui::vbox(std::move(contents))) |
                            ftxui::center});
  });
  root = ftxui::CatchEvent(root, [&](ftxui::Event event) {
    const bool dashboard_active = !creating && !settings_open && !delete_confirming && !scp_confirming && !help_open;
    if (help_open && event == ftxui::Event::Escape) {
      help_open = false;
      active_form = 0;
      return true;
    }
    if (dashboard_active && event == ftxui::Event::Character('?')) {
      help_open = true;
      active_form = 0;
      return true;
    }
    if (dashboard_active && event == ftxui::Event::Character('q')) {
      screen.ExitLoopClosure()();
      return true;
    }
    if (dashboard_active && event == ftxui::Event::Character('r')) {
      refresh();
      return true;
    }
    if (dashboard_active && event == ftxui::Event::Character('d')) {
      try {
        const auto path = state_dir / "rsyncd-deployment.conf";
        write_network_rsyncd_template(path);
        status = "Deployment template written (not started): " + std::filesystem::absolute(path).string();
      } catch (const std::exception& error) { status = error.what(); }
      return true;
    }
    if (dashboard_active && !tasks.empty() && event == ftxui::Event::Character('R')) {
      try {
        const auto source_endpoint = rsync_assistant::parse_endpoint(tasks.at(selected).source);
        const auto destination_endpoint = rsync_assistant::parse_endpoint(tasks.at(selected).destination);
        const auto endpoint = source_endpoint.remote ? source_endpoint : destination_endpoint;
        if (!endpoint.remote || endpoint.rsync_daemon)
          throw std::runtime_error("selected task has no SSH assistant endpoint");
        const auto remote_tasks = rsync_assistant::remote_assistant_tasks(endpoint);
        remote_task_status = "Remote " + endpoint.host + ":";
        if (remote_tasks.empty()) remote_task_status += " no tasks";
        for (const auto& remote : remote_tasks)
          remote_task_status += "\n" + remote.id + " " + remote.state + " via " + remote.method;
      } catch (const std::exception& error) { remote_task_status = error.what(); }
      return true;
    }
    if (dashboard_active && event == ftxui::Event::Character('n')) {
      creating = true;
      wizard_step = 0;
      active_form = 1;
      source.clear();
      destination.clear();
      selected_source_paths.clear();
      flatten_selection = false;
      copy_source_contents = false;
      create_destination_parents = false;
      delete_extraneous = false;
      compression = settings.compression;
      dry_run = settings.dry_run;
      trusted_daemon = false;
      source_has_git_repository = false;
      include_git_data = false;
      source_has_project_ignores = false;
      include_project_ignored = false;
      return true;
    }
    if (dashboard_active && event == ftxui::Event::Character('s')) { settings_open = true; active_form = 7; return true; }
    if (settings_open && event == ftxui::Event::CtrlS) {
      try {
        settings.benchmark_size_mib = static_cast<unsigned>(std::stoul(settings_benchmark_size));
        settings.benchmark_timeout_seconds = static_cast<unsigned>(std::stoul(settings_benchmark_timeout));
        settings.benchmark_cache_hours = static_cast<unsigned>(std::stoul(settings_benchmark_cache));
        settings.daemon_advantage_threshold = std::stod(settings_benchmark_threshold);
        settings.save(settings_path); status = "Settings saved: " + settings_path.string();
      }
      catch (const std::exception& error) { status = error.what(); }
      return true;
    }
    if (settings_open && event == ftxui::Event::Escape) { settings_open = false; active_form = 0; return true; }
    if (event == ftxui::Event::Escape && (delete_confirming || scp_confirming)) {
      delete_confirming = false;
      delete_confirmation.clear();
      scp_confirming = false;
      scp_confirmation.clear();
      active_form = creating ? wizard_step + 1 : 0;
      return true;
    }
    if (creating && !browsing && wizard_step <= 1 && event == ftxui::Event::F2) {
      browse_destination = wizard_step == 1;
      const auto endpoint = rsync_assistant::parse_endpoint(browse_destination ? destination : source);
      browse_selected = 0;
      try {
        if (endpoint.remote) {
          ++browse_scan_generation;
          browse_entries.clear();
          for (const auto& path : rsync_assistant::remote_ssh_list(endpoint)) {
            const bool directory = path.ends_with('/');
            browse_entries.push_back({directory ? path.substr(0, path.size() - 1) : path, directory, false});
          }
          browse_remote = true;
          browse_remote_prefix = endpoint.host + ":";
          browse_remote_host = endpoint.host;
          browse_remote_directory = endpoint.path;
          browse_root = endpoint.path;
          browse_selected_paths.clear();
        } else {
          const auto& value = browse_destination ? destination : source;
          browse_directory = value.empty() ? std::filesystem::current_path() : std::filesystem::path{value};
          browse_root = browse_directory;
          browse_selected_paths.clear();
          flatten_selection = false;
          browse_search = false;
          browse_query.clear();
          browse_include_ignored = false;
          browse_remote = false;
          scan_browser();
        }
        browsing = true;
        active_form = 0;
      } catch (const std::exception& error) { status = error.what(); }
      return true;
    }
    if (creating && !browsing && wizard_step <= 1 && event == ftxui::Event::F3) {
      browse_destination = wizard_step == 1;
      const auto endpoint = rsync_assistant::parse_endpoint(browse_destination ? destination : source);
      browse_selected = 0;
      try {
        if (endpoint.remote || (browse_destination ? destination : source).empty()) {
          ++browse_scan_generation;
          if (!endpoint.remote) {
            browse_remote = true;
            browse_remote_host.clear();
            browse_manual_host.clear();
            browse_manual_path.clear();
            browse_ssh_hosts = rsync_assistant::ssh_config_hosts();
            browse_entries.clear();
            browsing = true;
            active_form = 0;
            return true;
          }
          browse_entries.clear();
          for (const auto& path : rsync_assistant::remote_ssh_list(endpoint)) {
            const bool directory = path.ends_with('/');
            browse_entries.push_back({directory ? path.substr(0, path.size() - 1) : path, directory, false});
          }
          browse_remote = true;
          browse_remote_prefix = endpoint.host + ":";
          browse_remote_host = endpoint.host;
          browse_remote_directory = endpoint.path;
          browse_root = endpoint.path;
          browse_selected_paths.clear();
        } else {
          const auto& value = browse_destination ? destination : source;
          browse_directory = value.empty() ? std::filesystem::current_path() : std::filesystem::path{value};
          browse_remote = false;
          scan_browser();
        }
        browsing = true;
        active_form = 0;
      } catch (const std::exception& error) { status = error.what(); }
      return true;
    }
    if (browsing) {
      if (browse_remote && browse_remote_host.empty()) {
        if (event == ftxui::Event::Escape) { browsing = false; active_form = wizard_step + 1; return true; }
        if (event == ftxui::Event::Return) {
          if (browse_manual_host.empty() && !browse_ssh_hosts.empty()) browse_manual_host = browse_ssh_hosts.at(browse_selected);
          if (browse_manual_host.empty()) { status = "Choose or enter an SSH Host"; return true; }
          try {
            browse_remote_host = browse_manual_host;
            browse_remote_directory = rsync_assistant::remote_ssh_home(browse_remote_host);
            browse_root = browse_remote_directory;
            browse_remote_prefix = browse_remote_host + ":";
            browse_entries.clear();
            for (const auto& path : rsync_assistant::remote_ssh_list({true, false, browse_remote_host, browse_remote_directory.string()})) {
              const bool directory = path.ends_with('/');
              browse_entries.push_back({directory ? path.substr(0, path.size() - 1) : path, directory, false});
            }
            browse_selected = 0;
          } catch (const std::exception& error) { browse_remote_host.clear(); status = error.what(); }
          return true;
        }
        if ((event == ftxui::Event::Character('j') || event == ftxui::Event::ArrowDown) && browse_selected + 1 < static_cast<int>(browse_ssh_hosts.size())) { ++browse_selected; return true; }
        if ((event == ftxui::Event::Character('k') || event == ftxui::Event::ArrowUp) && browse_selected > 0) { --browse_selected; return true; }
        return browse_manual_host_input->OnEvent(event);
      }
      if (browse_remote && !browse_remote_host.empty() && browse_manual_path_input->OnEvent(event)) {
        return true;
      }
      if (browse_search) {
        if (event == ftxui::Event::Escape) { browse_search = false; browse_query.clear(); scan_browser(); return true; }
        if (event == ftxui::Event::Return) { browse_search = false; scan_browser(); return true; }
        if (browse_search_input->OnEvent(event)) { scan_browser(); return true; }
        return true;
      }
      if (event == ftxui::Event::Escape) { browsing = false; active_form = wizard_step + 1; return true; }
      if (!browse_remote && event == ftxui::Event::Character('/')) { browse_search = true; browse_query.clear(); return true; }
      if (event == ftxui::Event::Character('g')) { browse_hidden = !browse_hidden; browse_selected = 0; scan_browser(); return true; }
      if (event == ftxui::Event::Character('i')) { browse_include_ignored = !browse_include_ignored; browse_selected = 0; scan_browser(); return true; }
      if ((event == ftxui::Event::Character('j') || event == ftxui::Event::ArrowDown) && browse_selected + 1 < static_cast<int>(browse_entries.size())) { ++browse_selected; return true; }
      if ((event == ftxui::Event::Character('k') || event == ftxui::Event::ArrowUp) && browse_selected > 0) { --browse_selected; return true; }
      if (event == ftxui::Event::Character('h')) {
        if (!browse_remote) {
          browse_directory = browse_directory.parent_path();
          scan_browser();
        } else {
          browse_remote_directory = browse_remote_directory.parent_path();
          browse_entries.clear();
          for (const auto& path : rsync_assistant::remote_ssh_list({true, false, browse_remote_host, browse_remote_directory.string()})) {
            const bool directory = path.ends_with('/');
            browse_entries.push_back({directory ? path.substr(0, path.size() - 1) : path, directory, false});
          }
        }
        browse_selected = 0;
        return true;
      }
      if (!browse_entries.empty() && (event == ftxui::Event::Character('l'))) {
        if (browse_entries.at(browse_selected).directory) {
          if (!browse_remote) {
            browse_directory = browse_entries.at(browse_selected).path;
            scan_browser();
          } else {
            const auto next_directory = browse_entries.at(browse_selected).path;
            browse_entries.clear();
            browse_remote_directory = next_directory;
            for (const auto& path : rsync_assistant::remote_ssh_list({true, false, browse_remote_host, browse_remote_directory.string()})) {
              const bool directory = path.ends_with('/');
              browse_entries.push_back({directory ? path.substr(0, path.size() - 1) : path, directory, false});
            }
          }
          browse_selected = 0;
        }
        return true;
      }
      if (!browse_destination && !browse_entries.empty() && event == ftxui::Event::Character(' ')) {
        const auto path = browse_entries.at(browse_selected).path.string();
        if (browse_selected_paths.contains(path)) browse_selected_paths.erase(path);
        else browse_selected_paths.insert(path);
        return true;
      }
      if (!browse_destination && event == ftxui::Event::Character('F')) {
        flatten_selection = !flatten_selection;
        return true;
      }
      if (event == ftxui::Event::Return && browse_remote && !browse_remote_host.empty() && !browse_manual_path.empty()) {
        if (!browse_manual_path.starts_with('/')) { status = "Remote path must be absolute"; return true; }
        const auto selected_path = browse_remote_prefix + browse_manual_path;
        if (browse_destination) destination = selected_path;
        else source = selected_path;
        browsing = false;
        active_form = wizard_step + 1;
        return true;
      }
      if (!browse_entries.empty() && event == ftxui::Event::Return) {
        if (!browse_destination) {
          if (browse_selected_paths.empty()) {
            if (browse_remote) source = browse_remote_prefix + browse_entries.at(browse_selected).path.string();
            else { status = "Select one or more source entries with Space"; return true; }
          } else {
            const auto root = browse_remote ? browse_remote_directory : browse_root;
            source = (browse_remote ? browse_remote_prefix : "") + root.string() + "/";
            selected_source_paths.clear();
            for (const auto& selected_path : browse_selected_paths) {
              std::error_code error;
              const auto relative = std::filesystem::relative(selected_path, root, error);
              if (error || relative.empty() || relative.is_absolute()) {
                status = "Selected path is outside the source root";
                return true;
              }
              selected_source_paths.push_back(relative.generic_string());
            }
          }
        } else {
        if (!browse_entries.at(browse_selected).directory) { status = "Destination must be a directory"; return true; }
        const auto selected_path = (browse_remote ? browse_remote_prefix : "") + browse_entries.at(browse_selected).path.string();
        if (browse_destination) destination = selected_path;
        else source = selected_path;
        }
        browsing = false;
        active_form = wizard_step + 1;
        return true;
      }
      return true;
    }
    if (event == ftxui::Event::Escape && creating && !browsing) {
      creating = false;
      active_form = 0;
      return true;
    }
    if (creating && !browsing && event == ftxui::Event::F4 && wizard_step > 0) {
      --wizard_step;
      active_form = wizard_step + 1;
      return true;
    }
    if (event == ftxui::Event::Return) {
      try {
        if (delete_confirming) {
          if (delete_confirmation != "DELETE") throw std::runtime_error("type DELETE to confirm");
          (void)client.execute(tasks.at(selected).id, true);
          delete_confirmation.clear();
          delete_confirming = false;
          active_form = 0;
          refresh();
          return true;
        }
        if (scp_confirming) {
          if (scp_confirmation != "SCP") throw std::runtime_error("type SCP to confirm system scp fallback");
          (void)client.execute_scp_fallback(tasks.at(selected).id);
          scp_confirmation.clear();
          scp_confirming = false;
          active_form = 0;
          refresh();
          return true;
        }
        if (creating) {
          if (wizard_step == 0) {
            if (source.empty())
              throw std::runtime_error("a Source is required before continuing");
            const auto source_endpoint = rsync_assistant::parse_endpoint(source);
            const auto profile = !source_endpoint.remote ? rsync_assistant::detect_project_profile(source_endpoint.path) : rsync_assistant::ProjectProfile{};
            source_has_git_repository = profile.has_git_repository;
            source_has_project_ignores = !profile.exclusions.empty();
            include_git_data = false;
            include_project_ignored = false;
            ++wizard_step;
            active_form = wizard_step + 1;
            return true;
          }
          if (wizard_step == 1) {
            if (destination.empty())
              throw std::runtime_error("a Destination directory is required before continuing");
            ++wizard_step;
            active_form = wizard_step + 1;
            return true;
          }
          if (wizard_step == 2) {
            ++wizard_step;
            active_form = wizard_step + 1;
            return true;
          }
          auto task_source = source;
          if (copy_source_contents && selected_source_paths.empty() && !task_source.empty() && !task_source.ends_with('/')) task_source += '/';
          rsync_assistant::CreateReadyTask request{task_source, destination, delete_extraneous, compression, dry_run, trusted_daemon, {}, selected_source_paths, flatten_selection, include_git_data, include_project_ignored};
          request.create_destination_parents = create_destination_parents;
          (void)client.create_ready_task(request);
          source.clear();
          destination.clear();
          delete_extraneous = false;
          compression = settings.compression;
          dry_run = settings.dry_run;
          trusted_daemon = false;
          source_has_git_repository = false;
          include_git_data = false;
          source_has_project_ignores = false;
          include_project_ignored = false;
          selected_source_paths.clear();
          flatten_selection = false;
          copy_source_contents = false;
          create_destination_parents = false;
          creating = false;
          wizard_step = 0;
          active_form = 0;
          refresh();
        } else if (!tasks.empty()) {
          const auto& task = tasks.at(selected);
          if (task.state == rsync_assistant::TaskState::ready) (void)client.preflight(task.id);
          if (task.state == rsync_assistant::TaskState::awaiting_execution_confirmation) {
            if (task.delete_extraneous) delete_confirming = true;
            else (void)client.execute(task.id);
            if (delete_confirming) active_form = 5;
          }
          refresh();
        }
      } catch (const std::exception& error) {
        status = error.what();
      }
      return true;
    }
    if (dashboard_active && !tasks.empty() && event == ftxui::Event::Character('p')) {
      try {
        const auto& task = tasks.at(selected);
        if (task.method == rsync_assistant::TransferMethod::scp)
          throw std::runtime_error("scp tasks do not offer resumable pause controls");
        if (task.state == rsync_assistant::TaskState::running) (void)client.pause(task.id);
        if (task.state == rsync_assistant::TaskState::paused) (void)client.resume(task.id);
        refresh();
      } catch (const std::exception& error) { status = error.what(); }
      return true;
    }
    if (dashboard_active && !tasks.empty() && event == ftxui::Event::Character('x')) {
      try { (void)client.stop(tasks.at(selected).id); refresh(); }
      catch (const std::exception& error) { status = error.what(); }
      return true;
    }
    if (dashboard_active && !tasks.empty() && event == ftxui::Event::Character('w')) {
      try { (void)client.await_completion(tasks.at(selected).id); refresh(); }
      catch (const std::exception& error) { status = error.what(); }
      return true;
    }
    if (dashboard_active && !tasks.empty() && event == ftxui::Event::Character('e')) {
      try {
        if (tasks.at(selected).method == rsync_assistant::TransferMethod::scp)
          throw std::runtime_error("scp tasks cannot claim rsync restart/resume behavior");
        (void)client.restart(tasks.at(selected).id); refresh();
      }
      catch (const std::exception& error) { status = error.what(); }
      return true;
    }
    if (dashboard_active && !tasks.empty() && event == ftxui::Event::Character('c')) {
      try {
        if (tasks.at(selected).state != rsync_assistant::TaskState::failed)
          throw std::runtime_error("scp fallback is available only after rsync failure");
        scp_confirming = true;
        active_form = 6;
      }
      catch (const std::exception& error) { status = error.what(); }
      return true;
    }
    if (dashboard_active && event == ftxui::Event::ArrowDown && selected + 1 < static_cast<int>(tasks.size())) {
      ++selected;
      refresh();
      return true;
    }
    if (dashboard_active && event == ftxui::Event::ArrowUp && selected > 0) {
      --selected;
      refresh();
      return true;
    }
    return false;
  });
  screen.Loop(root);
  return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
  try {
    const auto option_value = [&](std::string_view option) -> const char* {
      for (int index = 1; index + 1 < argc; ++index)
        if (std::string_view{argv[index]} == option) return argv[index + 1];
      return nullptr;
    };
    const auto has_argument = [&](std::string_view argument) {
      for (int index = 1; index < argc; ++index)
        if (std::string_view{argv[index]} == argument) return true;
      return false;
    };
    const auto state_dir = state_directory(argc, argv);
    const std::filesystem::path settings_path = option_value("--config") ?
        std::filesystem::absolute(option_value("--config")) : state_dir / "settings.toml";
    rsync_assistant::Settings settings;
    if (std::filesystem::exists(settings_path)) settings = rsync_assistant::Settings::load(settings_path);
    if (has_argument("--control-ping")) {
      std::cout << "rsync-assistant-control-v1\n";
      return 0;
    }
    if (has_argument("--control-tasks")) {
      const auto tasks = rsync_assistant::TaskControlSocketClient{state_dir / "control.sock"}.list_tasks();
      const auto state_name = [](rsync_assistant::TaskState state) {
        if (state == rsync_assistant::TaskState::ready) return "ready";
        if (state == rsync_assistant::TaskState::preflighting) return "preflighting";
        if (state == rsync_assistant::TaskState::awaiting_execution_confirmation) return "awaiting_confirmation";
        if (state == rsync_assistant::TaskState::running) return "running";
        if (state == rsync_assistant::TaskState::paused) return "paused";
        if (state == rsync_assistant::TaskState::completed) return "completed";
        if (state == rsync_assistant::TaskState::cancelled) return "cancelled";
        if (state == rsync_assistant::TaskState::interrupted) return "interrupted";
        return "failed";
      };
      const auto method_name = [](rsync_assistant::TransferMethod method) {
        if (method == rsync_assistant::TransferMethod::rsync_daemon) return "rsync_daemon";
        if (method == rsync_assistant::TransferMethod::rsync_ssh) return "rsync_ssh";
        if (method == rsync_assistant::TransferMethod::scp) return "scp";
        return "local_rsync";
      };
      for (const auto& task : tasks)
        std::cout << task.id << '\t' << state_name(task.state) << '\t' << method_name(task.method) << '\n';
      return 0;
    }
    if (const auto* token = option_value("--control-benchmark-root")) {
      if (!valid_benchmark_token(token)) throw std::runtime_error("invalid benchmark token");
      const auto root = state_dir / "benchmarks" / token;
      std::filesystem::create_directories(root);
      std::filesystem::permissions(root, std::filesystem::perms::owner_all,
                                   std::filesystem::perm_options::replace);
      std::cout << root.string() << '\n';
      return 0;
    }
    if (const auto* token = option_value("--control-benchmark-clean")) {
      if (!valid_benchmark_token(token)) throw std::runtime_error("invalid benchmark token");
      const auto root = state_dir / "benchmarks" / token;
      std::error_code error;
      std::filesystem::remove_all(root, error);
      if (error) throw std::runtime_error("cannot clean benchmark root: " + error.message());
      return 0;
    }
    if (const auto* path = option_value("--control-list")) {
      for (const auto& entry : std::filesystem::directory_iterator(
               path, std::filesystem::directory_options::skip_permission_denied)) {
        std::cout << entry.path().string();
        if (entry.is_directory()) std::cout << '/';
        std::cout << '\n';
      }
      return 0;
    }
    if (const auto* requested_path = option_value("--write-default-config")) {
      const auto path = std::filesystem::absolute(requested_path);
      std::filesystem::create_directories(path.parent_path());
      std::ofstream output{path};
      output << "# rsync-assistant local settings\n"
             << "# Keep this file owner-readable only when api_key is set.\n\n"
             << "[transfer]\n"
             << "dry_run = true\n"
             << "compression = false\n"
             << "benchmark_enabled = true\n\n"
             << "[benchmark]\n"
             << "# Temporary benchmark payload size; it is never written into user source directories.\n"
             << "size_mib = 64\n"
             << "timeout_seconds = 15\n"
             << "cache_hours = 24\n"
             << "# Direct daemon must be at least this multiple faster than SSH.\n"
             << "daemon_advantage_threshold = 1.1\n\n"
             << "[ai]\n"
             << "enabled = false\n"
             << "endpoint = \"\"\n"
             << "model = \"\"\n"
             << "api_key = \"\"\n";
      if (!output) throw std::runtime_error("cannot write configuration");
      std::cout << path << '\n';
      return 0;
    }
    if (const auto* requested_path = option_value("--write-rsyncd-config")) {
      const auto path = std::filesystem::absolute(requested_path);
      write_network_rsyncd_template(path);
      std::cout << path << '\n';
      return 0;
    }
    if (has_argument("daemon")) return run_daemon(state_dir, settings);
    if (has_argument("tui")) return run_tui(state_dir, settings, settings_path);

    try {
      (void)rsync_assistant::TaskControlSocketClient{state_dir / "control.sock"}.list_tasks();
    } catch (...) {
      const pid_t child = fork();
      if (child == 0) {
        setsid();
        _exit(run_daemon(state_dir, settings));
      }
      if (child < 0) throw std::runtime_error("cannot start daemon");
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return run_tui(state_dir, settings, settings_path);
  } catch (const std::exception& error) {
    std::cerr << "rsync-assistant: " << error.what() << '\n';
    return 1;
  }
}
