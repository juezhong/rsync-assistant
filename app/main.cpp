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
#include <cerrno>
#include <exception>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
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

int run_daemon(const std::filesystem::path& state_dir) {
  std::filesystem::create_directories(state_dir);
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
  rsync_assistant::TaskControlService service{state_dir / "tasks.sqlite3"};
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
  std::vector<rsync_assistant::TransferTask> tasks;
  int selected = 0;
  bool creating = false;
  int wizard_step = 0;
  std::string source;
  std::string destination;
  std::vector<std::string> selected_source_paths;
  bool flatten_selection = false;
  bool delete_extraneous = false;
  bool compression = settings.compression;
  bool dry_run = settings.dry_run;
  bool trusted_daemon = false;
  bool source_has_git_repository = false;
  bool include_git_data = false;
  bool delete_confirming = false;
  std::string delete_confirmation;
  bool scp_confirming = false;
  bool settings_open = false;
  std::string scp_confirmation;
  bool browsing = false;
  std::filesystem::path browse_directory = std::filesystem::current_path();
  std::vector<rsync_assistant::PathEntry> browse_entries;
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
  std::filesystem::path browse_remote_directory;
  auto source_input = ftxui::Input(&source, "Source path");
  auto destination_input = ftxui::Input(&destination, "Destination path");
  auto delete_checkbox = ftxui::Checkbox("Delete destination-only files (--delete)", &delete_extraneous);
  auto compression_checkbox = ftxui::Checkbox("Compress transfer (--compress)", &compression);
  auto dry_run_checkbox = ftxui::Checkbox("Dry-run before execution", &dry_run);
  auto trusted_daemon_checkbox = ftxui::Checkbox("Trust direct rsync daemon on LAN", &trusted_daemon);
  auto include_git_checkbox = ftxui::Checkbox("Include repository data (.git)", &include_git_data);
  auto browse_search_input = ftxui::Input(&browse_query, "Search paths");
  auto delete_confirmation_input = ftxui::Input(&delete_confirmation, "Type DELETE");
  auto scp_confirmation_input = ftxui::Input(&scp_confirmation, "Type SCP");
  auto settings_dry_run = ftxui::Checkbox("Default dry-run", &settings.dry_run);
  auto settings_compression = ftxui::Checkbox("Default compression", &settings.compression);
  auto settings_benchmark = ftxui::Checkbox("Enable benchmarks", &settings.benchmark_enabled);
  auto settings_ai_enabled = ftxui::Checkbox("Enable AI explanations", &settings.ai_enabled);
  auto settings_ai_endpoint = ftxui::Input(&settings.ai_endpoint, "AI endpoint");
  auto settings_ai_model = ftxui::Input(&settings.ai_model, "AI model");
  auto settings_api_key = ftxui::Input(&settings.api_key, "API key");
  auto form = ftxui::Container::Vertical({source_input, destination_input, dry_run_checkbox, compression_checkbox, delete_checkbox, trusted_daemon_checkbox, include_git_checkbox, browse_search_input, delete_confirmation_input, settings_dry_run, settings_compression, settings_benchmark, settings_ai_enabled, settings_ai_endpoint, settings_ai_model, settings_api_key});
  auto scan_browser = [&] {
    if (browse_search && !browse_query.empty()) {
      browse_entries.clear();
      for (const auto& path : rsync_assistant::search_paths(browse_root, browse_query, browse_hidden))
        browse_entries.push_back({path, std::filesystem::is_directory(path), false});
    } else {
      browse_entries = rsync_assistant::scan_directory_level(browse_directory, browse_hidden);
    }
    if (!browse_include_ignored && !browse_remote) {
      const auto profile = rsync_assistant::detect_project_profile(browse_root);
      browse_entries.erase(std::remove_if(browse_entries.begin(), browse_entries.end(), [&](const auto& entry) {
        std::error_code error;
        const auto relative = std::filesystem::relative(entry.path, browse_root, error).generic_string();
        if (error) return false;
        if (relative == ".git" || relative.starts_with(".git/")) return true;
        for (const auto& exclusion : profile.exclusions) {
          const auto prefix = exclusion.ends_with('/') ? exclusion.substr(0, exclusion.size() - 1) : exclusion;
          if (relative == prefix || relative.starts_with(prefix + "/")) return true;
        }
        return false;
      }), browse_entries.end());
    }
    if (browse_selected >= static_cast<int>(browse_entries.size())) browse_selected = 0;
  };
  auto refresh = [&] {
    try {
      tasks = client.list_tasks();
      if (selected >= static_cast<int>(tasks.size())) selected = 0;
      selected_log = tasks.empty() ? "No selected task" : client.execution_log(tasks.at(selected).id);
      std::string task_lines;
      const auto label = [](rsync_assistant::TaskState state) {
        if (state == rsync_assistant::TaskState::ready) return "Ready";
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
                      std::string{label(tasks[index].state)} + " " + tasks[index].id + "\n";
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
        ftxui::vbox({ftxui::text("Details / shortcuts") | ftxui::bold,
                     ftxui::separator(), ftxui::text("Enter: preflight/execute"),
                     ftxui::text("p: pause/resume  x: stop  w: wait"),
                     ftxui::text("e: re-prepare interrupted/failed rsync task"),
                     ftxui::text("c: explicit scp fallback after rsync failure"),
                     ftxui::text("n: new task  s: settings  d: daemon deployment guide"), ftxui::text("?: help")}) |
            ftxui::border | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 28),
    });
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
                                                     settings_benchmark->Render(), settings_ai_enabled->Render(),
                                                     settings_ai_endpoint->Render(), settings_ai_model->Render(),
                                                     settings_api_key->Render(), ftxui::separator(),
                                                     ftxui::text("Ctrl-S: save  Esc: close"),
                                                     ftxui::text(settings_path.string())})) | ftxui::center});
    }
    if (!creating) return dashboard;
    if (browsing) {
      std::string entries = browse_destination ? "h: parent  l: enter  /: search  g: hidden  i: ignored  Enter: select\n\n" :
                                                 "h: parent  l: enter  /: search  g: hidden  i: ignored  Space: toggle  F: flatten  Enter: confirm\n\n";
      if (browse_search) entries += "Search: " + browse_query + " (Enter: apply, Esc: cancel)\n";
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
                                        ftxui::paragraph(entries)) | ftxui::center});
    }
    ftxui::Elements contents;
    if (wizard_step == 0) {
      contents = {ftxui::text("Step 1/3: endpoints"), source_input->Render(),
                  destination_input->Render(), ftxui::separator(),
                  ftxui::text("F2: source picker  F3: destination picker"),
                  ftxui::text("Enter: next  Esc: cancel")};
    } else if (wizard_step == 1) {
      contents = {ftxui::text("Step 2/3: transfer options"), dry_run_checkbox->Render(),
                  compression_checkbox->Render(), delete_checkbox->Render(), trusted_daemon_checkbox->Render(),
                  source_has_git_repository ? include_git_checkbox->Render() : ftxui::text("No .git repository detected at source root"), ftxui::separator(),
                  ftxui::text("Enter: review  F4: previous  Esc: cancel")};
    } else {
      contents = {ftxui::text("Step 3/3: review"),
                  ftxui::text("Source: " + source), ftxui::text("Destination: " + destination),
                  ftxui::text(std::string{"Dry-run: "} + (dry_run ? "enabled" : "disabled")),
                  ftxui::text(std::string{"Compression: "} + (compression ? "enabled" : "disabled")),
                  ftxui::text(std::string{"Deletion: "} + (delete_extraneous ? "enabled" : "disabled")),
                  ftxui::text(selected_source_paths.empty() ? "Selection: whole source" :
                              "Selection: " + std::to_string(selected_source_paths.size()) + " entries (" + (flatten_selection ? "flatten" : "preserve paths") + ")"),
                  ftxui::separator(), ftxui::text("Enter: create Ready Task  F4: previous  Esc: cancel")};
    }
    return ftxui::dbox({dashboard | ftxui::dim,
                        ftxui::window(ftxui::text("New task"), ftxui::vbox(std::move(contents))) |
                            ftxui::center});
  });
  root = ftxui::CatchEvent(root, [&](ftxui::Event event) {
    if (event == ftxui::Event::Character('q')) {
      screen.ExitLoopClosure()();
      return true;
    }
    if (event == ftxui::Event::Character('r')) {
      refresh();
      return true;
    }
    if (!creating && event == ftxui::Event::Character('d')) {
      try {
        const auto path = state_dir / "rsyncd-deployment.conf";
        write_network_rsyncd_template(path);
        status = "Deployment template written (not started): " + std::filesystem::absolute(path).string();
      } catch (const std::exception& error) { status = error.what(); }
      return true;
    }
    if (event == ftxui::Event::Character('n')) {
      creating = true;
      wizard_step = 0;
      return true;
    }
    if (!creating && event == ftxui::Event::Character('s')) { settings_open = true; return true; }
    if (settings_open && event == ftxui::Event::CtrlS) {
      try { settings.save(settings_path); status = "Settings saved: " + settings_path.string(); }
      catch (const std::exception& error) { status = error.what(); }
      return true;
    }
    if (settings_open && event == ftxui::Event::Escape) { settings_open = false; return true; }
    if (event == ftxui::Event::Escape && (delete_confirming || scp_confirming)) {
      delete_confirming = false;
      delete_confirmation.clear();
      scp_confirming = false;
      scp_confirmation.clear();
      return true;
    }
    if (creating && wizard_step == 0 && event == ftxui::Event::F2) {
      browse_destination = false;
      const auto endpoint = rsync_assistant::parse_endpoint(source);
      browse_selected = 0;
      try {
        if (endpoint.remote) {
          if (!rsync_assistant::remote_assistant_available(endpoint)) throw std::runtime_error("remote assistant is unavailable; enter remote path manually");
          browse_entries.clear();
          for (const auto& path : rsync_assistant::remote_assistant_list(endpoint)) {
            const bool directory = path.ends_with('/');
            browse_entries.push_back({directory ? path.substr(0, path.size() - 1) : path, directory, false});
          }
          browse_remote = true;
          browse_remote_prefix = endpoint.host + ":";
          browse_remote_host = endpoint.host;
          browse_remote_directory = endpoint.path;
        } else {
          browse_directory = source.empty() ? std::filesystem::current_path() : std::filesystem::path{source};
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
      } catch (const std::exception& error) { status = error.what(); }
      return true;
    }
    if (creating && wizard_step == 0 && event == ftxui::Event::F3) {
      browse_destination = true;
      const auto endpoint = rsync_assistant::parse_endpoint(destination);
      browse_selected = 0;
      try {
        if (endpoint.remote) {
          if (!rsync_assistant::remote_assistant_available(endpoint)) throw std::runtime_error("remote assistant is unavailable; enter remote path manually");
          browse_entries.clear();
          for (const auto& path : rsync_assistant::remote_assistant_list(endpoint)) {
            const bool directory = path.ends_with('/');
            browse_entries.push_back({directory ? path.substr(0, path.size() - 1) : path, directory, false});
          }
          browse_remote = true;
          browse_remote_prefix = endpoint.host + ":";
          browse_remote_host = endpoint.host;
          browse_remote_directory = endpoint.path;
        } else {
          browse_directory = destination.empty() ? std::filesystem::current_path() : std::filesystem::path{destination};
          browse_remote = false;
          scan_browser();
        }
        browsing = true;
      } catch (const std::exception& error) { status = error.what(); }
      return true;
    }
    if (browsing) {
      if (browse_search) {
        if (event == ftxui::Event::Escape) { browse_search = false; browse_query.clear(); scan_browser(); return true; }
        if (event == ftxui::Event::Return) { browse_search = false; scan_browser(); return true; }
        if (browse_search_input->OnEvent(event)) { scan_browser(); return true; }
        return true;
      }
      if (event == ftxui::Event::Escape) { browsing = false; return true; }
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
          for (const auto& path : rsync_assistant::remote_assistant_list({true, false, browse_remote_host, browse_remote_directory.string()})) {
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
            for (const auto& path : rsync_assistant::remote_assistant_list({true, false, browse_remote_host, browse_remote_directory.string()})) {
              const bool directory = path.ends_with('/');
              browse_entries.push_back({directory ? path.substr(0, path.size() - 1) : path, directory, false});
            }
          }
          browse_selected = 0;
        }
        return true;
      }
      if (!browse_destination && !browse_remote && !browse_entries.empty() && event == ftxui::Event::Character(' ')) {
        const auto path = browse_entries.at(browse_selected).path.string();
        if (browse_selected_paths.contains(path)) browse_selected_paths.erase(path);
        else browse_selected_paths.insert(path);
        return true;
      }
      if (!browse_destination && !browse_remote && event == ftxui::Event::Character('F')) {
        flatten_selection = !flatten_selection;
        return true;
      }
      if (!browse_entries.empty() && event == ftxui::Event::Return) {
        if (!browse_destination && !browse_remote) {
          if (browse_selected_paths.empty()) {
            status = "Select one or more source entries with Space";
            return true;
          }
          source = browse_root.string() + "/";
          selected_source_paths.clear();
          for (const auto& selected_path : browse_selected_paths) {
            std::error_code error;
            const auto relative = std::filesystem::relative(selected_path, browse_root, error);
            if (error || relative.empty() || relative.is_absolute()) {
              status = "Selected path is outside the source root";
              return true;
            }
            selected_source_paths.push_back(relative.generic_string());
          }
        } else {
        const auto selected_path = (browse_remote ? browse_remote_prefix : "") + browse_entries.at(browse_selected).path.string();
        if (browse_destination) destination = selected_path;
        else source = selected_path;
        }
        browsing = false;
        return true;
      }
      return true;
    }
    if (event == ftxui::Event::Escape && creating && !browsing) {
      creating = false;
      return true;
    }
    if (creating && !browsing && event == ftxui::Event::F4 && wizard_step > 0) {
      --wizard_step;
      return true;
    }
    if (event == ftxui::Event::Return) {
      try {
        if (delete_confirming) {
          if (delete_confirmation != "DELETE") throw std::runtime_error("type DELETE to confirm");
          (void)client.execute(tasks.at(selected).id, true);
          delete_confirmation.clear();
          delete_confirming = false;
          refresh();
          return true;
        }
        if (scp_confirming) {
          if (scp_confirmation != "SCP") throw std::runtime_error("type SCP to confirm system scp fallback");
          (void)client.execute_scp_fallback(tasks.at(selected).id);
          scp_confirmation.clear();
          scp_confirming = false;
          refresh();
          return true;
        }
        if (creating) {
          if (wizard_step == 0) {
            if (source.empty() || destination.empty())
              throw std::runtime_error("source and destination are required before continuing");
            const auto source_endpoint = rsync_assistant::parse_endpoint(source);
            source_has_git_repository = !source_endpoint.remote &&
                rsync_assistant::detect_project_profile(source_endpoint.path).has_git_repository;
            include_git_data = false;
            ++wizard_step;
            return true;
          }
          if (wizard_step == 1) {
            ++wizard_step;
            return true;
          }
          (void)client.create_ready_task({source, destination, delete_extraneous, compression, dry_run, trusted_daemon, selected_source_paths, flatten_selection, include_git_data});
          source.clear();
          destination.clear();
          delete_extraneous = false;
          compression = settings.compression;
          dry_run = settings.dry_run;
          trusted_daemon = false;
          source_has_git_repository = false;
          include_git_data = false;
          selected_source_paths.clear();
          flatten_selection = false;
          creating = false;
          wizard_step = 0;
          refresh();
        } else if (!tasks.empty()) {
          const auto& task = tasks.at(selected);
          if (task.state == rsync_assistant::TaskState::ready) (void)client.preflight(task.id);
          if (task.state == rsync_assistant::TaskState::awaiting_execution_confirmation) {
            if (task.delete_extraneous) delete_confirming = true;
            else (void)client.execute(task.id);
          }
          refresh();
        }
      } catch (const std::exception& error) {
        status = error.what();
      }
      return true;
    }
    if (!creating && !tasks.empty() && event == ftxui::Event::Character('p')) {
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
    if (!creating && !tasks.empty() && event == ftxui::Event::Character('x')) {
      try { (void)client.stop(tasks.at(selected).id); refresh(); }
      catch (const std::exception& error) { status = error.what(); }
      return true;
    }
    if (!creating && !tasks.empty() && event == ftxui::Event::Character('w')) {
      try { (void)client.await_completion(tasks.at(selected).id); refresh(); }
      catch (const std::exception& error) { status = error.what(); }
      return true;
    }
    if (!creating && !tasks.empty() && event == ftxui::Event::Character('e')) {
      try {
        if (tasks.at(selected).method == rsync_assistant::TransferMethod::scp)
          throw std::runtime_error("scp tasks cannot claim rsync restart/resume behavior");
        (void)client.restart(tasks.at(selected).id); refresh();
      }
      catch (const std::exception& error) { status = error.what(); }
      return true;
    }
    if (!creating && !tasks.empty() && event == ftxui::Event::Character('c')) {
      try {
        if (tasks.at(selected).state != rsync_assistant::TaskState::failed)
          throw std::runtime_error("scp fallback is available only after rsync failure");
        scp_confirming = true;
      }
      catch (const std::exception& error) { status = error.what(); }
      return true;
    }
    if (!creating && event == ftxui::Event::ArrowDown && selected + 1 < static_cast<int>(tasks.size())) {
      ++selected;
      refresh();
      return true;
    }
    if (!creating && event == ftxui::Event::ArrowUp && selected > 0) {
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
    if (has_argument("daemon")) return run_daemon(state_dir);
    if (has_argument("tui")) return run_tui(state_dir, settings, settings_path);

    try {
      (void)rsync_assistant::TaskControlSocketClient{state_dir / "control.sock"}.list_tasks();
    } catch (...) {
      const pid_t child = fork();
      if (child == 0) {
        setsid();
        _exit(run_daemon(state_dir));
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
