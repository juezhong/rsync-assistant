#include "rsync_assistant/task_control_socket.hpp"
#include "rsync_assistant/directory_scanner.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>

namespace {
std::filesystem::path state_directory(int argc, char* argv[]) {
  if (argc >= 3 && std::string_view{argv[argc - 2]} == "--state-dir") return argv[argc - 1];
  return std::filesystem::temp_directory_path() / "rsync-assistant";
}

int run_daemon(const std::filesystem::path& state_dir) {
  std::filesystem::create_directories(state_dir);
  rsync_assistant::TaskControlService service{state_dir / "tasks.sqlite3"};
  rsync_assistant::TaskControlSocketServer server{service, state_dir / "control.sock"};
  server.serve();
  return 0;
}

int run_tui(const std::filesystem::path& state_dir) {
  rsync_assistant::TaskControlSocketClient client{state_dir / "control.sock"};
  auto screen = ftxui::ScreenInteractive::Fullscreen();
  std::string status = "r: refresh  q: quit";
  std::string selected_log;
  std::vector<rsync_assistant::TransferTask> tasks;
  int selected = 0;
  bool creating = false;
  std::string source;
  std::string destination;
  bool delete_extraneous = false;
  bool compression = false;
  bool browsing = false;
  std::filesystem::path browse_directory = std::filesystem::current_path();
  std::vector<rsync_assistant::PathEntry> browse_entries;
  int browse_selected = 0;
  bool browse_hidden = false;
  auto source_input = ftxui::Input(&source, "Source path");
  auto destination_input = ftxui::Input(&destination, "Destination path");
  auto delete_checkbox = ftxui::Checkbox("Delete destination-only files (--delete)", &delete_extraneous);
  auto compression_checkbox = ftxui::Checkbox("Compress transfer (--compress)", &compression);
  auto form = ftxui::Container::Vertical({source_input, destination_input, compression_checkbox, delete_checkbox});
  auto scan_browser = [&] {
    browse_entries = rsync_assistant::scan_directory_level(browse_directory, browse_hidden);
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
        return "Failed";
      };
      for (std::size_t index = 0; index < tasks.size(); ++index)
        task_lines += (static_cast<int>(index) == selected ? "> " : "  ") +
                      std::string{label(tasks[index].state)} + " " + tasks[index].id + "\n";
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
                     ftxui::text("c: explicit scp fallback after rsync failure"),
                     ftxui::text("n: new task"), ftxui::text("?: help")}) |
            ftxui::border | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 28),
    });
    if (!creating) return dashboard;
    if (browsing) {
      std::string entries = "h: parent  l: enter  g: hidden  Space/Enter: select\n\n";
      entries += browse_directory.string() + "\n";
      for (std::size_t index = 0; index < browse_entries.size(); ++index)
        entries += (static_cast<int>(index) == browse_selected ? "> " : "  ") +
                   browse_entries[index].path.filename().string() +
                   (browse_entries[index].directory ? "/\n" : "\n");
      return ftxui::dbox({dashboard | ftxui::dim,
                          ftxui::window(ftxui::text("Select source path"),
                                        ftxui::paragraph(entries)) | ftxui::center});
    }
    return ftxui::dbox({dashboard | ftxui::dim,
                        ftxui::window(ftxui::text("New task"),
                                      ftxui::vbox({source_input->Render(),
                                                   destination_input->Render(),
                                                   compression_checkbox->Render(),
                                                   delete_checkbox->Render(),
                                                   ftxui::separator(),
                                                   ftxui::text("Enter: create  Esc: cancel")})) |
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
    if (event == ftxui::Event::Character('n')) {
      creating = true;
      return true;
    }
    if (creating && event == ftxui::Event::Character('b')) {
      browse_directory = source.empty() ? std::filesystem::current_path() : std::filesystem::path{source};
      browse_selected = 0;
      try { scan_browser(); browsing = true; } catch (const std::exception& error) { status = error.what(); }
      return true;
    }
    if (browsing) {
      if (event == ftxui::Event::Escape) { browsing = false; return true; }
      if (event == ftxui::Event::Character('g')) { browse_hidden = !browse_hidden; browse_selected = 0; scan_browser(); return true; }
      if ((event == ftxui::Event::Character('j') || event == ftxui::Event::ArrowDown) && browse_selected + 1 < static_cast<int>(browse_entries.size())) { ++browse_selected; return true; }
      if ((event == ftxui::Event::Character('k') || event == ftxui::Event::ArrowUp) && browse_selected > 0) { --browse_selected; return true; }
      if (event == ftxui::Event::Character('h')) { browse_directory = browse_directory.parent_path(); browse_selected = 0; scan_browser(); return true; }
      if (!browse_entries.empty() && (event == ftxui::Event::Character('l'))) {
        if (browse_entries.at(browse_selected).directory) { browse_directory = browse_entries.at(browse_selected).path; browse_selected = 0; scan_browser(); }
        return true;
      }
      if (!browse_entries.empty() && (event == ftxui::Event::Return || event == ftxui::Event::Character(' '))) {
        source = browse_entries.at(browse_selected).path.string();
        browsing = false;
        return true;
      }
      return true;
    }
    if (event == ftxui::Event::Escape && creating && !browsing) {
      creating = false;
      return true;
    }
    if (event == ftxui::Event::Return) {
      try {
        if (creating) {
          (void)client.create_ready_task({source, destination, delete_extraneous, compression});
          source.clear();
          destination.clear();
          delete_extraneous = false;
          compression = false;
          creating = false;
          refresh();
        } else if (!tasks.empty()) {
          const auto& task = tasks.at(selected);
          if (task.state == rsync_assistant::TaskState::ready) (void)client.preflight(task.id);
          if (task.state == rsync_assistant::TaskState::awaiting_execution_confirmation) (void)client.execute(task.id);
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
    if (!creating && !tasks.empty() && event == ftxui::Event::Character('c')) {
      try { (void)client.execute_scp_fallback(tasks.at(selected).id); refresh(); }
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
    if (argc == 3 && std::string_view{argv[1]} == "--write-default-config") {
      const auto path = std::filesystem::absolute(argv[2]);
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
    if (argc == 3 && std::string_view{argv[1]} == "--write-rsyncd-config") {
      const auto path = std::filesystem::absolute(argv[2]);
      std::filesystem::create_directories(path.parent_path());
      std::ofstream output{path};
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
      std::cout << path << '\n';
      return 0;
    }
    const auto state_dir = state_directory(argc, argv);
    const std::string_view mode = argc >= 2 ? argv[1] : "";
    if (mode == "daemon") return run_daemon(state_dir);
    if (mode == "tui") return run_tui(state_dir);

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
    return run_tui(state_dir);
  } catch (const std::exception& error) {
    std::cerr << "rsync-assistant: " << error.what() << '\n';
    return 1;
  }
}
